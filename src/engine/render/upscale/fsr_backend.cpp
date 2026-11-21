#include "engine/render/upscale/upscale.h"

#if defined(PT_WITH_FSR)

#ifdef _WIN32
#include <windows.h>

#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_api_loader.h>
#else
#include <dlfcn.h>

#define __declspec(x)
#include <ffx_api/ffx_api.h>
#undef __declspec
#endif
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
#include "engine/render/upscale/fsr_provider_gate.h"

namespace pt {
namespace {

#ifdef _WIN32
using FfxModule = HMODULE;
constexpr const char* kFfxLibrary = "amd_fidelityfx_vk.dll";
constexpr const char* kFfx2VulkanLoader = "amd_fidelityfx_loader_vk.dll";
constexpr const char* kExecutable = "pt.exe";
#else
using FfxModule = void*;
constexpr const char* kFfxLibrary = "libamd_fidelityfx_vk.so";
constexpr const char* kFfx2VulkanLoader = kFfxLibrary;
constexpr const char* kExecutable = "pt";

struct ffxFunctions {
    PfnFfxCreateContext CreateContext;
    PfnFfxDestroyContext DestroyContext;
    PfnFfxConfigure Configure;
    PfnFfxQuery Query;
    PfnFfxDispatch Dispatch;
};

void ffxLoadFunctions(ffxFunctions* out, void* module) {
    out->CreateContext = reinterpret_cast<PfnFfxCreateContext>(dlsym(module, "ffxCreateContext"));
    out->DestroyContext = reinterpret_cast<PfnFfxDestroyContext>(dlsym(module, "ffxDestroyContext"));
    out->Configure = reinterpret_cast<PfnFfxConfigure>(dlsym(module, "ffxConfigure"));
    out->Query = reinterpret_cast<PfnFfxQuery>(dlsym(module, "ffxQuery"));
    out->Dispatch = reinterpret_cast<PfnFfxDispatch>(dlsym(module, "ffxDispatch"));
}
#endif

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

class FsrBackend final : public UpscaleBackend {
public:
    FsrBackend(vk::Context& ctx, int generation) : ctx_(ctx), generation_(generation) {}
    ~FsrBackend() override {
        Release();
        if (module_) {
#ifdef _WIN32
            FreeLibrary(module_);
#else
            dlclose(module_);
#endif
        }
    }

    UpscalerKind Kind() const override { return generation_ == 4 ? UpscalerKind::Fsr4 : UpscalerKind::Fsr; }

    bool Supported(std::string& reason) override {
        if (!std::filesystem::exists(ExecutableDir() / kFfxLibrary)) {
            reason = std::format("{} is missing next to {}", kFfxLibrary, kExecutable);
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
        std::filesystem::path path = ExecutableDir() / kFfxLibrary;
#ifdef _WIN32
        if (generation_ == 4 && !module_) {
            const std::filesystem::path loader = ExecutableDir() / kFfx2VulkanLoader;
            if (std::error_code ec; std::filesystem::exists(loader, ec)) {
                module_ = LoadLibraryExW(loader.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                if (module_) {
                    sdk2_ = true;
                    path = loader;
                    LogInfo("fsr: FSR 4 uses {}", kFfx2VulkanLoader);
                } else {
                    LogWarn("fsr: {} is present but cannot be loaded (error {}); FSR 4 uses {}", kFfx2VulkanLoader, GetLastError(),
                            kFfxLibrary);
                }
            }
        }
        module_ = module_ ? module_ : LoadLibraryW(path.c_str());
#else
        if (!module_) {
            module_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (module_) {
                LogInfo("fsr: loaded {}", path.string());
            } else if (std::error_code ec; std::filesystem::exists(path, ec)) {
                const char* error = dlerror();
                LogWarn("fsr: cannot load {}: {}", path.string(), error ? error : "unknown error");
                reason = std::format("{} could not be loaded ({})", kFfxLibrary, error ? error : "unknown error");
                return false;
            }
        }
#endif
        if (!module_) {
            reason = std::format("{} is missing next to {}", kFfxLibrary, kExecutable);
            return false;
        }
        ffxLoadFunctions(&fn_, module_);
        if (!fn_.CreateContext || !fn_.DestroyContext || !fn_.Dispatch || !fn_.Query || !fn_.Configure) {
            reason = std::format("{} has no FidelityFX API exports", kFfxLibrary);
            return false;
        }
        const char* library = sdk2_ ? kFfx2VulkanLoader : kFfxLibrary;
        FsrGateInput gate;
        gate.generation = generation_;
        gate.api = FsrApi::Vulkan;
        gate.vendor_id = ctx_.properties.vendorID;
        gate.vulkan_sdk2 = sdk2_;
        uint64_t count = 0;
        ffxQueryDescGetVersions versions{};
        versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        versions.outputCount = &count;
        const bool counted = fn_.Query(nullptr, &versions.header) == FFX_API_RETURN_OK;
        if (counted && count > 0 && count < 256) {
            std::vector<uint64_t> ids(count);
            std::vector<const char*> names(count);
            versions.versionIds = ids.data();
            versions.versionNames = names.data();
            if (fn_.Query(nullptr, &versions.header) == FFX_API_RETURN_OK) {
                gate.query_ok = true;
                for (uint64_t i = 0; i < count; ++i) gate.providers.push_back({ids[i], names[i] ? names[i] : ""});
            }
        } else if (counted && count == 0) {
            gate.query_ok = true;
        }
        const FsrGateResult decision = DecideFsrProvider(gate);
        LogInfo("fsr: {} upscalers: {}", library, decision.listed);
        if (!decision.available) {
            version_id_ = 0;
            reason = generation_ == 4 ? decision.reason
                     : !gate.query_ok || gate.providers.empty() ? std::format("no FSR upscaler in {}", library)
                                                                : std::format("no FSR {} upscaler in {}", generation_, library);
            if (generation_ == 4) LogInfo("fsr: FSR 4 not offered: {}", reason);
            return false;
        }
        version_id_ = decision.id;
        version_name_ = decision.name;
        if (generation_ == 4) LogInfo("fsr: FSR 4 offered with upscaler provider {}", version_name_);
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
#if defined(FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION)
        ffxCreateContextDescUpscaleVersion api_version{};
        if (sdk2_) {
            api_version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
            api_version.version = FFX_UPSCALER_VERSION;
            api_version.header.pNext = desc.header.pNext;
            desc.header.pNext = &api_version.header;
        }
#endif
        ffxOverrideVersion version_override{};
        version_override.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        version_override.versionId = version_id_;
        backend.header.pNext = &version_override.header;
        const ffxReturnCode_t result = fn_.CreateContext(&context_, &desc.header, nullptr);
        if (result != FFX_API_RETURN_OK) {
            LogError("fsr: ffxCreateContext failed ({}) for FSR {}", result, version_name_);
            context_ = nullptr;
            if (generation_ == 4) {
                version_id_ = 0;
                reason_ = std::string(kFsr4ReasonDriver);
                LogWarn("fsr: FSR 4 not offered: {}", reason_);
            }
            return false;
        }
        ffxQueryGetProviderVersion version{};
        version.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        const bool named = fn_.Query(&context_, &version.header) == FFX_API_RETURN_OK && version.versionName;
        const std::string created = named ? version.versionName : "";
        if (generation_ == 4 && named && !FsrCreatedIsGeneration(created, 4)) {
            LogWarn("fsr: the FSR {} context runs upscaler {}, not FSR 4", version_name_, created);
            fn_.DestroyContext(&context_, nullptr);
            context_ = nullptr;
            version_id_ = 0;
            reason_ = std::string(kFsr4ReasonDriver);
            LogWarn("fsr: FSR 4 not offered: {}", reason_);
            return false;
        }
        if (named) {
            LogInfo("fsr: upscaler {} created for {}x{} -> {}x{}", created, create.render.width, create.render.height,
                    create.display.width, create.display.height);
#ifndef _WIN32
            LogShaderSet();
#endif
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
#ifndef _WIN32
    void LogShaderSet() const {
        VkPhysicalDeviceSubgroupSizeControlProperties subgroups{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &subgroups};
        vkGetPhysicalDeviceProperties2(ctx_.physical, &properties);
        VkPhysicalDeviceShaderFloat16Int8Features float16{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &float16};
        vkGetPhysicalDeviceFeatures2(ctx_.physical, &features);
        const bool no_lut = std::getenv("PT_FSR_NO_LUT") != nullptr;
        const bool no_fp16 = std::getenv("PT_FSR_NO_FP16") != nullptr;
        const bool lut = subgroups.minSubgroupSize == 32 && subgroups.maxSubgroupSize == 64 && !no_lut;
        const bool half = float16.shaderFloat16 && !no_fp16;
        LogInfo("fsr: shader set: subgroups {}-{}, float16 {}: reprojection {}, {}-bit shaders{}{}", subgroups.minSubgroupSize,
                subgroups.maxSubgroupSize, float16.shaderFloat16 ? "yes" : "no", lut ? "by the Lanczos table" : "computed",
                half ? 16 : 32, no_lut ? " (PT_FSR_NO_LUT)" : "", no_fp16 ? " (PT_FSR_NO_FP16)" : "");
    }
#endif

    vk::Context& ctx_;
    int generation_ = 3;
    bool probed_ = false;
    std::string reason_;
    uint64_t version_id_ = 0;
    std::string version_name_;
    FfxModule module_ = nullptr;
    ffxFunctions fn_{};
    ffxContext context_ = nullptr;
    bool debug_view_ = false;
    bool sdk2_ = false;
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
