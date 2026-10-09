#include "engine/render/upscale/upscale_platform.h"

// only the Windows upscaler SDKs (FSR, XeSS, DLSS) report wide text; they are not built elsewhere (cmake/Upscalers.cmake)
#ifdef _WIN32

#include <windows.h>

#include <cstring>

namespace pt {

std::string NarrowText(const wchar_t* text) {
    if (!text) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

namespace {

// the d3dkmthk.h structures used here (shared/d3dkmthk.h, d3dkmdt.h of the Windows SDK), loaded from gdi32.dll at run time
struct KmtOpenAdapterFromLuid {
    LUID luid;
    UINT adapter;
};
struct KmtQueryAdapterInfo {
    UINT adapter;
    int type;
    void* data;
    UINT size;
};
struct KmtCloseAdapter {
    UINT adapter;
};
constexpr int kKmtWddm27Caps = 70;  // KMTQAITYPE_WDDM_2_7_CAPS: HwSchSupported bit 0, HwSchEnabled bit 1
using KmtOpen = LONG(APIENTRY*)(KmtOpenAdapterFromLuid*);
using KmtQuery = LONG(APIENTRY*)(const KmtQueryAdapterInfo*);
using KmtClose = LONG(APIENTRY*)(const KmtCloseAdapter*);

}

int HardwareGpuScheduling(const uint8_t (&luid)[8]) {
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi) {
        gdi = LoadLibraryW(L"gdi32.dll");
    }
    const auto open = gdi ? reinterpret_cast<KmtOpen>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid")) : nullptr;
    const auto query = gdi ? reinterpret_cast<KmtQuery>(GetProcAddress(gdi, "D3DKMTQueryAdapterInfo")) : nullptr;
    const auto close = gdi ? reinterpret_cast<KmtClose>(GetProcAddress(gdi, "D3DKMTCloseAdapter")) : nullptr;
    if (!open || !query || !close) {
        return -1;
    }
    KmtOpenAdapterFromLuid adapter{};
    std::memcpy(&adapter.luid, luid, sizeof(adapter.luid));
    if (open(&adapter) != 0) {
        return -1;
    }
    UINT caps = 0;
    const KmtQueryAdapterInfo info{adapter.adapter, kKmtWddm27Caps, &caps, sizeof(caps)};
    const LONG status = query(&info);
    const KmtCloseAdapter done{adapter.adapter};
    close(&done);
    if (status != 0) {
        return -1;
    }
    return (caps & 2u) ? 1 : 0;
}

}

#endif
