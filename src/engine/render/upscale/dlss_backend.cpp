#include "engine/render/upscale/upscale.h"

#if defined(PT_WITH_DLSS)

#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_vk.h>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <iterator>
#include <string>
#include <system_error>

#include "engine/core/log.h"
#include "engine/render/upscale/upscale_platform.h"

namespace pt {
namespace {

constexpr const char* kProjectId = "3c9e5a1b-7d24-4f68-b0e3-5a2c8d91f47e";
constexpr const char* kEngineVersion = "1.0";
constexpr uint32_t kNvidiaVendor = 0x10DE;
constexpr const char* kInstanceExtensions[] = {VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME};
constexpr const char* kDeviceExtensions[] = {VK_NVX_BINARY_IMPORT_EXTENSION_NAME, VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME,
                                             VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};

const std::wstring& DataPath() {
    static const std::wstring path = [] {
        std::error_code ec;
        std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "pt-port-ngx";
        std::filesystem::create_directories(dir, ec);
        return dir.wstring();
    }();
    return path;
}

const std::wstring& FeaturePath() {
    static const std::wstring path = ExecutableDir().wstring();
    return path;
}

NVSDK_NGX_FeatureCommonInfo FeatureInfo() {
    static const wchar_t* paths[1] = {FeaturePath().c_str()};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    info.LoggingInfo.MinimumLoggingLevel = std::getenv("PT_UPSCALE_CHECK") ? NVSDK_NGX_LOGGING_LEVEL_ON : NVSDK_NGX_LOGGING_LEVEL_OFF;
    return info;
}

NVSDK_NGX_FeatureDiscoveryInfo Discovery(const NVSDK_NGX_FeatureCommonInfo* common) {
    NVSDK_NGX_FeatureDiscoveryInfo info{};
    info.SDKVersion = NVSDK_NGX_Version_API;
    info.FeatureID = NVSDK_NGX_Feature_SuperSampling;
    info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    info.Identifier.v.ProjectDesc.ProjectId = kProjectId;
    info.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    info.Identifier.v.ProjectDesc.EngineVersion = kEngineVersion;
    info.ApplicationDataPath = DataPath().c_str();
    info.FeatureInfo = common;
    return info;
}

NVSDK_NGX_PerfQuality_Value QualityValue(UpscaleQuality quality) {
    switch (quality) {
    case UpscaleQuality::NativeAA: return NVSDK_NGX_PerfQuality_Value_DLAA;
    case UpscaleQuality::Quality: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case UpscaleQuality::Balanced: return NVSDK_NGX_PerfQuality_Value_Balanced;
    case UpscaleQuality::Performance: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    case UpscaleQuality::UltraPerformance: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    default: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    }
}

// the model is the NGX render preset of every quality mode; Default lets NGX pick per mode (K for DLAA, quality and balanced, M for
// performance, L for ultra performance in SDK 310.9.1). L and M are the second generation transformer (FP8), about twice the
// cost of K on RTX 20 and 30 GPUs, which have no FP8 (upscaling.md)
uint32_t PresetValue(DlssModel model) {
    switch (model) {
    case DlssModel::K: return NVSDK_NGX_DLSS_Hint_Render_Preset_K;
    case DlssModel::L: return NVSDK_NGX_DLSS_Hint_Render_Preset_L;
    case DlssModel::M: return NVSDK_NGX_DLSS_Hint_Render_Preset_M;
    default: return NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
    }
}

}

uint32_t DlssPresetHint(DlssModel model) {
    return PresetValue(model);
}

bool DlssAutoExposure() {
    return false;
}

namespace {

NVSDK_NGX_Resource_VK Resource(const UpscaleImage& image, VkImageAspectFlags aspect, bool read_write) {
    return NVSDK_NGX_Create_ImageView_Resource_VK(image.view, image.image, {aspect, 0, 1, 0, 1}, image.format, image.extent.width,
                                                  image.extent.height, read_write);
}

class DlssBackend final : public UpscaleBackend {
public:
    explicit DlssBackend(vk::Context& ctx) : ctx_(ctx) {}
    ~DlssBackend() override { Shutdown(); }

    UpscalerKind Kind() const override { return UpscalerKind::Dlss; }

    bool Supported(std::string& reason) override {
        if (ctx_.properties.vendorID != kNvidiaVendor) {
            reason = "needs an NVIDIA RTX GPU";
            return false;
        }
        if (!std::filesystem::exists(ExecutableDir() / L"nvngx_dlss.dll")) {
            reason = "nvngx_dlss.dll is missing next to pt.exe";
            return false;
        }
        for (const char* name : kDeviceExtensions) {
            if (!UpscaleHost::Get().Enabled(name, true)) {
                reason = std::format("the driver has no {}", name);
                return false;
            }
        }
        reason.clear();
        return true;
    }

    bool Available(std::string& reason) override {
        if (!Supported(reason)) {
            return false;
        }
        const NVSDK_NGX_FeatureCommonInfo common = FeatureInfo();
        NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, DataPath().c_str(),
                                                                       ctx_.instance, ctx_.physical, ctx_.device, vkGetInstanceProcAddr,
                                                                       vkGetDeviceProcAddr, &common, NVSDK_NGX_Version_API);
        if (NVSDK_NGX_FAILED(result)) {
            reason = "NGX did not initialise (driver without DLSS support?)";
            LogWarn("dlss: NVSDK_NGX_VULKAN_Init_with_ProjectID failed ({:#x})", static_cast<uint32_t>(result));
            return false;
        }
        initialized_ = true;
        const std::string missing = MissingExtension(common);
        if (!missing.empty()) {
            reason = std::format("restart the game to use DLSS (needs {})", missing);
            return false;
        }
        result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&params_);
        if (NVSDK_NGX_FAILED(result) || !params_) {
            reason = "NGX capability query failed";
            return false;
        }
        int available = 0;
        NVSDK_NGX_Parameter_GetI(params_, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
        if (!available) {
            int needs_driver = 0;
            unsigned int major = 0;
            unsigned int minor = 0;
            NVSDK_NGX_Parameter_GetI(params_, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
            NVSDK_NGX_Parameter_GetUI(params_, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
            NVSDK_NGX_Parameter_GetUI(params_, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
            reason = needs_driver ? std::format("driver too old, DLSS needs {}.{} or newer", major, minor) : "DLSS is not supported on this GPU";
            return false;
        }
        return true;
    }

    bool Create(VkCommandBuffer cmd, const UpscaleCreate& create) override {
        ReleaseFeature();
        if (!params_) {
            return false;
        }
        const uint32_t preset = PresetValue(create.dlss_model);
        for (const char* mode : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                                 NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                                 NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                                 NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality}) {
            NVSDK_NGX_Parameter_SetUI(params_, mode, preset);
        }
        NVSDK_NGX_DLSS_Create_Params p{};
        p.Feature.InWidth = create.render.width;
        p.Feature.InHeight = create.render.height;
        p.Feature.InTargetWidth = create.display.width;
        p.Feature.InTargetHeight = create.display.height;
        p.Feature.InPerfQualityValue = QualityValue(create.quality);
        p.InFeatureCreateFlags =
            NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
        // DLSS's own exposure (guide 3.9/3.10: MidGray / (AverageLuma * (1 - MidGray)) over the frame) instead of the game's eye
        // adaptation, which leaves P.T.'s dark frames far below mid grey: with it model K trails the handy light's spot over a still
        // wall less (upscaling.md, Flashlight); PT_DLSS_AUTO_EXPOSURE=0 passes the game's value as before
        static const bool auto_exposure = [] {
            const char* e = std::getenv("PT_DLSS_AUTO_EXPOSURE");
            return !e || std::atoi(e) != 0;
        }();
        if (auto_exposure) {
            p.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        }
        p.InEnableOutputSubrects = false;
        const NVSDK_NGX_Result result = NGX_VULKAN_CREATE_DLSS_EXT1(ctx_.device, cmd, 1, 1, &feature_, params_, &p);
        if (NVSDK_NGX_FAILED(result) || !feature_) {
            LogError("dlss: feature creation failed ({:#x}) for {}x{} -> {}x{}", static_cast<uint32_t>(result), create.render.width,
                     create.render.height, create.display.width, create.display.height);
            feature_ = nullptr;
            return false;
        }
        LogInfo("dlss: super resolution created for {}x{} -> {}x{}, quality value {}, model {} (render preset hint {})", create.render.width,
                create.render.height, create.display.width, create.display.height, static_cast<int>(p.Feature.InPerfQualityValue),
                DlssModelKey(create.dlss_model), preset);
        return true;
    }


    bool Dispatch(const UpscaleDispatch& d) override {
        if (!feature_) {
            return false;
        }
        NVSDK_NGX_Resource_VK color = Resource(d.color, VK_IMAGE_ASPECT_COLOR_BIT, false);
        NVSDK_NGX_Resource_VK depth = Resource(d.depth, VK_IMAGE_ASPECT_DEPTH_BIT, false);
        NVSDK_NGX_Resource_VK motion = Resource(d.motion, VK_IMAGE_ASPECT_COLOR_BIT, false);
        NVSDK_NGX_Resource_VK exposure = Resource(d.exposure, VK_IMAGE_ASPECT_COLOR_BIT, false);
        NVSDK_NGX_Resource_VK reactive = Resource(d.reactive, VK_IMAGE_ASPECT_COLOR_BIT, false);
        NVSDK_NGX_Resource_VK output = Resource(d.output, VK_IMAGE_ASPECT_COLOR_BIT, true);
        NVSDK_NGX_VK_DLSS_Eval_Params e{};
        e.Feature.pInColor = &color;
        e.Feature.pInOutput = &output;
        e.pInDepth = &depth;
        e.pInMotionVectors = &motion;
        e.InJitterOffsetX = d.jitter.x;
        e.InJitterOffsetY = d.jitter.y;
        e.InRenderSubrectDimensions = {d.render.width, d.render.height};
        e.InReset = d.reset ? 1 : 0;
        e.InMVScaleX = d.motion_scale.x;
        e.InMVScaleY = d.motion_scale.y;
        e.pInExposureTexture = &exposure;
        e.pInBiasCurrentColorMask = d.reactive.Valid() ? &reactive : nullptr;
        e.InPreExposure = d.pre_exposure;
        e.InExposureScale = 1.0f;
        e.InFrameTimeDeltaInMsec = d.frame_ms;
        if (std::getenv("PT_DLSS_TRACE")) {
            static uint64_t frames = 0;
            if (++frames % 60 == 0) LogInfo("dlss eval: reset {} jitter ({:.4f} {:.4f}) frame_ms {:.4f} mask {}", d.reset, d.jitter.x, d.jitter.y, d.frame_ms, e.pInBiasCurrentColorMask != nullptr);
        }
        const NVSDK_NGX_Result result = NGX_VULKAN_EVALUATE_DLSS_EXT(d.cmd, feature_, params_, &e);
        if (NVSDK_NGX_FAILED(result)) {
            if (!error_logged_) {
                LogError("dlss: evaluate failed ({:#x})", static_cast<uint32_t>(result));
                error_logged_ = true;
            }
            return false;
        }
        return true;
    }

    void Release() override { ReleaseFeature(); }

private:
    std::string MissingExtension(const NVSDK_NGX_FeatureCommonInfo& common) const {
        const NVSDK_NGX_FeatureDiscoveryInfo discovery = Discovery(&common);
        uint32_t count = 0;
        VkExtensionProperties* properties = nullptr;
        if (NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&discovery, &count, &properties))) {
            for (uint32_t i = 0; properties && i < count; ++i) {
                if (!UpscaleHost::Get().Enabled(properties[i].extensionName, false)) {
                    return properties[i].extensionName;
                }
            }
        }
        count = 0;
        properties = nullptr;
        if (NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(ctx_.instance, ctx_.physical, &discovery, &count,
                                                                                    &properties))) {
            for (uint32_t i = 0; properties && i < count; ++i) {
                if (!UpscaleHost::Get().Enabled(properties[i].extensionName, true)) {
                    return properties[i].extensionName;
                }
            }
        }
        return {};
    }

    void ReleaseFeature() {
        if (feature_) {
            vkDeviceWaitIdle(ctx_.device);
            NVSDK_NGX_VULKAN_ReleaseFeature(feature_);
            feature_ = nullptr;
        }
    }

    void Shutdown() {
        ReleaseFeature();
        if (params_) {
            NVSDK_NGX_VULKAN_DestroyParameters(params_);
            params_ = nullptr;
        }
        if (initialized_) {
            vkDeviceWaitIdle(ctx_.device);
            NVSDK_NGX_VULKAN_Shutdown1(ctx_.device);
            initialized_ = false;
        }
    }

    vk::Context& ctx_;
    bool initialized_ = false;
    NVSDK_NGX_Parameter* params_ = nullptr;
    NVSDK_NGX_Handle* feature_ = nullptr;
    bool error_logged_ = false;
};

void AppendExtensions(VkExtensionProperties* properties, uint32_t count, std::vector<std::string>& out) {
    for (uint32_t i = 0; properties && i < count; ++i) {
        out.emplace_back(properties[i].extensionName);
    }
}

}

std::unique_ptr<UpscaleBackend> CreateDlssBackend(vk::Context& ctx) {
    return std::make_unique<DlssBackend>(ctx);
}

void DlssInstanceExtensions(bool query, std::vector<std::string>& out) {
    if (!query) {
        out.assign(std::begin(kInstanceExtensions), std::end(kInstanceExtensions));
        return;
    }
    const NVSDK_NGX_FeatureCommonInfo common = FeatureInfo();
    const NVSDK_NGX_FeatureDiscoveryInfo discovery = Discovery(&common);
    uint32_t count = 0;
    VkExtensionProperties* properties = nullptr;
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&discovery, &count, &properties))) {
        AppendExtensions(properties, count, out);
    }
}

bool DlssFrameGenRequirements(VkInstance instance, VkPhysicalDevice physical, uint32_t& unsupported, std::string& detail) {
    const NVSDK_NGX_FeatureCommonInfo common = FeatureInfo();
    NVSDK_NGX_FeatureDiscoveryInfo discovery = Discovery(&common);
    discovery.FeatureID = NVSDK_NGX_Feature_FrameGeneration;
    NVSDK_NGX_FeatureRequirement requirement{};
    const NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, physical, &discovery, &requirement);
    if (NVSDK_NGX_FAILED(result)) {
        detail = std::format("query failed {:#x}", static_cast<uint32_t>(result));
        return false;
    }
    unsupported = static_cast<uint32_t>(requirement.FeatureSupported);
    detail = std::format("NGX support bits {:#x}, minimum architecture {:#x}, minimum OS {}", unsupported, requirement.MinHWArchitecture,
                         requirement.MinOSVersion);
    return true;
}

void DlssDeviceExtensions(bool query, VkInstance instance, VkPhysicalDevice physical, std::vector<std::string>& out) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical, &props);
    if (props.vendorID != kNvidiaVendor) {
        return;
    }
    if (!query) {
        out.assign(std::begin(kDeviceExtensions), std::end(kDeviceExtensions));
        return;
    }
    const NVSDK_NGX_FeatureCommonInfo common = FeatureInfo();
    const NVSDK_NGX_FeatureDiscoveryInfo discovery = Discovery(&common);
    uint32_t count = 0;
    VkExtensionProperties* properties = nullptr;
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance, physical, &discovery, &count, &properties))) {
        AppendExtensions(properties, count, out);
    }
}

}

#else

namespace pt {

std::unique_ptr<UpscaleBackend> CreateDlssBackend(vk::Context&) {
    return nullptr;
}

uint32_t DlssPresetHint(DlssModel) {
    return 0;
}

bool DlssAutoExposure() {
    return false;
}

void DlssInstanceExtensions(bool, std::vector<std::string>&) {}

void DlssDeviceExtensions(bool, VkInstance, VkPhysicalDevice, std::vector<std::string>&) {}

bool DlssFrameGenRequirements(VkInstance, VkPhysicalDevice, uint32_t& unsupported, std::string& detail) {
    unsupported = 0;
    detail = "this build has no DLSS SDK";
    return false;
}

}

#endif
