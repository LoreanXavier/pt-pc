#pragma once

#ifdef _WIN32

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <map>
#include <string>

namespace pt::d3d12 {

using Microsoft::WRL::ComPtr;

uint32_t WindowsBuild();

struct DeviceOptions {
    const uint8_t* luid = nullptr;
    bool debug = false;
    bool dred = false;
    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_12_0;
    const wchar_t* queue_name = L"pt present queue";
    void (*device_created)(ID3D12Device* device, const uint8_t (&luid)[8]) = nullptr;
};

class Device {
public:
    Device() = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    ~Device() { Destroy(); }

    bool Create(const uint8_t (&luid)[8], std::string& error);
    bool Create(const DeviceOptions& options, std::string& error);
    void Destroy();
    bool Valid() const { return device_ != nullptr; }

    bool ShaderModel(D3D_SHADER_MODEL wanted) const;
    bool TearingSupported() const { return tearing_; }
    bool Removed(std::string* reason = nullptr) const;

    bool CreateSharedTexture(DXGI_FORMAT format, uint32_t width, uint32_t height, const wchar_t* name, ComPtr<ID3D12Resource>& out,
                             HANDLE& handle);
    bool CreateSharedFence(ComPtr<ID3D12Fence>& out, HANDLE& handle);
    bool WaitFence(ID3D12Fence* fence, uint64_t value, uint32_t timeout_ms);
    bool WaitIdle(uint32_t timeout_ms = 5000);

    ID3D12Device* Get() const { return device_.Get(); }
    ID3D12CommandQueue* Queue() const { return queue_.Get(); }
    IDXGIFactory4* Factory() const { return factory_.Get(); }
    bool CreateFactory(ComPtr<IDXGIFactory4>& out) const;
    void Luid(uint8_t (&out)[8]) const;
    IDXGIAdapter1* Adapter() const { return adapter_.Get(); }
    bool DredEnabled() const { return dred_; }
    void LogDred();
    void LogPresentEnvironment(HWND window);
    const std::string& AdapterName() const { return adapter_name_; }
    uint32_t DrainDebugMessages();
    uint32_t TotalDebugErrors() const { return total_errors_; }

private:

    HMODULE d3d12_ = nullptr;
    HMODULE dxgi_ = nullptr;
    void* create_factory_ = nullptr;
    LUID luid_{};
    ComPtr<IDXGIFactory4> factory_;
    ComPtr<IDXGIAdapter1> adapter_;
    HWND logged_environment_window_ = nullptr;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12Fence> idle_fence_;
    ComPtr<ID3D12InfoQueue> info_queue_;
    uint64_t idle_value_ = 0;
    uint32_t total_errors_ = 0;
    std::map<int, uint32_t> warnings_;
    HANDLE event_ = nullptr;
    bool tearing_ = false;
    bool dred_ = false;
    bool dred_breadcrumbs_ = false;
    std::string adapter_name_;
};

}

#endif
