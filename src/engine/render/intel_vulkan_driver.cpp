#include "engine/render/intel_vulkan_driver.h"

#include <cstring>
#include <format>

#ifdef _WIN32
#include <windows.h>
#include <dxgi.h>
#endif

namespace pt::vk {

std::string IntelWindowsDriverName(uint32_t driver_version) {
    return std::format("31.0.{}.{}", driver_version >> 14, driver_version & 0x3fffu);
}

std::string WindowsDriverName(const WindowsDriverVersion& version) {
    return std::format("{}.{}.{}.{}", version.field[0], version.field[1], version.field[2], version.field[3]);
}

#ifdef _WIN32
WindowsDriverVersion DxgiDriverVersion(const uint8_t (&luid)[8]) {
    WindowsDriverVersion out;
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    using CreateFactory1 = HRESULT(WINAPI*)(REFIID, void**);
    auto create = dxgi ? reinterpret_cast<CreateFactory1>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1"))) : nullptr;
    IDXGIFactory1* factory = nullptr;
    if (!create || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        return out;
    }
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; !out.valid && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        LARGE_INTEGER umd{};
        if (SUCCEEDED(adapter->GetDesc1(&d)) && std::memcmp(&d.AdapterLuid, luid, sizeof(d.AdapterLuid)) == 0 &&
            SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
            out.field[0] = HIWORD(umd.HighPart);
            out.field[1] = LOWORD(umd.HighPart);
            out.field[2] = HIWORD(umd.LowPart);
            out.field[3] = LOWORD(umd.LowPart);
            out.valid = true;
        }
        adapter->Release();
    }
    factory->Release();
    return out;
}

std::vector<DxgiAdapter> DxgiHardwareAdapters() {
    std::vector<DxgiAdapter> out;
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    using CreateFactory1 = HRESULT(WINAPI*)(REFIID, void**);
    auto create = dxgi ? reinterpret_cast<CreateFactory1>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1"))) : nullptr;
    IDXGIFactory1* factory = nullptr;
    if (!create || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        return out;
    }
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(adapter->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.VendorId != 0x1414) {
            DxgiAdapter a;
            std::memcpy(a.luid, &d.AdapterLuid, sizeof(a.luid));
            a.vendor_id = d.VendorId;
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, nullptr, 0, nullptr, nullptr);
            if (bytes > 1) {
                a.name.resize(static_cast<size_t>(bytes - 1));
                WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, a.name.data(), bytes, nullptr, nullptr);
            }
            out.push_back(std::move(a));
        }
        adapter->Release();
    }
    factory->Release();
    return out;
}
#endif

bool IntelDriverCrashesFirstFrame(uint32_t vendor_id, uint32_t driver_version, bool windows, const WindowsDriverVersion& dxgi) {
    if (!windows || vendor_id != kIntelVendorId) {
        return false;
    }
    if (dxgi.valid) {
        if (dxgi.field[2] != 100 && dxgi.field[2] != 101) {
            return false;
        }
        const uint16_t good[4] = {31, 0, 101, 2145};
        for (int i = 0; i < 4; ++i) {
            if (dxgi.field[i] != good[i]) {
                return dxgi.field[i] < good[i];
            }
        }
        return false;
    }
    const uint32_t branch = driver_version >> 14;
    return (branch == 100 || branch == 101) && driver_version < kIntelFirstGoodDriver;
}

}
