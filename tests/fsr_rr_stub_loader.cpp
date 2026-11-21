#include <windows.h>
#include <d3d12.h>
#include <cmath>
#include <cstdio>
#include <string>
#include <ffx_api.h>
#include <ffx_api_types.h>
#include <dx12/ffx_api_dx12.h>
#include <ffx_denoiser.h>

namespace {

HMODULE g_real = nullptr;
int g_stub_context = 0;
ffxApiMessage g_message = nullptr;
unsigned g_dispatches = 0, g_errors = 0;
uint32_t g_last_frame = 0;
FfxApiDimensions2D g_max{};
uint32_t g_signals = 0;

template <class F>
F Real(const char* name) {
    if (!g_real) {
        wchar_t path[MAX_PATH * 2];
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&g_stub_context), &self);
        GetModuleFileNameW(self, path, MAX_PATH * 2);
        std::wstring dir(path);
        dir.resize(dir.find_last_of(L"\\/") + 1);
        g_real = LoadLibraryW((dir + L"amd_fidelityfx_loader_real.dll").c_str());
    }
    return g_real ? reinterpret_cast<F>(GetProcAddress(g_real, name)) : nullptr;
}

void Report(const char* text) {
    ++g_errors;
    if (g_message) {
        wchar_t wide[512];
        std::swprintf(wide, 512, L"stub check failed: %hs", text);
        g_message(FFX_API_MESSAGE_TYPE_ERROR, wide);
    }
}

bool Finite(float v) { return std::isfinite(v) && std::fpclassify(v) != FP_SUBNORMAL; }

bool CheckResource(const FfxApiResource& r, const FfxApiDimensions2D& render, const char* name, bool output) {
    std::string what = std::string(name);
    if (!r.resource) { Report((what + " is invalid").c_str()); return false; }
    if (r.description.type != FFX_API_RESOURCE_TYPE_TEXTURE2D) Report((what + ": type must be TEXTURE_2D").c_str());
    if (r.description.depth != 1) Report((what + ": depth must be 1").c_str());
    if (r.description.width < render.width) Report((what + ": width below the render width").c_str());
    if (r.description.height < render.height) Report((what + ": height below the render height").c_str());
    if (output && !(r.description.usage & FFX_API_RESOURCE_USAGE_UAV)) Report((what + ": usage requires UAV").c_str());
    return true;
}

D3D12_RESOURCE_STATES ToD3D(uint32_t s) {
    switch (s) {
    case FFX_API_RESOURCE_STATE_UNORDERED_ACCESS: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case FFX_API_RESOURCE_STATE_COMPUTE_READ: return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_PIXEL_READ: return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ: return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case FFX_API_RESOURCE_STATE_COPY_SRC: return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case FFX_API_RESOURCE_STATE_COPY_DEST: return D3D12_RESOURCE_STATE_COPY_DEST;
    default: return D3D12_RESOURCE_STATE_COMMON;
    }
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

void TouchRead(ID3D12GraphicsCommandList* list, const FfxApiResource& r) {
    if (!r.resource) return;
    auto* res = static_cast<ID3D12Resource*>(r.resource);
    const D3D12_RESOURCE_STATES state = ToD3D(r.state);
    if (state == D3D12_RESOURCE_STATE_COPY_SOURCE) return;
    D3D12_RESOURCE_BARRIER b = Transition(res, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &b);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    list->ResourceBarrier(1, &b);
}

ffxReturnCode_t StubDispatch(const ffxDispatchDescHeader* header) {
    const auto* d = reinterpret_cast<const ffxDispatchDescDenoiser*>(header);
    if (header->type != FFX_API_DISPATCH_DESC_TYPE_DENOISER) return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    ++g_dispatches;
    if (!d->commandList) { Report("commandList cannot be nullptr"); return FFX_API_RETURN_ERROR_PARAMETER; }
    auto* list = static_cast<ID3D12GraphicsCommandList*>(d->commandList);
    const FfxApiDimensions2D render = d->renderSize;
    if (render.width > g_max.width || render.height > g_max.height) Report("renderSize above maxRenderSize");
    CheckResource(d->linearDepth, render, "linearDepth", false);
    CheckResource(d->motionVectors, render, "motionVectors", false);
    CheckResource(d->normals, render, "normals", false);
    CheckResource(d->specularAlbedo, render, "specularAlbedo", false);
    CheckResource(d->diffuseAlbedo, render, "diffuseAlbedo", false);
    if (d->linearDepth.description.format != FFX_API_SURFACE_FORMAT_R32_FLOAT) Report("linearDepth is not R32F");
    if (d->motionVectorScale.x == 0.0f || d->motionVectorScale.y == 0.0f) Report("motionVectorScale x or y is 0");
    if (std::abs(d->motionVectorScale.z) != 1.0f) Report("motionVectorScale z is not -1 nor 1");
    if (!Finite(d->jitterOffsets.x) || !Finite(d->jitterOffsets.y) || std::abs(d->jitterOffsets.x) > 1.0f || std::abs(d->jitterOffsets.y) > 1.0f) Report("jitterOffsets outside [-1, 1] or not a normal number");
    if (!Finite(d->cameraPositionDelta.x) || !Finite(d->cameraPositionDelta.y) || !Finite(d->cameraPositionDelta.z)) Report("cameraPositionDelta is invalid");
    if (!(d->linearDepthBounds.min < d->linearDepthBounds.max)) Report("linearDepthBounds min must be less than max");
    const auto* view = reinterpret_cast<const float*>(&d->view);
    const auto* projection = reinterpret_cast<const float*>(&d->projection);
    for (int i = 0; i < 16; ++i) {
        if (!Finite(view[i])) Report("view is invalid");
        if (!Finite(projection[i])) Report("projection is invalid");
    }
    if ((d->flags & ~uint32_t(FFX_DENOISER_DISPATCH_RESET | FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO)) != 0) Report("dispatch flags contain unsupported flags");
    if (g_dispatches > 1 && d->frameIndex != g_last_frame + 1 && !(d->flags & FFX_DENOISER_DISPATCH_RESET)) Report("frameIndex gap without a reset");
    g_last_frame = d->frameIndex;

    TouchRead(list, d->linearDepth);
    TouchRead(list, d->motionVectors);
    TouchRead(list, d->normals);
    TouchRead(list, d->specularAlbedo);
    TouchRead(list, d->diffuseAlbedo);

    uint32_t seen = 0;
    for (auto* next = header->pNext; next; next = next->pNext) {
        const FfxApiDenoiserSignal* signal = nullptr;
        switch (next->type) {
        case FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_DIFFUSE: signal = &reinterpret_cast<ffxDispatchDescDenoiserDirectDiffuse*>(next)->signal; seen |= FFX_DENOISER_SIGNAL_DIRECT_DIFFUSE; break;
        case FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_SPECULAR: signal = &reinterpret_cast<ffxDispatchDescDenoiserDirectSpecular*>(next)->signal; seen |= FFX_DENOISER_SIGNAL_DIRECT_SPECULAR; break;
        case FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_DIFFUSE: signal = &reinterpret_cast<ffxDispatchDescDenoiserIndirectDiffuse*>(next)->signal; seen |= FFX_DENOISER_SIGNAL_INDIRECT_DIFFUSE; break;
        case FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR: signal = &reinterpret_cast<ffxDispatchDescDenoiserIndirectSpecular*>(next)->signal; seen |= FFX_DENOISER_SIGNAL_INDIRECT_SPECULAR; break;
        default: Report("unknown description chained to the dispatch"); break;
        }
        if (!signal) continue;
        CheckResource(signal->input, render, "signal input", false);
        CheckResource(signal->output, render, "signal output", true);
        if (signal->checkerboardOrigin) Report("checkerboardOrigin is not zero");
        if (!signal->input.resource || !signal->output.resource) continue;
        if (signal->input.resource == signal->output.resource) Report("a signal's input is its output");
        auto* in = static_cast<ID3D12Resource*>(signal->input.resource);
        auto* out = static_cast<ID3D12Resource*>(signal->output.resource);
        const D3D12_RESOURCE_STATES in_state = ToD3D(signal->input.state), out_state = ToD3D(signal->output.state);
        D3D12_RESOURCE_BARRIER to[2] = {Transition(in, in_state, D3D12_RESOURCE_STATE_COPY_SOURCE), Transition(out, out_state, D3D12_RESOURCE_STATE_COPY_DEST)};
        list->ResourceBarrier(2, to);
        list->CopyResource(out, in);
        D3D12_RESOURCE_BARRIER back[2] = {Transition(in, D3D12_RESOURCE_STATE_COPY_SOURCE, in_state), Transition(out, D3D12_RESOURCE_STATE_COPY_DEST, out_state)};
        list->ResourceBarrier(2, back);
    }
    if (seen != g_signals) Report("the dispatch's signals are not the context's");
    if (g_message && g_dispatches == 1) g_message(FFX_API_MESSAGE_TYPE_WARNING, L"stub loader: messages reach the application");
    return FFX_API_RETURN_OK;
}

}

extern "C" {

__declspec(dllexport) ffxReturnCode_t ffxCreateContext(ffxContext* context, ffxCreateContextDescHeader* desc, const ffxAllocationCallbacks* alloc) {
    if (desc->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER) {
        const auto* c = reinterpret_cast<const ffxCreateContextDescDenoiser*>(desc);
        if (c->version != FFX_DENOISER_VERSION) Report("ffxCreateContextDescDenoiser::version is invalid");
        if (!c->signalFlags) Report("signalFlags is invalid");
        if (c->checkerboardSignalFlags & ~c->signalFlags) Report("checkerboardSignalFlags must be a subset of signalFlags");
        g_max = c->maxRenderSize;
        g_signals = c->signalFlags;
        g_dispatches = 0;
        *context = &g_stub_context;
        return FFX_API_RETURN_OK;
    }
    auto f = Real<PfnFfxCreateContext>("ffxCreateContext");
    return f ? f(context, desc, alloc) : FFX_API_RETURN_NO_PROVIDER;
}

__declspec(dllexport) ffxReturnCode_t ffxDestroyContext(ffxContext* context, const ffxAllocationCallbacks* alloc) {
    if (context && *context == &g_stub_context) {
        *context = nullptr;
        return FFX_API_RETURN_OK;
    }
    auto f = Real<PfnFfxDestroyContext>("ffxDestroyContext");
    return f ? f(context, alloc) : FFX_API_RETURN_NO_PROVIDER;
}

__declspec(dllexport) ffxReturnCode_t ffxConfigure(ffxContext* context, const ffxConfigureDescHeader* desc) {
    if (desc->type == FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG && reinterpret_cast<const ffxConfigureDescGlobalDebug*>(desc)->effectId == FFX_API_EFFECT_ID_DENOISER) {
        g_message = reinterpret_cast<const ffxConfigureDescGlobalDebug*>(desc)->fpMessage;
        return FFX_API_RETURN_OK;
    }
    auto f = Real<PfnFfxConfigure>("ffxConfigure");
    return f ? f(context, desc) : FFX_API_RETURN_NO_PROVIDER;
}

__declspec(dllexport) ffxReturnCode_t ffxQuery(ffxContext* context, ffxQueryDescHeader* desc) {
    if (desc->type == FFX_API_QUERY_DESC_TYPE_GET_VERSIONS && reinterpret_cast<ffxQueryDescGetVersions*>(desc)->createDescType == FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER) {
        auto* q = reinterpret_cast<ffxQueryDescGetVersions*>(desc);
        *q->outputCount = 1;
        if (q->versionIds) q->versionIds[0] = 0x1234;
        if (q->versionNames) q->versionNames[0] = "stub 1.2.0";
        return FFX_API_RETURN_OK;
    }
    if (desc->type == FFX_API_QUERY_DESC_TYPE_DENOISER_GET_DEFAULT_KEYVALUE) {
        *static_cast<float*>(reinterpret_cast<ffxQueryDescDenoiserGetDefaultKeyValue*>(desc)->data) = 1.5f;
        return FFX_API_RETURN_OK;
    }
    if (desc->type == FFX_API_QUERY_DESC_TYPE_DENOISER_GPU_MEMORY_USAGE) {
        *reinterpret_cast<ffxQueryDescDenoiserGetGPUMemoryUsage*>(desc)->gpuMemoryUsage = {300ull << 20, 100ull << 20};
        return FFX_API_RETURN_OK;
    }
    auto f = Real<PfnFfxQuery>("ffxQuery");
    return f ? f(context, desc) : FFX_API_RETURN_NO_PROVIDER;
}

__declspec(dllexport) ffxReturnCode_t ffxDispatch(ffxContext* context, const ffxDispatchDescHeader* desc) {
    if (context && *context == &g_stub_context) {
        return StubDispatch(desc);
    }
    auto f = Real<PfnFfxDispatch>("ffxDispatch");
    return f ? f(context, desc) : FFX_API_RETURN_NO_PROVIDER;
}

}
