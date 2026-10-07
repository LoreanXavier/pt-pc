#include "engine/render/upscale/upscale.h"

#if defined(PT_WITH_FSR)

#include <windows.h>

#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_api_loader.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>

#include <cfloat>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include "engine/core/log.h"
#include "engine/render/upscale/upscale_platform.h"

namespace pt {
namespace {

void Message(uint32_t type, const wchar_t* message) {
    const std::string text = NarrowText(message);
    if (type == FFX_API_MESSAGE_TYPE_ERROR) {
        LogError("fsr: {}", text);
    } else {
        LogWarn("fsr: {}", text);
    }
}

FfxApiResource Resource(const UpscaleImage& image, uint32_t state, bool depth, bool storage) {
    FfxApiResource resource{};
    if (!image.Valid()) {
        return resource;
    }
    resource.resource = reinterpret_cast<void*>(image.image);
    resource.state = state;
    FfxApiResourceDescription& d = resource.description;
    d.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    d.format = ffxApiGetSurfaceFormatVK(image.format);
    d.width = image.extent.width;
    d.height = image.extent.height;
    d.depth = 1;
    d.mipCount = 1;
    d.flags = FFX_API_RESOURCE_FLAGS_NONE;
    d.usage = FFX_API_RESOURCE_USAGE_READ_ONLY | (depth ? FFX_API_RESOURCE_USAGE_DEPTHTARGET : 0u) | (storage ? FFX_API_RESOURCE_USAGE_UAV : 0u);
    return resource;
}

/* FSR 3 and FSR 4 are two providers of one FidelityFX API, but AMD ships FSR 4 only in its DX12 DLLs; on Vulkan the FSR 4 path can only say why it is missing. */
class FsrBackend final : public UpscaleBackend {
public:
    FsrBackend(vk::Context& ctx, int generation) : ctx_(ctx), generation_(generation) {}
    ~FsrBackend() override {
        Release();
        if (module_) {
            FreeLibrary(module_);
        }
    }

    UpscalerKind Kind() const override { return generation_ == 4 ? UpscalerKind::Fsr4 : UpscalerKind::Fsr; }

    bool Supported(std::string& reason) override {
        if (!std::filesystem::exists(ExecutableDir() / L"amd_fidelityfx_vk.dll")) {
            reason = "amd_fidelityfx_vk.dll is missing next to pt.exe";
            return false;
        }
        if (generation_ == 4 && !probed_) {
            std::string probe;
            if (!Available(probe)) {
                reason = probe;
                return false;
            }
        } else if (generation_ == 4 && !version_id_) {
            reason = reason_;
            return false;
        }
        reason.clear();
        return true;
    }

    bool Available(std::string& reason) override {
        if (probed_) {
            reason = reason_;
            return version_id_ != 0;
        }
        probed_ = true;
        const bool ok = Probe(reason_);
        reason = reason_;
        return ok;
    }

    bool Probe(std::string& reason) {
        const std::filesystem::path path = ExecutableDir() / L"amd_fidelityfx_vk.dll";
        module_ = module_ ? module_ : LoadLibraryW(path.c_str());
        if (!module_) {
            reason = "amd_fidelityfx_vk.dll is missing next to pt.exe";
            return false;
        }
        ffxLoadFunctions(&fn_, module_);
        if (!fn_.CreateContext || !fn_.DestroyContext || !fn_.Dispatch || !fn_.Query || !fn_.Configure) {
            reason = "amd_fidelityfx_vk.dll has no FidelityFX API exports";
            return false;
        }
        uint64_t count = 0;
        ffxQueryDescGetVersions versions{};
        versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        versions.outputCount = &count;
        if (fn_.Query(nullptr, &versions.header) != FFX_API_RETURN_OK || count == 0) {
            reason = "no FSR upscaler in amd_fidelityfx_vk.dll";
            return false;
        }
        std::vector<uint64_t> ids(count);
        std::vector<const char*> names(count);
        versions.versionIds = ids.data();
        versions.versionNames = names.data();
        if (fn_.Query(nullptr, &versions.header) != FFX_API_RETURN_OK) {
            reason = "no FSR upscaler in amd_fidelityfx_vk.dll";
            return false;
        }
        std::string listed;
        std::vector<int> best;
        for (uint64_t i = 0; i < count; ++i) {
            const std::string name = names[i] ? names[i] : "";
            listed += std::format("{}{}", listed.empty() ? "" : ", ", name);
            std::vector<int> parts;
            for (size_t at = 0; at < name.size();) {
                size_t end = at;
                while (end < name.size() && name[end] >= '0' && name[end] <= '9') ++end;
                if (end == at) break;
                parts.push_back(std::stoi(name.substr(at, end - at)));
                at = end < name.size() && name[end] == '.' ? end + 1 : name.size();
            }
            if (!parts.empty() && parts[0] == generation_ && parts > best) {
                best = parts;
                version_id_ = ids[i];
                version_name_ = name;
            }
        }
        LogInfo("fsr: amd_fidelityfx_vk.dll upscalers: {}", listed);
        if (!version_id_) {
            reason = generation_ == 4 ? "AMD releases FSR 4 for DirectX 12 only, and this game renders with Vulkan"
                                      : std::format("no FSR {} upscaler in amd_fidelityfx_vk.dll", generation_);
            return false;
        }
        return true;
    }

    bool Create(VkCommandBuffer, const UpscaleCreate& create) override {
        Release();
        ffxCreateBackendVKDesc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        backend.vkDevice = ctx_.device;
        backend.vkPhysicalDevice = ctx_.physical;
        backend.vkDeviceProcAddr = vkGetDeviceProcAddr;
        ffxCreateContextDescUpscale desc{};
        desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        desc.header.pNext = &backend.header;
        desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
        if (std::getenv("PT_UPSCALE_CHECK")) {
            desc.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
        }
        desc.maxRenderSize = {create.render.width, create.render.height};
        desc.maxUpscaleSize = {create.display.width, create.display.height};
        desc.fpMessage = Message;
        ffxOverrideVersion version_override{};
        version_override.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        version_override.versionId = version_id_;
        backend.header.pNext = &version_override.header;
        const ffxReturnCode_t result = fn_.CreateContext(&context_, &desc.header, nullptr);
        if (result != FFX_API_RETURN_OK) {
            LogError("fsr: ffxCreateContext failed ({}) for FSR {}", result, version_name_);
            context_ = nullptr;
            return false;
        }
        ffxQueryGetProviderVersion version{};
        version.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (fn_.Query(&context_, &version.header) == FFX_API_RETURN_OK && version.versionName) {
            LogInfo("fsr: upscaler {} created for {}x{} -> {}x{}", version.versionName, create.render.width, create.render.height,
                    create.display.width, create.display.height);
        }
        debug_view_ = std::getenv("PT_UPSCALE_DEBUG") != nullptr;
        return true;
    }

    bool Dispatch(const UpscaleDispatch& d) override {
        if (!context_) {
            return false;
        }
        ffxDispatchDescUpscale u{};
        u.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        u.commandList = d.cmd;
        u.color = Resource(d.color, FFX_API_RESOURCE_STATE_COMPUTE_READ, false, false);
        u.depth = Resource(d.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ, true, false);
        u.motionVectors = Resource(d.motion, FFX_API_RESOURCE_STATE_COMPUTE_READ, false, false);
        u.exposure = Resource(d.exposure, FFX_API_RESOURCE_STATE_COMPUTE_READ, false, false);
        u.reactive = Resource(d.reactive, FFX_API_RESOURCE_STATE_COMPUTE_READ, false, false);
        u.transparencyAndComposition = Resource(d.transparency, FFX_API_RESOURCE_STATE_COMPUTE_READ, false, false);
        u.output = Resource(d.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, false, true);
        u.jitterOffset = {d.jitter.x, d.jitter.y};
        u.motionVectorScale = {d.motion_scale.x, d.motion_scale.y};
        u.renderSize = {d.render.width, d.render.height};
        u.upscaleSize = {d.display.width, d.display.height};
        u.enableSharpening = d.sharpness > 0.0f;
        u.sharpness = d.sharpness;
        u.frameTimeDelta = d.frame_ms;
        u.preExposure = d.pre_exposure;
        u.reset = d.reset;
        u.cameraNear = FLT_MAX;
        u.cameraFar = d.near_plane;
        u.cameraFovAngleVertical = d.fov_y;
        u.viewSpaceToMetersFactor = 1.0f;
        u.flags = debug_view_ ? FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW : 0u;
        const ffxReturnCode_t result = fn_.Dispatch(&context_, &u.header);
        if (result != FFX_API_RETURN_OK) {
            if (!dispatch_error_logged_) {
                LogError("fsr: ffxDispatch failed ({})", result);
                dispatch_error_logged_ = true;
            }
            return false;
        }
        return true;
    }

    void Release() override {
        if (context_) {
            vkDeviceWaitIdle(ctx_.device);
            fn_.DestroyContext(&context_, nullptr);
            context_ = nullptr;
        }
    }

private:
    vk::Context& ctx_;
    int generation_ = 3;
    bool probed_ = false;
    std::string reason_;
    uint64_t version_id_ = 0;
    std::string version_name_;
    HMODULE module_ = nullptr;
    ffxFunctions fn_{};
    ffxContext context_ = nullptr;
    bool debug_view_ = false;
    bool dispatch_error_logged_ = false;
};

}

std::unique_ptr<UpscaleBackend> CreateFsrBackend(vk::Context& ctx, int generation) {
    return std::make_unique<FsrBackend>(ctx, generation);
}

}

#else

namespace pt {

std::unique_ptr<UpscaleBackend> CreateFsrBackend(vk::Context&, int) {
    return nullptr;
}

}

#endif
