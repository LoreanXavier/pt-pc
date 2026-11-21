#include "engine/render/d3d12/present/d3d12_device.h"

#ifdef _WIN32

#include <cstdlib>
#include <wrl/client.h>
#include <cstring>
#include <format>

#include "engine/core/log.h"
#include "engine/render/upscale/upscale_platform.h"

namespace pt::d3d12 {
namespace {

using CreateFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);

std::string HResult(HRESULT hr) {
    return std::format("{:#010x}", static_cast<uint32_t>(hr));
}

const char* DxgiName(HRESULT hr) {
    switch (static_cast<uint32_t>(hr)) {
        case 0x887a0005: return "DXGI_ERROR_DEVICE_REMOVED";
        case 0x887a0006: return "DXGI_ERROR_DEVICE_HUNG";
        case 0x887a0007: return "DXGI_ERROR_DEVICE_RESET";
        case 0x887a0020: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
        case 0x887a0001: return "DXGI_ERROR_INVALID_CALL";
        case 0x887a0026: return "DXGI_ERROR_ACCESS_LOST";
        case 0x887a002b: return "DXGI_ERROR_ACCESS_DENIED";
        default: return nullptr;
    }
}

}

uint32_t WindowsBuild() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto get = ntdll ? reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion"))) : nullptr;
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (!get || get(&info) != 0) {
        return 0;
    }
    return info.dwBuildNumber;
}

bool Device::Create(const uint8_t (&luid)[8], std::string& error) {
    DeviceOptions options;
    options.luid = luid;
    return Create(options, error);
}

bool Device::Create(const DeviceOptions& options, std::string& error) {
    Destroy();
    d3d12_ = LoadLibraryW(L"d3d12.dll");
    dxgi_ = LoadLibraryW(L"dxgi.dll");
    if (!d3d12_ || !dxgi_) {
        error = "d3d12.dll or dxgi.dll could not be loaded";
        Destroy();
        return false;
    }
    auto create_factory = reinterpret_cast<CreateFactory2>(reinterpret_cast<void*>(GetProcAddress(dxgi_, "CreateDXGIFactory2")));
    auto create_device = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(reinterpret_cast<void*>(GetProcAddress(d3d12_, "D3D12CreateDevice")));
    auto get_debug = reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(reinterpret_cast<void*>(GetProcAddress(d3d12_, "D3D12GetDebugInterface")));
    if (!create_factory || !create_device) {
        error = "d3d12.dll or dxgi.dll lacks its entry points";
        Destroy();
        return false;
    }
    create_factory_ = reinterpret_cast<void*>(create_factory);
    const bool debug = options.debug || std::getenv("PT_D3D12_DEBUG") != nullptr;
    if (debug && get_debug) {
        ComPtr<ID3D12Debug> layer;
        if (const HRESULT dhr = get_debug(IID_PPV_ARGS(&layer)); SUCCEEDED(dhr)) {
            layer->EnableDebugLayer();
            const bool gpu_validation = std::getenv("PT_D3D12_GPU_VALIDATION") != nullptr;
            ComPtr<ID3D12Debug1> layer1;
            if (gpu_validation && SUCCEEDED(layer.As(&layer1))) {
                layer1->SetEnableGPUBasedValidation(TRUE);
            }
            LogInfo("d3d12: debug layer on{}", gpu_validation ? (layer1 ? ", GPU-based validation on" : ", GPU-based validation unavailable") : "");
        } else {
            LogWarn("d3d12: PT_D3D12_DEBUG set but the debug layer is not installed (Windows optional feature Graphics Tools, or "
                    "d3d12SDKLayers.dll next to the Agility SDK's D3D12Core.dll): {}", HResult(dhr));
        }
    }
    const char* dred_env = std::getenv("PT_D3D12_DRED");
    const bool dred_off = dred_env && dred_env[0] == '0';
    dred_breadcrumbs_ = !dred_off && (options.dred || dred_env != nullptr);
    if (!dred_off && get_debug) {
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> settings;
        if (SUCCEEDED(get_debug(IID_PPV_ARGS(&settings)))) {
            settings->SetAutoBreadcrumbsEnablement(dred_breadcrumbs_ ? D3D12_DRED_ENABLEMENT_FORCED_ON : D3D12_DRED_ENABLEMENT_SYSTEM_CONTROLLED);
            settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred_ = true;
        } else {
            LogWarn("d3d12: DRED could not be enabled");
        }
    }
    HRESULT hr = create_factory(debug ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&factory_));
    if (FAILED(hr) && debug) {
        hr = create_factory(0, IID_PPV_ARGS(&factory_));
    }
    if (FAILED(hr)) {
        error = "CreateDXGIFactory2 failed (" + HResult(hr) + ")";
        Destroy();
        return false;
    }
    LUID id{};
    if (options.luid) {
        std::memcpy(&id, options.luid, sizeof(id));
        hr = factory_->EnumAdapterByLuid(id, IID_PPV_ARGS(&adapter_));
        if (FAILED(hr)) {
            error = "no DXGI adapter has the Vulkan device's LUID (" + HResult(hr) + ")";
            Destroy();
            return false;
        }
    } else {
        ComPtr<IDXGIFactory6> factory6;
        factory_.As(&factory6);
        const char* pick = std::getenv("PT_D3D12_ADAPTER");
        const int wanted = pick ? std::atoi(pick) : -1;
        int usable = 0;
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> candidate;
            hr = factory6 ? factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate))
                          : factory_->EnumAdapters1(i, &candidate);
            if (FAILED(hr)) {
                break;
            }
            DXGI_ADAPTER_DESC1 d{};
            candidate->GetDesc1(&d);
            if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || d.VendorId == 0x1414 ||
                FAILED(create_device(candidate.Get(), options.feature_level, __uuidof(ID3D12Device), nullptr))) {
                continue;
            }
            if (wanted < 0 || usable == wanted) {
                adapter_ = candidate;
                break;
            }
            ++usable;
        }
        if (!adapter_) {
            error = std::format("no hardware adapter has a DirectX 12 device at feature level {:#x}", static_cast<uint32_t>(options.feature_level));
            Destroy();
            return false;
        }
        DXGI_ADAPTER_DESC1 d{};
        adapter_->GetDesc1(&d);
        id = d.AdapterLuid;
    }
    DXGI_ADAPTER_DESC1 desc{};
    adapter_->GetDesc1(&desc);
    adapter_name_ = NarrowText(desc.Description);
    hr = create_device(adapter_.Get(), options.feature_level, IID_PPV_ARGS(&device_));
    if (FAILED(hr)) {
        error = options.feature_level == D3D_FEATURE_LEVEL_12_0
                    ? std::format("{} has no DirectX 12 device at feature level 12_0 ({})", adapter_name_, HResult(hr))
                    : std::format("{} has no DirectX 12 device at feature level {:#x} ({})", adapter_name_,
                                  static_cast<uint32_t>(options.feature_level), HResult(hr));
        Destroy();
        return false;
    }
    luid_ = id;
    if (options.device_created) {
        uint8_t luid[8];
        std::memcpy(luid, &id, sizeof(luid));
        options.device_created(device_.Get(), luid);
    }
    if (debug) {
        device_.As(&info_queue_);
        if (info_queue_) {
            D3D12_MESSAGE_ID deny[] = {D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
                                       D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
                                       D3D12_MESSAGE_ID_CREATEGRAPHICSPIPELINESTATE_RENDERTARGETVIEW_NOT_SET};
            D3D12_INFO_QUEUE_FILTER filter{};
            filter.DenyList.NumIDs = static_cast<UINT>(std::size(deny));
            filter.DenyList.pIDList = deny;
            info_queue_->AddStorageFilterEntries(&filter);
        }
    }
    D3D12_COMMAND_QUEUE_DESC queue{};
    queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = device_->CreateCommandQueue(&queue, IID_PPV_ARGS(&queue_));
    if (SUCCEEDED(hr)) {
        hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&idle_fence_));
    }
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(hr) || !event_) {
        error = "the DirectX 12 queue or fence could not be created (" + HResult(hr) + ")";
        Destroy();
        return false;
    }
    queue_->SetName(options.queue_name);
    ComPtr<IDXGIFactory5> factory5;
    BOOL tearing = FALSE;
    if (SUCCEEDED(factory_.As(&factory5)) &&
        SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing)))) {
        tearing_ = tearing != FALSE;
    }
    LogInfo("d3d12: device on {} (LUID {:08x}:{:08x}), tearing {}{}", adapter_name_, static_cast<uint32_t>(id.HighPart), id.LowPart,
            tearing_ ? "supported" : "unsupported", !dred_ ? "" : dred_breadcrumbs_ ? ", DRED on" : ", DRED page faults on");
    return true;
}

bool Device::CreateFactory(ComPtr<IDXGIFactory4>& out) const {
    out.Reset();
    if (!create_factory_) {
        return false;
    }
    return SUCCEEDED(reinterpret_cast<CreateFactory2>(create_factory_)(0, IID_PPV_ARGS(&out)));
}

void Device::Luid(uint8_t (&out)[8]) const {
    std::memcpy(out, &luid_, sizeof(out));
}

void Device::LogDred() {
    ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
    if (!dred_ || !device_ || FAILED(device_.As(&dred))) {
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs{};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&crumbs))) {
        int lists = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE1* node = crumbs.pHeadAutoBreadcrumbNode; node && lists < 16; node = node->pNext, ++lists) {
            const uint32_t done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
            if (done >= node->BreadcrumbCount) {
                continue;
            }
            LogError("d3d12 DRED: list {} on queue {}: {} of {} operations done", node->pCommandListDebugNameA ? node->pCommandListDebugNameA : "?",
                     node->pCommandQueueDebugNameA ? node->pCommandQueueDebugNameA : "?", done, node->BreadcrumbCount);
            for (UINT i = 0; i < node->BreadcrumbContextsCount; ++i) {
                const D3D12_DRED_BREADCRUMB_CONTEXT& c = node->pBreadcrumbContexts[i];
                LogError("d3d12 DRED:   at operation {}: {}", c.BreadcrumbIndex, NarrowText(c.pContextString));
            }
        }
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT1 fault{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&fault)) && fault.PageFaultVA) {
        LogError("d3d12 DRED: page fault at {:#x}", static_cast<uint64_t>(fault.PageFaultVA));
        for (const D3D12_DRED_ALLOCATION_NODE1* n = fault.pHeadExistingAllocationNode; n; n = n->pNext) {
            LogError("d3d12 DRED:   existing allocation there: {}", n->ObjectNameA ? n->ObjectNameA : "?");
        }
        for (const D3D12_DRED_ALLOCATION_NODE1* n = fault.pHeadRecentFreedAllocationNode; n; n = n->pNext) {
            LogError("d3d12 DRED:   recently freed allocation there: {}", n->ObjectNameA ? n->ObjectNameA : "?");
        }
    }
}

void Device::Destroy() {
    if (device_ && queue_) {
        WaitIdle();
    }
    DrainDebugMessages();
    for (const auto& [id, count] : warnings_) {
        if (count > 5) {
            LogWarn("d3d12 debug: warning {} came {} times in all (the first 5 are above)", id, count);
        }
    }
    warnings_.clear();
    info_queue_.Reset();
    idle_fence_.Reset();
    queue_.Reset();
    device_.Reset();
    adapter_.Reset();
    factory_.Reset();
    if (event_) {
        CloseHandle(event_);
        event_ = nullptr;
    }
    d3d12_ = nullptr;
    dxgi_ = nullptr;
    create_factory_ = nullptr;
    idle_value_ = 0;
    tearing_ = false;
    dred_ = false;
}

bool Device::ShaderModel(D3D_SHADER_MODEL wanted) const {
    if (!device_) {
        return false;
    }
    D3D12_FEATURE_DATA_SHADER_MODEL model{wanted};
    return SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model))) && model.HighestShaderModel >= wanted;
}

void Device::LogPresentEnvironment(HWND window) {
    if (logged_environment_window_ == window) {
        return;
    }
    logged_environment_window_ = window;
    bool elevated = false;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION e{};
        DWORD size = 0;
        if (GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size)) {
            elevated = e.TokenIsElevated != 0;
        }
        CloseHandle(token);
    }
    DWORD hags = 0;
    DWORD hags_size = sizeof(hags);
    const bool hags_known = RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"HwSchMode",
                                         RRF_RT_REG_DWORD, nullptr, &hags, &hags_size) == ERROR_SUCCESS;
    const HMONITOR monitor = window ? MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) : nullptr;
    int outputs = 0;
    bool window_on_adapter = false;
    ComPtr<IDXGIOutput> output;
    for (UINT i = 0; adapter_ && SUCCEEDED(adapter_->EnumOutputs(i, &output)); ++i) {
        DXGI_OUTPUT_DESC od{};
        if (SUCCEEDED(output->GetDesc(&od)) && od.Monitor == monitor) {
            window_on_adapter = true;
        }
        ++outputs;
        output.Reset();
    }
    LogInfo("d3d12: presenting environment: process {}, session {}, hardware scheduling {}, the adapter has {} output(s) and the window's monitor is {} of them",
            elevated ? "elevated" : "not elevated", GetSystemMetrics(SM_REMOTESESSION) ? "remote" : "local",
            !hags_known ? "unknown" : hags == 2 ? "on" : "off", outputs, window_on_adapter ? "one" : "not one");
}

bool Device::Removed(std::string* reason) const {
    if (!device_) {
        return false;
    }
    const HRESULT hr = device_->GetDeviceRemovedReason();
    if (hr == S_OK) {
        return false;
    }
    if (reason) {
        const char* name = DxgiName(hr);
        *reason = "the DirectX 12 device was removed (" + HResult(hr) + (name ? std::string(", ") + name : std::string()) + ")";
    }
    return true;
}

bool Device::CreateSharedTexture(DXGI_FORMAT format, uint32_t width, uint32_t height, const wchar_t* name, ComPtr<ID3D12Resource>& out,
                                 HANDLE& handle) {
    handle = nullptr;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&out));
    if (SUCCEEDED(hr)) {
        out->SetName(name);
        hr = device_->CreateSharedHandle(out.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
    }
    if (FAILED(hr)) {
        LogError("d3d12: shared texture {}x{} format {} failed ({})", width, height, static_cast<int>(format), HResult(hr));
        out.Reset();
        handle = nullptr;
        return false;
    }
    return true;
}

bool Device::CreateSharedFence(ComPtr<ID3D12Fence>& out, HANDLE& handle) {
    handle = nullptr;
    HRESULT hr = device_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&out));
    if (SUCCEEDED(hr)) {
        hr = device_->CreateSharedHandle(out.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
    }
    if (FAILED(hr)) {
        LogError("d3d12: shared fence failed ({})", HResult(hr));
        out.Reset();
        handle = nullptr;
        return false;
    }
    return true;
}

bool Device::WaitFence(ID3D12Fence* fence, uint64_t value, uint32_t timeout_ms) {
    if (!fence || fence->GetCompletedValue() >= value) {
        return true;
    }
    const ULONGLONG started = GetTickCount64();
    for (;;) {
        const ULONGLONG spent = GetTickCount64() - started;
        if (spent >= timeout_ms) {
            return fence->GetCompletedValue() >= value;
        }
        ResetEvent(event_);
        if (FAILED(fence->SetEventOnCompletion(value, event_))) {
            return false;
        }
        const DWORD waited = WaitForSingleObject(event_, static_cast<DWORD>(timeout_ms - spent));
        if (fence->GetCompletedValue() >= value) {
            return true;
        }
        if (waited != WAIT_OBJECT_0) {
            return false;
        }
    }
}

bool Device::WaitIdle(uint32_t timeout_ms) {
    if (!queue_ || !idle_fence_) {
        return true;
    }
    ++idle_value_;
    if (FAILED(queue_->Signal(idle_fence_.Get(), idle_value_))) {
        return false;
    }
    const bool done = WaitFence(idle_fence_.Get(), idle_value_, timeout_ms);
    if (!done) {
        LogWarn("d3d12: the present queue did not go idle within {} ms", timeout_ms);
    }
    return done;
}

uint32_t Device::DrainDebugMessages() {
    if (!info_queue_) {
        return 0;
    }
    uint32_t errors = 0;
    const UINT64 count = info_queue_->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T size = 0;
        info_queue_->GetMessage(i, nullptr, &size);
        std::string storage(size, '\0');
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (FAILED(info_queue_->GetMessage(i, message, &size))) {
            continue;
        }
        const bool error = message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR;
        errors += error ? 1 : 0;
        if (error) {
            LogError("d3d12 debug: {}", message->pDescription);
        } else if (message->Severity == D3D12_MESSAGE_SEVERITY_WARNING) {
            if (++warnings_[static_cast<int>(message->ID)] <= 5) {
                LogWarn("d3d12 debug: {}", message->pDescription);
            }
        }
    }
    info_queue_->ClearStoredMessages();
    total_errors_ += errors;
    return errors;
}

}

#endif
