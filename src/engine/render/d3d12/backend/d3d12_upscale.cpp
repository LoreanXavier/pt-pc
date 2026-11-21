#include "engine/render/d3d12/backend/d3d12_upscale.h"
#include "engine/render/upscale/neural_native.h"
#include "engine/render/d3d12/backend/fsr_ray_regeneration.h"

#ifdef _WIN32

#include <windows.h>

#include <d3d12.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

#include "engine/render/upscale/streamline.h"
#include "engine/core/log.h"
#include "engine/core/resource_path.h"
#include "engine/render/d3d12/backend/d3d12_native.h"
#include "engine/render/d3d12/backend/reshade_native.h"
#include "engine/render/upscale/upscale_platform.h"
#include "engine/render/upscale/fsr_provider_gate.h"

#if defined(PT_WITH_DLSS)
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_dlssd_d3d.h>
#endif
#if defined(PT_WITH_XESS)
#include <xess/xess_d3d12.h>
#endif
#if defined(PT_WITH_FSR)
#if defined(PT_WITH_FSR_RR)
#include <dx12/ffx_api_dx12.h>
#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <ffx_upscale.h>
#else
#include <ffx_api/dx12/ffx_api_dx12.h>
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_api_loader.h>
#include <ffx_api/ffx_upscale.h>
#endif
#endif

namespace pt::gpu::d3d12 {
namespace {

[[maybe_unused]] ID3D12Resource* Res(const UpscaleImage& image) {
    return image.handle ? Native(image.handle) : nullptr;
}

[[maybe_unused]] ID3D12GraphicsCommandList* ListOf(gpu::CommandList* list) {
    return list ? Native(*list) : nullptr;
}

#if defined(PT_WITH_DLSS)

constexpr const char* kProjectId = "3c9e5a1b-7d24-4f68-b0e3-5a2c8d91f47e";
constexpr const char* kEngineVersion = "1.0";
constexpr uint32_t kNvidiaVendor = 0x10DE;

const std::wstring& DataPath() {
    static const std::wstring path = [] {
        std::error_code ec;
        std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "pt-port-ngx";
        std::filesystem::create_directories(dir, ec);
        return dir.wstring();
    }();
    return path;
}

unsigned long InitNgxSafely(ID3D12Device* device, const wchar_t* data_path, const NVSDK_NGX_FeatureCommonInfo* common, NVSDK_NGX_Result& result) {
    __try {
        result = NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, data_path, device, common,
                                                     NVSDK_NGX_Version_API);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

NVSDK_NGX_FeatureCommonInfo FeatureInfo() {
    static const std::wstring feature_path = ExecutableDir().wstring();
    static const std::wstring nr_path = [] {
        if (!nr_native::Enabled()) return std::wstring{};
        const auto& runtime = NativeNeuralRuntime();
        return runtime ? runtime->path.parent_path().wstring() : std::wstring{};
    }();
    static const wchar_t* paths[2] = {nr_path.empty() ? feature_path.c_str() : nr_path.c_str(), feature_path.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = nr_path.empty() ? 1 : 2;
    info.LoggingInfo.MinimumLoggingLevel = std::getenv("PT_UPSCALE_CHECK") ? NVSDK_NGX_LOGGING_LEVEL_ON : NVSDK_NGX_LOGGING_LEVEL_OFF;
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

class DlssD3D12 final : public UpscaleBackend {
public:
    explicit DlssD3D12(gpu::Device& device) : device_(device) {}
    ~DlssD3D12() override { Shutdown(); }

    UpscalerKind Kind() const override { return UpscalerKind::Dlss; }

    bool Supported(std::string& reason) override {
        if (device_.Info().vendor_id != kNvidiaVendor) {
            reason = "needs an NVIDIA RTX GPU";
            return false;
        }
        if (!std::filesystem::exists(ExecutableDir() / L"nvngx_dlss.dll")) {
            reason = "nvngx_dlss.dll is missing next to pt.exe";
            return false;
        }
        reason.clear();
        return true;
    }

    bool Available(std::string& reason) override {
        if (!Supported(reason)) {
            return false;
        }
        if (!InitializeNeuralCore(reason)) return false;
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
        CheckRayReconstruction();
        return true;
    }

    bool InitializeNeuralCore(std::string& reason) override {
        if (initialized_ && params_) return true;
        if (device_.Info().vendor_id != kNvidiaVendor) { reason = "needs an NVIDIA RTX GPU"; return false; }
        if (init_fault_) {
            reason = std::format("the NVIDIA driver faulted while DLSS started ({:#x}); DLSS is off until the game starts again", init_fault_);
            return false;
        }
        const NVSDK_NGX_FeatureCommonInfo common = FeatureInfo();
        ngx_device_ = ReShadeNative(NativeDevice(device_));
        if (ngx_device_.Get() != NativeDevice(device_)) {
            LogInfo("dlss d3d12: NGX uses ReShade's unwrapped device with native command lists");
        }
        NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
        init_fault_ = InitNgxSafely(ngx_device_.Get(), DataPath().c_str(), &common, result);
        if (init_fault_) {
            reason = std::format("the NVIDIA driver faulted while DLSS started ({:#x}); DLSS is off until the game starts again", init_fault_);
            LogError("dlss d3d12: the NVIDIA driver faulted inside NVSDK_NGX_D3D12_Init_with_ProjectID (exception {:#x}); DLSS stays off "
                     "for this session, the other upscalers work", init_fault_);
            return false;
        }
        if (NVSDK_NGX_FAILED(result)) {
            reason = "NGX did not initialise (driver without DLSS support?)";
            LogWarn("dlss d3d12: NVSDK_NGX_D3D12_Init_with_ProjectID failed ({:#x})", static_cast<uint32_t>(result));
            return false;
        }
        initialized_ = true;
        result = NVSDK_NGX_D3D12_GetCapabilityParameters(&params_);
        if (NVSDK_NGX_FAILED(result) || !params_) {
            reason = "NGX capability query failed";
            return false;
        }
        LogInfo("dlss d3d12: NGX initialised");
        return true;
    }

    bool RayReconstructionAvailable(std::string& reason) override {
        reason = rr_reason_;
        return rr_available_;
    }

    bool Create(VkCommandBuffer, const UpscaleCreate& create) override {
        ReleaseFeature();
        const auto native_list = ReShadeNative(ListOf(create.list));
        ID3D12GraphicsCommandList* list = native_list.Get();
        if (!params_ || !list) {
            return false;
        }
        bool ok = false;
        if (create.ray_reconstruction) {
            ok = CreateRayReconstruction(list, create);
        } else {
            const uint32_t preset = DlssPresetHint(create.dlss_model);
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
            static const bool auto_exposure = [] {
                const char* e = std::getenv("PT_DLSS_AUTO_EXPOSURE");
                return !e || std::atoi(e) != 0;
            }();
            if (auto_exposure) {
                p.InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
            }
            p.InEnableOutputSubrects = false;
            const NVSDK_NGX_Result result = NGX_D3D12_CREATE_DLSS_EXT(list, 1, 1, &feature_, params_, &p);
            ok = NVSDK_NGX_SUCCEED(result) && feature_;
            if (!ok) {
                LogError("dlss d3d12: feature creation failed ({:#x}) for {}x{} -> {}x{}", static_cast<uint32_t>(result), create.render.width,
                         create.render.height, create.display.width, create.display.height);
                feature_ = nullptr;
            } else {
                LogInfo("dlss d3d12: super resolution created for {}x{} -> {}x{}, quality value {}, model {} (render preset hint {})",
                        create.render.width, create.render.height, create.display.width, create.display.height,
                        static_cast<int>(p.Feature.InPerfQualityValue), DlssModelKey(create.dlss_model), preset);
            }
        }
        ResumeAfterNative(*create.list);
        return ok;
    }

    bool Dispatch(const UpscaleDispatch& d) override {
        const auto native_list = ReShadeNative(ListOf(d.list));
        ID3D12GraphicsCommandList* list = native_list.Get();
        if (!feature_ || !list) {
            return false;
        }
        const bool ok = ray_reconstruction_ ? DispatchRayReconstruction(list, d) : DispatchSuperResolution(list, d);
        ResumeAfterNative(*d.list);
        return ok;
    }

    void Release() override { ReleaseFeature(); }

private:
    bool DispatchSuperResolution(ID3D12GraphicsCommandList* list, const UpscaleDispatch& d) {
        NVSDK_NGX_D3D12_DLSS_Eval_Params e{};
        e.Feature.pInColor = Res(d.color);
        e.Feature.pInOutput = Res(d.output);
        e.pInDepth = Res(d.depth);
        e.pInMotionVectors = Res(d.motion);
        e.InJitterOffsetX = d.jitter.x;
        e.InJitterOffsetY = d.jitter.y;
        e.InRenderSubrectDimensions = {d.render.width, d.render.height};
        e.InReset = d.reset ? 1 : 0;
        e.InMVScaleX = d.motion_scale.x;
        e.InMVScaleY = d.motion_scale.y;
        e.pInExposureTexture = Res(d.exposure);
        e.pInBiasCurrentColorMask = Res(d.reactive);
        e.InPreExposure = d.pre_exposure;
        e.InExposureScale = 1.0f;
        e.InFrameTimeDeltaInMsec = d.frame_ms;
        const NVSDK_NGX_Result result = NGX_D3D12_EVALUATE_DLSS_EXT(list, feature_, params_, &e);
        if (NVSDK_NGX_FAILED(result)) {
            if (!error_logged_) {
                LogError("dlss d3d12: evaluate failed ({:#x})", static_cast<uint32_t>(result));
                error_logged_ = true;
            }
            return false;
        }
        return true;
    }

    void CheckRayReconstruction() {
        rr_available_ = false;
        if (!std::filesystem::exists(ExecutableDir() / L"nvngx_dlssd.dll")) {
            rr_reason_ = "nvngx_dlssd.dll is missing next to pt.exe";
            return;
        }
        int available = 0;
        NVSDK_NGX_Parameter_GetI(params_, NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &available);
        if (!available) {
            rr_reason_ = "DLSS Ray Reconstruction is not supported on this GPU";
            return;
        }
        rr_reason_.clear();
        rr_available_ = true;
    }

    bool CreateRayReconstruction(ID3D12GraphicsCommandList* list, const UpscaleCreate& create) {
        if (!rr_available_) {
            return false;
        }
        for (const char* mode : {NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA,
                                 NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality,
                                 NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced,
                                 NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance,
                                 NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance,
                                 NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality}) {
            NVSDK_NGX_Parameter_SetUI(params_, mode, NVSDK_NGX_RayReconstruction_Hint_Render_Preset_Default);
        }
        NVSDK_NGX_DLSSD_Create_Params p{};
        p.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
        p.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
        p.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;
        p.InWidth = create.render.width;
        p.InHeight = create.render.height;
        p.InTargetWidth = create.display.width;
        p.InTargetHeight = create.display.height;
        p.InPerfQualityValue = QualityValue(create.quality);
        p.InFeatureCreateFlags =
            NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
        p.InEnableOutputSubrects = false;
        const NVSDK_NGX_Result result = NGX_D3D12_CREATE_DLSSD_EXT(list, 1, 1, &feature_, params_, &p);
        if (NVSDK_NGX_FAILED(result) || !feature_) {
            LogError("dlss d3d12: ray reconstruction creation failed ({:#x})", static_cast<uint32_t>(result));
            feature_ = nullptr;
            return false;
        }
        ray_reconstruction_ = true;
        LogInfo("dlss d3d12: ray reconstruction created for {}x{} -> {}x{}", create.render.width, create.render.height, create.display.width,
                create.display.height);
        return true;
    }

    bool DispatchRayReconstruction(ID3D12GraphicsCommandList* list, const UpscaleDispatch& d) {
        if (!d.diffuse_albedo.Valid() || !d.specular_albedo.Valid() || !d.normal_roughness.Valid()) {
            if (!error_logged_) {
                LogError("dlss d3d12: ray reconstruction has no guide buffers this frame");
                error_logged_ = true;
            }
            return false;
        }
        NVSDK_NGX_D3D12_DLSSD_Eval_Params e{};
        e.pInColor = Res(d.color);
        e.pInOutput = Res(d.output);
        e.pInDepth = Res(d.depth);
        e.pInMotionVectors = Res(d.motion);
        e.pInDiffuseAlbedo = Res(d.diffuse_albedo);
        e.pInSpecularAlbedo = Res(d.specular_albedo);
        e.pInNormals = Res(d.normal_roughness);
        e.pInRoughness = nullptr;
        e.pInSpecularHitDistance = Res(d.specular_hit_distance);
        e.pInColorBeforeTransparency = Res(d.color_before_transparency);
        glm::mat4 world_to_view = d.world_to_view;
        glm::mat4 view_to_clip = d.view_to_clip;
        e.pInWorldToViewMatrix = &world_to_view[0][0];
        e.pInViewToClipMatrix = &view_to_clip[0][0];
        e.InJitterOffsetX = d.jitter.x;
        e.InJitterOffsetY = d.jitter.y;
        e.InRenderSubrectDimensions = {d.render.width, d.render.height};
        e.InReset = d.reset ? 1 : 0;
        e.InMVScaleX = d.motion_scale.x;
        e.InMVScaleY = d.motion_scale.y;
        e.pInExposureTexture = Res(d.exposure);
        e.InPreExposure = d.pre_exposure;
        e.InExposureScale = 1.0f;
        e.InFrameTimeDeltaInMsec = d.frame_ms;
        const NVSDK_NGX_Result result = NGX_D3D12_EVALUATE_DLSSD_EXT(list, feature_, params_, &e);
        if (NVSDK_NGX_FAILED(result)) {
            if (!error_logged_) {
                LogError("dlss d3d12: ray reconstruction evaluate failed ({:#x})", static_cast<uint32_t>(result));
                error_logged_ = true;
            }
            return false;
        }
        return true;
    }

    void ReleaseFeature() {
        if (feature_) {
            device_.WaitIdle();
            NVSDK_NGX_D3D12_ReleaseFeature(feature_);
            feature_ = nullptr;
        }
        ray_reconstruction_ = false;
    }

    void Shutdown() {
        ReleaseFeature();
        if (params_) {
            NVSDK_NGX_D3D12_DestroyParameters(params_);
            params_ = nullptr;
        }
        if (initialized_) {
            device_.WaitIdle();
            if (!streamline::ActiveD3D12()) {
                NVSDK_NGX_D3D12_Shutdown1(ngx_device_.Get());
            } else {
                LogInfo("dlss d3d12: NGX stays up for Streamline's DLSS Frame Generation (slShutdown ends it)");
            }
            initialized_ = false;
        }
        ngx_device_.Reset();
    }

    gpu::Device& device_;
    Microsoft::WRL::ComPtr<ID3D12Device> ngx_device_;
    bool initialized_ = false;
    unsigned long init_fault_ = 0;
    NVSDK_NGX_Parameter* params_ = nullptr;
    NVSDK_NGX_Handle* feature_ = nullptr;
    bool ray_reconstruction_ = false;
    bool rr_available_ = false;
    std::string rr_reason_ = "not checked";
    bool error_logged_ = false;
};

#endif

#if defined(PT_WITH_XESS)

struct XessD3D12Api {
    HMODULE module = nullptr;
    decltype(&xessGetVersion) GetVersion = nullptr;
    decltype(&xessD3D12CreateContext) CreateContext = nullptr;
    decltype(&xessD3D12Init) Init = nullptr;
    decltype(&xessD3D12Execute) Execute = nullptr;
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
        get(CreateContext, "xessD3D12CreateContext");
        get(Init, "xessD3D12Init");
        get(Execute, "xessD3D12Execute");
        get(DestroyContext, "xessDestroyContext");
        get(SetVelocityScale, "xessSetVelocityScale");
        get(SetLoggingCallback, "xessSetLoggingCallback");
        if (!GetVersion || !CreateContext || !Init || !Execute || !DestroyContext || !SetVelocityScale) {
            FreeLibrary(module);
            module = nullptr;
            return false;
        }
        return true;
    }
};

XessD3D12Api& Xess() {
    static XessD3D12Api api;
    return api;
}

void XessLog(const char* message, xess_logging_level_t level) {
    if (level >= XESS_LOGGING_LEVEL_ERROR) {
        LogError("xess d3d12: {}", message);
    } else if (level >= XESS_LOGGING_LEVEL_WARNING) {
        LogWarn("xess d3d12: {}", message);
    }
}

xess_quality_settings_t XessQuality(UpscaleQuality quality, VkExtent2D render, VkExtent2D display) {
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

class XessD3D12 final : public UpscaleBackend {
public:
    explicit XessD3D12(gpu::Device& device) : device_(device) {}
    ~XessD3D12() override { Release(); }

    UpscalerKind Kind() const override { return UpscalerKind::Xess; }

    bool Supported(std::string& reason) override {
        if (!std::filesystem::exists(ExecutableDir() / L"libxess.dll")) {
            reason = "libxess.dll is missing next to pt.exe";
            return false;
        }
        reason.clear();
        return true;
    }

    bool Available(std::string& reason) override {
        if (!Supported(reason)) {
            return false;
        }
        if (!Xess().Load()) {
            reason = "libxess.dll has no XeSS DirectX 12 exports";
            return false;
        }
        if (!MakeContext(reason)) {
            return false;
        }
        xess_version_t version{};
        Xess().GetVersion(&version);
        LogInfo("xess d3d12: XeSS {}.{}.{}", version.major, version.minor, version.patch);
        reason.clear();
        return true;
    }

    bool Create(VkCommandBuffer, const UpscaleCreate& create) override {
        std::string reason;
        if (!context_ && !MakeContext(reason)) {
            LogError("xess d3d12: {}", reason);
            return false;
        }
        device_.WaitIdle();
        xess_d3d12_init_params_t p{};
        p.outputResolution = {create.display.width, create.display.height};
        p.qualitySetting = XessQuality(create.quality, create.render, create.display);
        p.initFlags = XESS_INIT_FLAG_INVERTED_DEPTH | XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK;
        const xess_result_t result = Xess().Init(context_, &p);
        if (result != XESS_RESULT_SUCCESS) {
            LogError("xess d3d12: xessD3D12Init failed ({}) for {}x{} -> {}x{}", static_cast<int>(result), create.render.width, create.render.height,
                     create.display.width, create.display.height);
            return false;
        }
        Xess().SetVelocityScale(context_, static_cast<float>(create.render.width), static_cast<float>(create.render.height));
        LogInfo("xess d3d12: super resolution for {}x{} -> {}x{}, quality setting {}", create.render.width, create.render.height,
                create.display.width, create.display.height, static_cast<int>(p.qualitySetting));
        initialized_ = true;
        return true;
    }

    bool Dispatch(const UpscaleDispatch& d) override {
        ID3D12GraphicsCommandList* list = ListOf(d.list);
        if (!context_ || !initialized_ || !list) {
            return false;
        }
        xess_d3d12_execute_params_t e{};
        e.pColorTexture = Res(d.color);
        e.pVelocityTexture = Res(d.motion);
        e.pDepthTexture = Res(d.depth);
        e.pResponsivePixelMaskTexture = Res(d.reactive);
        e.pOutputTexture = Res(d.output);
        e.jitterOffsetX = d.jitter.x;
        e.jitterOffsetY = d.jitter.y;
        e.exposureScale = 1.0f;
        e.resetHistory = d.reset ? 1u : 0u;
        e.inputWidth = d.render.width;
        e.inputHeight = d.render.height;
        const xess_result_t result = Xess().Execute(context_, list, &e);
        ResumeAfterNative(*d.list);
        if (result != XESS_RESULT_SUCCESS) {
            if (!error_logged_) {
                LogError("xess d3d12: xessD3D12Execute failed ({})", static_cast<int>(result));
                error_logged_ = true;
            }
            return false;
        }
        return true;
    }

    void Release() override {
        if (context_) {
            device_.WaitIdle();
            Xess().DestroyContext(context_);
            context_ = nullptr;
        }
        initialized_ = false;
    }

private:
    bool MakeContext(std::string& reason) {
        const xess_result_t result = Xess().CreateContext(NativeDevice(device_), &context_);
        if (result != XESS_RESULT_SUCCESS || !context_) {
            reason = result == XESS_RESULT_ERROR_UNSUPPORTED_DRIVER ? "the driver is too old for XeSS" : "XeSS does not support this GPU";
            LogWarn("xess d3d12: xessD3D12CreateContext failed ({})", static_cast<int>(result));
            context_ = nullptr;
            return false;
        }
        if (Xess().SetLoggingCallback) {
            Xess().SetLoggingCallback(context_, XESS_LOGGING_LEVEL_WARNING, XessLog);
        }
        return true;
    }

    gpu::Device& device_;
    xess_context_handle_t context_ = nullptr;
    bool initialized_ = false;
    bool error_logged_ = false;
};

#endif

#if defined(PT_WITH_FSR)

void FsrMessage(uint32_t type, const wchar_t* message) {
    const std::string text = NarrowText(message);
    if (type == FFX_API_MESSAGE_TYPE_ERROR) {
        LogError("fsr d3d12: {}", text);
    } else {
        LogWarn("fsr d3d12: {}", text);
    }
}

constexpr uint32_t kAmdVendorId = 0x1002;
constexpr const wchar_t* kFsrLoader = L"amd_fidelityfx_loader_dx12.dll";
constexpr const wchar_t* kFsrUpscaler = L"amd_fidelityfx_upscaler_dx12.dll";

uint32_t FfxState(D3D12_RESOURCE_STATES state) {
    if (state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        return FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    const bool pixel = (state & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) != 0;
    const bool compute = (state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0;
    if (pixel && compute) {
        return FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
    }
    if (pixel) {
        return FFX_API_RESOURCE_STATE_PIXEL_READ;
    }
    if (compute) {
        return FFX_API_RESOURCE_STATE_COMPUTE_READ;
    }
    if (state & D3D12_RESOURCE_STATE_COPY_SOURCE) {
        return FFX_API_RESOURCE_STATE_COPY_SRC;
    }
    if (state & D3D12_RESOURCE_STATE_COPY_DEST) {
        return FFX_API_RESOURCE_STATE_COPY_DEST;
    }
    return FFX_API_RESOURCE_STATE_COMMON;
}

FfxApiResource FfxResource(const UpscaleImage& image, uint32_t extra_usage = 0) {
    if (!image.handle) {
        return FfxApiResource{};
    }
    return ffxApiGetResourceDX12(Native(image.handle), FfxState(TrackedState(image.handle)), extra_usage);
}

class FsrD3D12 final : public UpscaleBackend {
public:
    FsrD3D12(gpu::Device& device, int generation) : device_(device), generation_(generation), rr_(device) {}
    ~FsrD3D12() override {
        Release();
        if (module_) {
            FreeLibrary(module_);
        }
    }

    UpscalerKind Kind() const override { return generation_ == 4 ? UpscalerKind::Fsr4 : UpscalerKind::Fsr; }

    bool Supported(std::string& reason) override {
        if (generation_ == 4 && device_.Info().vendor_id != kAmdVendorId && !probed_) {
            reason = kFsr4ReasonNotAmd;
            return false;
        }
        return FilesPresent(reason);
    }

    bool FilesPresent(std::string& reason) {
        for (const wchar_t* file : {kFsrLoader, kFsrUpscaler}) {
            if (!std::filesystem::exists(ExecutableDir() / file)) {
                reason = NarrowText(file) + " is missing next to pt.exe";
                return false;
            }
        }
        if (probed_ && !version_id_) {
            reason = reason_;
            return false;
        }
        reason.clear();
        return true;
    }

    bool Available(std::string& reason) override {
        if (!FilesPresent(reason)) {
            return false;
        }
        if (!probed_) {
            probed_ = true;
            Probe(reason_);
        }
        reason = reason_;
        return version_id_ != 0;
    }

    bool RayReconstructionAvailable(std::string& reason) override {
        return generation_ == 4 && rr_.Available(reason);
    }
    bool Denoise(const RayDenoiseDispatch& d) override { return rr_.Dispatch(d); }
    void DisableRayReconstruction(const std::string& reason) override { rr_.Disable(reason); }

    bool Create(VkCommandBuffer, const UpscaleCreate& create) override {
        Release();
        const char* input_check = std::getenv("PT_FSR_RR_INPUT_CHECK");
        const bool stand_in = input_check && input_check[0] == '2';
        if ((create.ray_reconstruction || stand_in) && !rr_.Create(create.render)) return false;
        ffxCreateBackendDX12Desc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = NativeDevice(device_);
        ffxCreateContextDescUpscale desc{};
        desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        desc.header.pNext = &backend.header;
#if defined(PT_WITH_FSR_RR)
        ffxCreateContextDescUpscaleVersion api_version{};
        api_version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
        api_version.version = FFX_UPSCALER_VERSION;
        api_version.header.pNext = desc.header.pNext;
        desc.header.pNext = &api_version.header;
#endif
        desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
        if (std::getenv("PT_UPSCALE_CHECK")) {
            desc.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
        }
        desc.maxRenderSize = {create.render.width, create.render.height};
        desc.maxUpscaleSize = {create.display.width, create.display.height};
        desc.fpMessage = FsrMessage;
        ffxOverrideVersion version_override{};
        version_override.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        version_override.versionId = version_id_;
        backend.header.pNext = &version_override.header;
        const ffxReturnCode_t result = fn_.CreateContext(&context_, &desc.header, nullptr);
        if (result != FFX_API_RETURN_OK) {
            LogError("fsr d3d12: ffxCreateContext failed ({}) for FSR {}", result, version_name_);
            context_ = nullptr;
            if (generation_ == 4) {
                reason_ = kFsr4ReasonDriver;
                version_id_ = 0;
                LogWarn("fsr d3d12: FSR 4 not offered: {}", reason_);
            }
            return false;
        }
        ffxQueryGetProviderVersion version{};
        version.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        const bool named = fn_.Query(&context_, &version.header) == FFX_API_RETURN_OK && version.versionName;
        const std::string created = named ? version.versionName : "";
        if (generation_ == 4 && named && !FsrCreatedIsGeneration(created, 4)) {
            LogWarn("fsr d3d12: the FSR {} context runs upscaler {}, not FSR 4", version_name_, created);
            fn_.DestroyContext(&context_, nullptr);
            context_ = nullptr;
            reason_ = kFsr4ReasonDriver;
            version_id_ = 0;
            LogWarn("fsr d3d12: FSR 4 not offered: {}", reason_);
            return false;
        }
        if (named) {
            LogInfo("fsr d3d12: upscaler {} created for {}x{} -> {}x{}", created, create.render.width, create.render.height,
                    create.display.width, create.display.height);
        }
        return true;
    }

    bool Dispatch(const UpscaleDispatch& d) override {
        ID3D12GraphicsCommandList* list = ListOf(d.list);
        if (!context_ || !list) {
            return false;
        }
        ffxDispatchDescUpscale u{};
        u.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        u.commandList = list;
        u.color = FfxResource(d.color);
        u.depth = FfxResource(d.depth, FFX_API_RESOURCE_USAGE_DEPTHTARGET);
        u.motionVectors = FfxResource(d.motion);
        u.exposure = FfxResource(d.exposure);
        u.reactive = FfxResource(d.reactive);
        u.transparencyAndComposition = FfxResource(d.transparency);
        u.output = FfxResource(d.output, FFX_API_RESOURCE_USAGE_UAV);
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
        u.flags = std::getenv("PT_UPSCALE_DEBUG") ? FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW : 0u;
        const ffxReturnCode_t result = fn_.Dispatch(&context_, &u.header);
        ResumeAfterNative(*d.list);
        if (result != FFX_API_RETURN_OK) {
            if (!dispatch_error_logged_) {
                LogError("fsr d3d12: ffxDispatch failed ({})", result);
                dispatch_error_logged_ = true;
            }
            return false;
        }
        return true;
    }

    void Release() override {
        rr_.Release();
        if (context_) {
            device_.WaitIdle();
            fn_.DestroyContext(&context_, nullptr);
            context_ = nullptr;
        }
    }

private:
    bool Probe(std::string& reason) {
        const std::filesystem::path path = ExecutableDir() / kFsrLoader;
        module_ = module_ ? module_ : LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module_) {
            reason = std::format("{} cannot be loaded (error {})", NarrowText(kFsrLoader), GetLastError());
            return false;
        }
        ffxLoadFunctions(&fn_, module_);
        if (!fn_.CreateContext || !fn_.DestroyContext || !fn_.Dispatch || !fn_.Query || !fn_.Configure) {
            reason = NarrowText(kFsrLoader) + " has no FidelityFX API exports";
            return false;
        }
        FsrGateInput gate;
        gate.generation = generation_;
        gate.api = FsrApi::D3D12;
        gate.vendor_id = device_.Info().vendor_id;
        uint64_t count = 0;
        ffxQueryDescGetVersions versions{};
        versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        versions.device = NativeDevice(device_);
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
        LogInfo("fsr d3d12: amd_fidelityfx_upscaler_dx12.dll upscalers for this GPU (vendor 0x{:04x}): {}", gate.vendor_id, decision.listed);
        if (!decision.available) {
            version_id_ = 0;
            reason = generation_ == 4 ? decision.reason
                     : !gate.query_ok || gate.providers.empty() ? "the AMD FSR API lists no upscaler for this GPU"
                                                                : "the AMD FSR API lists no FSR 3 upscaler for this GPU";
            if (generation_ == 4) LogInfo("fsr d3d12: FSR 4 not offered: {}", reason);
            return false;
        }
        version_id_ = decision.id;
        version_name_ = decision.name;
        if (generation_ == 4) LogInfo("fsr d3d12: FSR 4 offered with upscaler provider {}", version_name_);
        reason.clear();
        return true;
    }

    gpu::Device& device_;
    FsrRayRegeneration rr_;
    int generation_ = 3;
    bool probed_ = false;
    std::string reason_;
    uint64_t version_id_ = 0;
    std::string version_name_;
    HMODULE module_ = nullptr;
    ffxFunctions fn_{};
    ffxContext context_ = nullptr;
    bool dispatch_error_logged_ = false;
};

#endif

}

std::unique_ptr<UpscaleBackend> CreateUpscaleBackend(UpscalerKind kind, gpu::Device& device) {
    switch (kind) {
#if defined(PT_WITH_DLSS)
    case UpscalerKind::Dlss: return std::make_unique<DlssD3D12>(device);
#endif
#if defined(PT_WITH_XESS)
    case UpscalerKind::Xess: return std::make_unique<XessD3D12>(device);
#endif
#if defined(PT_WITH_FSR)
    case UpscalerKind::Fsr: return std::make_unique<FsrD3D12>(device, 3);
    case UpscalerKind::Fsr4: return std::make_unique<FsrD3D12>(device, 4);
#endif
    default: break;
    }
    (void)device;
    return nullptr;
}

}

#endif
