#include "engine/render/d3d12/backend/fsr_ray_regeneration.h"
#include "engine/render/d3d12/backend/d3d12_native.h"
#include "engine/core/log.h"
#include "engine/render/upscale/upscale_platform.h"
#include <cstring>
#include <cstdlib>
#include <format>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

#if defined(PT_WITH_FSR_RR)
#include <windows.h>
#include <ffx_api_loader.h>
#include <dx12/ffx_api_dx12.h>
#include <ffx_denoiser.h>
#endif

namespace pt::gpu::d3d12 {

#if defined(PT_WITH_FSR_RR)
namespace {

void DenoiserMessage(uint32_t type, const wchar_t* message) {
    static std::mutex lock;
    static std::set<std::string> seen;
    const std::string text = message ? NarrowText(message) : std::string();
    {
        std::lock_guard guard(lock);
        if (seen.size() >= 64 || !seen.insert(text).second) {
            return;
        }
    }
    if (type == FFX_API_MESSAGE_TYPE_ERROR) LogError("fsr rr: AMD: {}", text);
    else LogWarn("fsr rr: AMD: {}", text);
}

std::string FileVersion(const std::wstring& path) {
    HMODULE module = LoadLibraryW(L"version.dll");
    if (!module) return "no version";
    using SizeFn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
    using InfoFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
    using ValueFn = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    const auto size_fn = reinterpret_cast<SizeFn>(GetProcAddress(module, "GetFileVersionInfoSizeW"));
    const auto info_fn = reinterpret_cast<InfoFn>(GetProcAddress(module, "GetFileVersionInfoW"));
    const auto value_fn = reinterpret_cast<ValueFn>(GetProcAddress(module, "VerQueryValueW"));
    std::string out = "no version";
    DWORD ignored = 0;
    const DWORD size = size_fn ? size_fn(path.c_str(), &ignored) : 0;
    if (size && info_fn && value_fn) {
        std::vector<uint8_t> data(size);
        VS_FIXEDFILEINFO* fixed = nullptr;
        UINT length = 0;
        if (info_fn(path.c_str(), 0, size, data.data()) && value_fn(data.data(), L"\\", reinterpret_cast<LPVOID*>(&fixed), &length) && fixed) {
            out = std::format("{}.{}.{}.{}", HIWORD(fixed->dwFileVersionMS), LOWORD(fixed->dwFileVersionMS), HIWORD(fixed->dwFileVersionLS), LOWORD(fixed->dwFileVersionLS));
        }
    }
    FreeLibrary(module);
    return out;
}

void LogLibrary(const wchar_t* name, bool driver) {
    std::wstring path;
    if (driver) {
        wchar_t buffer[MAX_PATH * 2] = {};
        HMODULE loaded = GetModuleHandleW(name);
        if (!loaded || !GetModuleFileNameW(loaded, buffer, static_cast<DWORD>(std::size(buffer)))) {
            LogInfo("fsr rr: {} not loaded", NarrowText(name));
            return;
        }
        path = buffer;
    } else {
        path = (ExecutableDir() / name).wstring();
    }
    LogInfo("fsr rr: {} version {} at {}", NarrowText(name), FileVersion(path), NarrowText(path.c_str()));
}

void LogResource(const std::string& name, const FfxApiResource& r, D3D12_RESOURCE_STATES state) {
    if (!r.resource) {
        LogWarn("fsr rr: input {}: missing", name);
        return;
    }
    const uint32_t f = r.description.format;
    const std::string format = f == FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT ? std::string("RGBA16F") : f == FFX_API_SURFACE_FORMAT_R32_FLOAT ? std::string("R32F") : std::format("format {}", f);
    LogInfo("fsr rr: input {}: {}x{} {} (ffx state {:#x}, d3d12 state {:#x}, usage {:#x})", name, r.description.width, r.description.height, format, r.state,
            static_cast<uint32_t>(state), r.description.usage);
}

}
#endif

struct FsrRayRegeneration::Impl {
    explicit Impl(gpu::Device& d) : device(d) {}
    gpu::Device& device;
    std::string reason;
    bool checked = false;
    bool supported = false;
    bool failed = false;
    bool dispatched = false;
#if defined(PT_WITH_FSR_RR)
    HMODULE module = nullptr;
    ffxFunctions fn{};
    ffxContext context = nullptr;
    uint64_t version = 0;
    std::string version_name;
    glm::vec3 previous_eye{};
    uint32_t previous_frame = 0;

    FfxApiResource Resource(const UpscaleImage& image, bool output = false) {
        if (!image.handle) return {};
        const auto state = TrackedState(image.handle);
        uint32_t ffx_state = FFX_API_RESOURCE_STATE_COMMON;
        if (state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) ffx_state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
        else if ((state & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) && (state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
            ffx_state = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
        else if (state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) ffx_state = FFX_API_RESOURCE_STATE_COMPUTE_READ;
        else if (state & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) ffx_state = FFX_API_RESOURCE_STATE_PIXEL_READ;
        return ffxApiGetResourceDX12(Native(image.handle), ffx_state, output ? FFX_API_RESOURCE_USAGE_UAV : 0);
    }
#endif
};

FsrRayRegeneration::FsrRayRegeneration(gpu::Device& device) : impl_(std::make_unique<Impl>(device)) {}
FsrRayRegeneration::~FsrRayRegeneration() {
    Release();
#if defined(PT_WITH_FSR_RR)
    if (impl_->module) FreeLibrary(impl_->module);
#endif
}

bool FsrRayRegeneration::Available(std::string& reason) {
    auto& p = *impl_;
    if (!p.checked) {
        p.checked = true;
        p.reason = "FSR Ray Regeneration needs an AMD Radeon GPU with DirectX 12";
#if defined(PT_WITH_FSR_RR)
        const char* input_check = std::getenv("PT_FSR_RR_INPUT_CHECK");
        if (p.device.Info().vendor_id == 0x1002 || (input_check && input_check[0] == '2')) {
            p.reason = "AMD's driver offers no FSR Ray Regeneration for this GPU (AMD lists the Radeon RX 9000 series and Windows 11)";
            const auto path = ExecutableDir() / L"amd_fidelityfx_loader_dx12.dll";
            if (!std::filesystem::exists(ExecutableDir() / L"amd_fidelityfx_denoiser_dx12.dll")) {
                p.reason = "amd_fidelityfx_denoiser_dx12.dll is missing next to pt.exe";
            } else {
                p.module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                if (p.module) {
                    ffxLoadFunctions(&p.fn, p.module);
                    if (p.fn.Query && p.fn.CreateContext && p.fn.DestroyContext && p.fn.Dispatch) {
                        LogInfo("fsr rr: {} (vendor {:04x} device {:04x})", p.device.Info().adapter, p.device.Info().vendor_id, p.device.Info().device_id);
                        LogLibrary(L"amd_fidelityfx_loader_dx12.dll", false);
                        LogLibrary(L"amd_fidelityfx_denoiser_dx12.dll", false);
                        uint64_t count = 0;
                        ffxQueryDescGetVersions q{};
                        q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
                        q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER;
                        q.device = NativeDevice(p.device);
                        q.outputCount = &count;
                        const auto counted = p.fn.Query(nullptr, &q.header);
                        if (counted != FFX_API_RETURN_OK || !count) {
                            LogInfo("fsr rr: amd_fidelityfx_denoiser_dx12.dll providers for this GPU: none (query {})", counted);
                        }
                        if (counted == FFX_API_RETURN_OK && count && count < 256) {
                            std::vector<uint64_t> ids(count);
                            std::vector<const char*> names(count);
                            q.versionIds = ids.data(); q.versionNames = names.data();
                            if (p.fn.Query(nullptr, &q.header) == FFX_API_RETURN_OK) {
                                std::string listed;
                                for (uint64_t i = 0; i < count; ++i) listed += std::format("{}{}", i ? ", " : "", names[i] ? names[i] : "?");
                                LogInfo("fsr rr: amd_fidelityfx_denoiser_dx12.dll providers for this GPU: {}", listed);
                                p.version = ids[0];
                                p.version_name = names[0] ? names[0] : "1.2.0";
                                p.supported = true;
                                p.reason.clear();
                            }
                        }
                    }
                } else p.reason = std::format("FSR Ray Regeneration loader failed ({})", GetLastError());
            }
        }
#else
        p.reason = "FSR Ray Regeneration was not built";
#endif
        LogInfo("fsr rr: {}", p.supported ? "provider available, checked again at context creation" : p.reason);
    }
    reason = p.reason;
    return p.supported && !p.failed;
}

bool FsrRayRegeneration::Create(VkExtent2D size) {
    Release();
    auto& p = *impl_;
    std::string reason;
    if (!Available(reason)) return false;
#if defined(PT_WITH_FSR_RR)
    ffxCreateContextDescDenoiser c{};
    c.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER;
    c.version = FFX_DENOISER_VERSION;
    c.maxRenderSize = {size.width, size.height};
    c.signalFlags = FFX_DENOISER_SIGNAL_DIRECT_DIFFUSE | FFX_DENOISER_SIGNAL_DIRECT_SPECULAR |
                    FFX_DENOISER_SIGNAL_INDIRECT_DIFFUSE | FFX_DENOISER_SIGNAL_INDIRECT_SPECULAR;
    const char* validate = std::getenv("PT_FSR_RR_VALIDATE");
    const bool validating = !validate || validate[0] != '0';
    if (validating) c.flags = FFX_DENOISER_ENABLE_VALIDATION;
    if (p.fn.Configure) {
        ffxConfigureDescGlobalDebug debug{};
        debug.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
        debug.effectId = FFX_API_EFFECT_ID_DENOISER;
        debug.fpMessage = DenoiserMessage;
        debug.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_VERBOSE;
        const auto configured = p.fn.Configure(nullptr, &debug.header);
        if (configured != FFX_API_RETURN_OK) LogWarn("fsr rr: AMD's messages not connected ({})", configured);
    }
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = NativeDevice(p.device);
    ffxOverrideVersion version{};
    version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    version.versionId = p.version;
    c.header.pNext = &backend.header;
    backend.header.pNext = &version.header;
    const auto result = p.fn.CreateContext(&p.context, &c.header, nullptr);
    if (result != FFX_API_RETURN_OK || !p.context) {
        p.reason = std::format("FSR Ray Regeneration context creation failed ({})", result);
        p.failed = true;
        if (p.context) p.fn.DestroyContext(&p.context, nullptr);
        p.context = nullptr;
        LogWarn("fsr rr: {}; reverting to the port denoiser", p.reason);
        return false;
    }
    p.dispatched = false;
    LogInfo("fsr rr: Ray Regeneration {} (id {:#x}, built against {:#x}) created at {}x{}, four radiance signals, validation {}", p.version_name, p.version, FFX_DENOISER_VERSION, size.width,
            size.height, validating ? "on" : "off");
    LogLibrary(L"amdxc64.dll", true);
    LogLibrary(L"amdxcffx64.dll", true);
    const std::pair<uint64_t, const char*> keys[] = {
        {FFX_API_CONFIGURE_DENOISER_KEY_MAX_RADIANCE, "max radiance"}, {FFX_API_CONFIGURE_DENOISER_KEY_RADIANCE_CLIP_STD_K, "radiance clip k"},
        {FFX_API_CONFIGURE_DENOISER_KEY_STABILITY_BIAS, "stability bias"}, {FFX_API_CONFIGURE_DENOISER_KEY_DISOCCLUSION_THRESHOLD, "disocclusion threshold"},
        {FFX_API_CONFIGURE_DENOISER_KEY_CROSS_BILATERAL_NORMAL_STRENGTH, "normal strength"}, {FFX_API_CONFIGURE_DENOISER_KEY_GAUSSIAN_KERNEL_RELAXATION, "kernel relaxation"}};
    std::string defaults;
    for (const auto& [key, name] : keys) {
        float value = 0.0f;
        ffxQueryDescDenoiserGetDefaultKeyValue q{};
        q.header.type = FFX_API_QUERY_DESC_TYPE_DENOISER_GET_DEFAULT_KEYVALUE;
        q.key = key;
        q.count = 1;
        q.data = &value;
        const auto queried = p.fn.Query(&p.context, &q.header);
        defaults += std::format("{}{} ", defaults.empty() ? "" : ", ", name);
        defaults += queried == FFX_API_RETURN_OK ? std::format("{}", value) : std::format("query failed ({})", queried);
    }
    LogInfo("fsr rr: AMD's defaults: {}", defaults);
    FfxApiEffectMemoryUsage memory{};
    ffxQueryDescDenoiserGetGPUMemoryUsage usage{};
    usage.header.type = FFX_API_QUERY_DESC_TYPE_DENOISER_GPU_MEMORY_USAGE;
    usage.device = NativeDevice(p.device);
    usage.maxRenderSize = c.maxRenderSize;
    usage.signalFlags = c.signalFlags;
    usage.flags = c.flags;
    usage.gpuMemoryUsage = &memory;
    if (p.fn.Query(&p.context, &usage.header) == FFX_API_RETURN_OK) {
        LogInfo("fsr rr: AMD's video memory {} MB, {} MB of it aliasable", memory.totalUsageInBytes >> 20, memory.aliasableUsageInBytes >> 20);
    }
    return true;
#else
    return false;
#endif
}

bool FsrRayRegeneration::Dispatch(const RayDenoiseDispatch& d) {
#if defined(PT_WITH_FSR_RR)
    auto& p = *impl_;
    if (!p.context || p.failed || !d.list) return false;
    ffxDispatchDescDenoiser r{};
    r.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER;
    r.commandList = Native(*d.list);
    r.linearDepth = p.Resource(d.depth);
    r.motionVectors = p.Resource(d.motion);
    r.normals = p.Resource(d.normal_roughness);
    r.diffuseAlbedo = p.Resource(d.diffuse_albedo);
    r.specularAlbedo = p.Resource(d.specular_albedo);
    r.motionVectorScale = {1.0f, 1.0f, 1.0f};
    r.jitterOffsets = {d.jitter.x, d.jitter.y};
    const bool reset = d.reset || !p.dispatched || d.frame_index != p.previous_frame + 1u ||
                       glm::distance(p.previous_eye, d.eye) > 2.0f;
    const glm::vec3 delta = reset ? glm::vec3(0.0f) : p.previous_eye - d.eye;
    r.cameraPositionDelta = {delta.x, delta.y, delta.z};
    std::memcpy(&r.view, &d.world_to_view, sizeof(r.view));
    std::memcpy(&r.projection, &d.view_to_clip, sizeof(r.projection));
    r.linearDepthBounds = {d.near_plane, 200.0f};
    r.renderSize = {d.render.width, d.render.height};
    r.frameIndex = d.frame_index;
    r.flags = FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO | (reset ? FFX_DENOISER_DISPATCH_RESET : 0u);
    ffxDispatchDescDenoiserDirectDiffuse dd{};
    ffxDispatchDescDenoiserDirectSpecular ds{};
    ffxDispatchDescDenoiserIndirectDiffuse id{};
    ffxDispatchDescDenoiserIndirectSpecular is{};
    dd.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_DIFFUSE;
    ds.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_SPECULAR;
    id.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_DIFFUSE;
    is.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR;
    r.header.pNext = &dd.header; dd.header.pNext = &ds.header; ds.header.pNext = &id.header; id.header.pNext = &is.header;
    FfxApiDenoiserSignal* signals[] = {&dd.signal, &ds.signal, &id.signal, &is.signal};
    for (unsigned i = 0; i < 4; ++i) {
        signals[i]->input = p.Resource(d.input[i]);
        signals[i]->output = p.Resource(d.output[i], true);
    }
    if (!p.dispatched) {
        LogResource("linear depth", r.linearDepth, TrackedState(d.depth.handle));
        LogResource("motion vectors", r.motionVectors, TrackedState(d.motion.handle));
        LogResource("normals", r.normals, TrackedState(d.normal_roughness.handle));
        LogResource("diffuse albedo", r.diffuseAlbedo, TrackedState(d.diffuse_albedo.handle));
        LogResource("specular albedo", r.specularAlbedo, TrackedState(d.specular_albedo.handle));
        static const char* const kSignals[] = {"direct diffuse", "direct specular", "indirect diffuse", "indirect specular"};
        for (unsigned i = 0; i < 4; ++i) {
            LogResource(std::format("{} signal", kSignals[i]), signals[i]->input, TrackedState(d.input[i].handle));
            LogResource(std::format("{} output", kSignals[i]), signals[i]->output, TrackedState(d.output[i].handle));
        }
        const glm::mat4& proj = d.view_to_clip;
        LogInfo("fsr rr: dispatch render {}x{}, jitter {} {}, depth bounds {} to {}, motion scale {} {} {}, flags {:#x}, frame {}", r.renderSize.width, r.renderSize.height, r.jitterOffsets.x,
                r.jitterOffsets.y, r.linearDepthBounds.min, r.linearDepthBounds.max, r.motionVectorScale.x, r.motionVectorScale.y, r.motionVectorScale.z, r.flags, r.frameIndex);
        LogInfo("fsr rr: projection x {} y {} z {} {} w {}, view forward {} {} {}", proj[0][0], proj[1][1], proj[2][2], proj[3][2], proj[2][3], d.world_to_view[0][2], d.world_to_view[1][2],
                d.world_to_view[2][2]);
    }
    const auto result = p.fn.Dispatch(&p.context, &r.header);
    ResumeAfterNative(*d.list);
    if (result != FFX_API_RETURN_OK) {
        p.failed = true;
        p.reason = std::format("FSR Ray Regeneration dispatch failed ({})", result);
        LogError("fsr rr: {}; next frame uses the port denoiser", p.reason);
        return false;
    }
    if (!p.dispatched) LogInfo("fsr rr: first denoising dispatch succeeded");
    p.dispatched = true;
    p.previous_eye = d.eye;
    p.previous_frame = d.frame_index;
    return true;
#else
    return false;
#endif
}

void FsrRayRegeneration::Release() {
#if defined(PT_WITH_FSR_RR)
    auto& p = *impl_;
    if (p.context) {
        p.device.WaitIdle();
        p.fn.DestroyContext(&p.context, nullptr);
        p.context = nullptr;
    }
    p.dispatched = false;
#endif
}

void FsrRayRegeneration::Disable(const std::string& reason) {
    impl_->failed = true;
    impl_->reason = reason;
    LogWarn("fsr rr: {}; reverting to the port denoiser", reason);
}

}
