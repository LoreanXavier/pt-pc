#include "engine/render/upscale/upscale.h"

#if defined(PT_WITH_XESS)

#include <windows.h>

#include <xess/xess_vk.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <type_traits>

#include "engine/core/log.h"
#include "engine/render/upscale/upscale_platform.h"

namespace pt {
namespace {

struct XessApi {
    HMODULE module = nullptr;
    decltype(&xessGetVersion) GetVersion = nullptr;
    decltype(&xessVKGetRequiredInstanceExtensions) RequiredInstanceExtensions = nullptr;
    decltype(&xessVKGetRequiredDeviceExtensions) RequiredDeviceExtensions = nullptr;
    decltype(&xessVKGetRequiredDeviceFeatures) RequiredDeviceFeatures = nullptr;
    decltype(&xessVKCreateContext) CreateContext = nullptr;
    decltype(&xessVKInit) Init = nullptr;
    decltype(&xessVKExecute) Execute = nullptr;
    decltype(&xessDestroyContext) DestroyContext = nullptr;
    decltype(&xessSetVelocityScale) SetVelocityScale = nullptr;
    decltype(&xessSetLoggingCallback) SetLoggingCallback = nullptr;

    bool Load() {
        if (module) {
            return true;
        }
        module = LoadLibraryW((ExecutableDir() / L"libxess.dll").c_str());
        if (!module) {
            return false;
        }
        auto get = [&](auto& fn, const char* name) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(module, name)); };
        get(GetVersion, "xessGetVersion");
        get(RequiredInstanceExtensions, "xessVKGetRequiredInstanceExtensions");
        get(RequiredDeviceExtensions, "xessVKGetRequiredDeviceExtensions");
        get(RequiredDeviceFeatures, "xessVKGetRequiredDeviceFeatures");
        get(CreateContext, "xessVKCreateContext");
        get(Init, "xessVKInit");
        get(Execute, "xessVKExecute");
        get(DestroyContext, "xessDestroyContext");
        get(SetVelocityScale, "xessSetVelocityScale");
        get(SetLoggingCallback, "xessSetLoggingCallback");
        if (!GetVersion || !RequiredDeviceExtensions || !RequiredDeviceFeatures || !CreateContext || !Init || !Execute || !DestroyContext ||
            !SetVelocityScale) {
            FreeLibrary(module);
            module = nullptr;
            return false;
        }
        return true;
    }
};

XessApi& Api() {
    static XessApi api;
    return api;
}

void Log(const char* message, xess_logging_level_t level) {
    if (level >= XESS_LOGGING_LEVEL_ERROR) {
        LogError("xess: {}", message);
    } else if (level >= XESS_LOGGING_LEVEL_WARNING) {
        LogWarn("xess: {}", message);
    }
}

xess_quality_settings_t QualitySetting(UpscaleQuality quality, VkExtent2D render, VkExtent2D display) {
    switch (quality) {
    case UpscaleQuality::NativeAA: return XESS_QUALITY_SETTING_AA;
    case UpscaleQuality::Quality: return XESS_QUALITY_SETTING_ULTRA_QUALITY;
    case UpscaleQuality::Balanced: return XESS_QUALITY_SETTING_QUALITY;
    case UpscaleQuality::Performance: return XESS_QUALITY_SETTING_BALANCED;
    case UpscaleQuality::UltraPerformance: return XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;
    default: break;
    }
    struct Preset {
        float ratio;
        xess_quality_settings_t setting;
    };
    constexpr Preset presets[] = {{1.0f, XESS_QUALITY_SETTING_AA},          {1.3f, XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS},
                                  {1.5f, XESS_QUALITY_SETTING_ULTRA_QUALITY}, {1.7f, XESS_QUALITY_SETTING_QUALITY},
                                  {2.0f, XESS_QUALITY_SETTING_BALANCED},     {2.3f, XESS_QUALITY_SETTING_PERFORMANCE},
                                  {3.0f, XESS_QUALITY_SETTING_ULTRA_PERFORMANCE}};
    const float ratio = static_cast<float>(display.width) / static_cast<float>(std::max(render.width, 1u));
    const Preset* best = &presets[0];
    for (const Preset& p : presets) {
        if (std::abs(p.ratio - ratio) < std::abs(best->ratio - ratio)) {
            best = &p;
        }
    }
    return best->setting;
}

xess_vk_image_view_info View(const UpscaleImage& image, VkImageAspectFlags aspect) {
    xess_vk_image_view_info info{};
    info.imageView = image.view;
    info.image = image.image;
    info.subresourceRange = {aspect, 0, 1, 0, 1};
    info.format = image.format;
    info.width = image.extent.width;
    info.height = image.extent.height;
    return info;
}

void ReadFeatures(const void* chain, DeviceFeatureSet& out) {
    for (auto* s = static_cast<const VkBaseInStructure*>(chain); s; s = s->pNext) {
        switch (s->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
            out.core = reinterpret_cast<const VkPhysicalDeviceFeatures2*>(s)->features;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
            out.v11 = *reinterpret_cast<const VkPhysicalDeviceVulkan11Features*>(s);
            out.v11.pNext = nullptr;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
            out.v12 = *reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(s);
            out.v12.pNext = nullptr;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
            out.v13 = *reinterpret_cast<const VkPhysicalDeviceVulkan13Features*>(s);
            out.v13.pNext = nullptr;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT:
            out.mutable_descriptor = *reinterpret_cast<const VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT*>(s);
            out.mutable_descriptor.pNext = nullptr;
            break;
        default:
            LogWarn("xess: unhandled required feature struct {}", static_cast<int>(s->sType));
            out.unknown = true;
            break;
        }
    }
}

bool Query(VkInstance instance, VkPhysicalDevice physical, DeviceFeatureSet& out) {
    XessApi& api = Api();
    if (!api.Load()) {
        return false;
    }
    uint32_t count = 0;
    const char* const* names = nullptr;
    if (api.RequiredDeviceExtensions(instance, physical, &count, &names) != XESS_RESULT_SUCCESS) {
        return false;
    }
    for (uint32_t i = 0; names && i < count; ++i) {
        out.extensions.emplace_back(names[i]);
    }
    void* features = nullptr;
    if (api.RequiredDeviceFeatures(instance, physical, &features) != XESS_RESULT_SUCCESS) {
        return false;
    }
    ReadFeatures(features, out);
    return true;
}

class XessBackend final : public UpscaleBackend {
public:
    explicit XessBackend(vk::Context& ctx) : ctx_(ctx) {}
    ~XessBackend() override { Release(); }

    UpscalerKind Kind() const override { return UpscalerKind::Xess; }

    bool Supported(std::string& reason) override {
        if (!std::filesystem::exists(ExecutableDir() / L"libxess.dll")) {
            reason = "libxess.dll is missing next to pt.exe";
            return false;
        }
        if (!UpscaleHost::Get().Enabled(VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME, true)) {
            reason = "the driver has no VK_EXT_mutable_descriptor_type";
            return false;
        }
        reason.clear();
        return true;
    }

    bool Available(std::string& reason) override {
        if (!Supported(reason)) {
            return false;
        }
        XessApi& api = Api();
        if (!api.Load()) {
            reason = "libxess.dll has no XeSS Vulkan exports";
            return false;
        }
        DeviceFeatureSet wanted;
        if (!Query(ctx_.instance, ctx_.physical, wanted)) {
            reason = "XeSS does not support this GPU";
            return false;
        }
        const std::string missing = UpscaleHost::Get().MissingFeature(wanted);
        if (!missing.empty()) {
            reason = std::format("restart the game to use XeSS (needs {})", missing);
            return false;
        }
        const xess_result_t result = api.CreateContext(ctx_.instance, ctx_.physical, ctx_.device, &context_);
        if (result != XESS_RESULT_SUCCESS || !context_) {
            reason = result == XESS_RESULT_ERROR_UNSUPPORTED_DRIVER ? "the driver is too old for XeSS" : "XeSS does not support this GPU";
            LogWarn("xess: xessVKCreateContext failed ({})", static_cast<int>(result));
            context_ = nullptr;
            return false;
        }
        if (api.SetLoggingCallback) {
            api.SetLoggingCallback(context_, XESS_LOGGING_LEVEL_WARNING, Log);
        }
        xess_version_t version{};
        api.GetVersion(&version);
        LogInfo("xess: XeSS {}.{}.{}", version.major, version.minor, version.patch);
        reason.clear();
        return true;
    }

    bool Create(VkCommandBuffer, const UpscaleCreate& create) override {
        if (!context_) {
            XessApi& api = Api();
            const xess_result_t result = api.CreateContext(ctx_.instance, ctx_.physical, ctx_.device, &context_);
            if (result != XESS_RESULT_SUCCESS || !context_) {
                LogError("xess: xessVKCreateContext failed ({}) while recreating super resolution", static_cast<int>(result));
                context_ = nullptr;
                return false;
            }
            if (api.SetLoggingCallback) {
                api.SetLoggingCallback(context_, XESS_LOGGING_LEVEL_WARNING, Log);
            }
        }
        vkDeviceWaitIdle(ctx_.device);
        xess_vk_init_params_t p{};
        p.outputResolution = {create.display.width, create.display.height};
        p.qualitySetting = QualitySetting(create.quality, create.render, create.display);
        p.initFlags = XESS_INIT_FLAG_INVERTED_DEPTH | XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK;
        const xess_result_t result = Api().Init(context_, &p);
        if (result != XESS_RESULT_SUCCESS) {
            LogError("xess: xessVKInit failed ({}) for {}x{} -> {}x{}", static_cast<int>(result), create.render.width, create.render.height,
                     create.display.width, create.display.height);
            return false;
        }
        Api().SetVelocityScale(context_, static_cast<float>(create.render.width), static_cast<float>(create.render.height));
        LogInfo("xess: super resolution for {}x{} -> {}x{}, quality setting {}", create.render.width, create.render.height, create.display.width,
                create.display.height, static_cast<int>(p.qualitySetting));
        initialized_ = true;
        return true;
    }

    bool Dispatch(const UpscaleDispatch& d) override {
        if (!context_ || !initialized_) {
            return false;
        }
        xess_vk_execute_params_t e{};
        e.colorTexture = View(d.color, VK_IMAGE_ASPECT_COLOR_BIT);
        e.velocityTexture = View(d.motion, VK_IMAGE_ASPECT_COLOR_BIT);
        e.depthTexture = View(d.depth, VK_IMAGE_ASPECT_DEPTH_BIT);
        e.responsivePixelMaskTexture = View(d.reactive, VK_IMAGE_ASPECT_COLOR_BIT);
        e.outputTexture = View(d.output, VK_IMAGE_ASPECT_COLOR_BIT);
        e.jitterOffsetX = d.jitter.x;
        e.jitterOffsetY = d.jitter.y;
        e.exposureScale = 1.0f;
        e.resetHistory = d.reset ? 1u : 0u;
        e.inputWidth = d.render.width;
        e.inputHeight = d.render.height;
        const xess_result_t result = Api().Execute(context_, d.cmd, &e);
        if (result != XESS_RESULT_SUCCESS) {
            if (!error_logged_) {
                LogError("xess: xessVKExecute failed ({})", static_cast<int>(result));
                error_logged_ = true;
            }
            return false;
        }
        return true;
    }

    void Release() override {
        if (context_) {
            vkDeviceWaitIdle(ctx_.device);
            Api().DestroyContext(context_);
            context_ = nullptr;
        }
        initialized_ = false;
    }

private:
    vk::Context& ctx_;
    xess_context_handle_t context_ = nullptr;
    bool initialized_ = false;
    bool error_logged_ = false;
};

}

std::unique_ptr<UpscaleBackend> CreateXessBackend(vk::Context& ctx) {
    return std::make_unique<XessBackend>(ctx);
}

void XessDeviceRequirements(bool query, VkInstance instance, VkPhysicalDevice physical, DeviceFeatureSet& out) {
    if (!std::filesystem::exists(ExecutableDir() / L"libxess.dll")) {
        return;
    }
    if (query && Query(instance, physical, out)) {
        return;
    }
    out = {};
    out.extensions = {VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME};
    out.core.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    out.v12.shaderInt8 = VK_TRUE;
    out.v12.scalarBlockLayout = VK_TRUE;
    out.v13.shaderIntegerDotProduct = VK_TRUE;
    out.mutable_descriptor.mutableDescriptorType = VK_TRUE;
}

}

#else

namespace pt {

std::unique_ptr<UpscaleBackend> CreateXessBackend(vk::Context&) {
    return nullptr;
}

void XessDeviceRequirements(bool, VkInstance, VkPhysicalDevice, DeviceFeatureSet&) {}

}

#endif
