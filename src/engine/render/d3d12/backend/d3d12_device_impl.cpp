#include "engine/platform/os.h"
#include "engine/render/d3d12/backend/d3d12_internal.h"

#include <D3D12MemAlloc.h>
#include <d3dx12/d3dx12.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>

#include "engine/core/crash_report.h"
#include "engine/core/json.h"
#include "engine/core/log.h"
#include "engine/core/resource_path.h"
#include "engine/render/d3d12/backend/d3d12_native.h"
#include "engine/render/d3d12/backend/reshade_native.h"
#include "engine/render/mesh.h"
#include "engine/render/upscale/upscale_platform.h"
#include "engine/render/upscale/streamline.h"

namespace pt::gpu::d3d12 {
namespace {

std::string Hr(HRESULT hr) {
    return std::format("{:#010x}", static_cast<uint32_t>(hr));
}

std::wstring Wide(const char* s) {
    std::wstring out;
    for (; s && *s; ++s) {
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*s)));
    }
    return out;
}

D3D12_FILTER ToFilter(const SamplerDesc& d) {
    if (d.max_anisotropy > 1.0f) {
        return d.compare ? D3D12_FILTER_COMPARISON_ANISOTROPIC : D3D12_FILTER_ANISOTROPIC;
    }
    const D3D12_FILTER_TYPE min = d.min == Filter::Linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    const D3D12_FILTER_TYPE mag = d.mag == Filter::Linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    const D3D12_FILTER_TYPE mip = d.mipmap == MipmapMode::Linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    return D3D12_ENCODE_BASIC_FILTER(min, mag, mip, d.compare ? D3D12_FILTER_REDUCTION_TYPE_COMPARISON : D3D12_FILTER_REDUCTION_TYPE_STANDARD);
}

D3D12_TEXTURE_ADDRESS_MODE ToAddress(AddressMode m) {
    switch (m) {
    case AddressMode::Repeat: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case AddressMode::MirroredRepeat: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case AddressMode::ClampToEdge: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case AddressMode::ClampToBorder: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    }
    return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
}

D3D12_SAMPLER_DESC ToSampler(const SamplerDesc& d) {
    D3D12_SAMPLER_DESC s{};
    s.Filter = ToFilter(d);
    s.AddressU = ToAddress(d.address_u);
    s.AddressV = ToAddress(d.address_v);
    s.AddressW = ToAddress(d.address_w);
    s.MaxAnisotropy = d.max_anisotropy > 1.0f ? static_cast<UINT>(d.max_anisotropy) : 1;
    s.ComparisonFunc = d.compare ? ToD3D(d.compare_op) : D3D12_COMPARISON_FUNC_NEVER;
    const float border = d.border == BorderColor::FloatOpaqueWhite ? 1.0f : 0.0f;
    s.BorderColor[0] = s.BorderColor[1] = s.BorderColor[2] = border;
    s.BorderColor[3] = d.border == BorderColor::FloatTransparentBlack ? 0.0f : 1.0f;
    s.MinLOD = d.min_lod;
    s.MaxLOD = d.max_lod;
    return s;
}

D3D12_SAMPLER_DESC DefaultSampler() {
    return ToSampler(SamplerDesc{});
}

void Blend(BlendMode mode, ColorMask mask, D3D12_RENDER_TARGET_BLEND_DESC& b) {
    b = {};
    b.RenderTargetWriteMask = static_cast<UINT8>(mask.bits);
    b.BlendOp = D3D12_BLEND_OP_ADD;
    b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    b.SrcBlend = D3D12_BLEND_ONE;
    b.DestBlend = D3D12_BLEND_ZERO;
    b.SrcBlendAlpha = D3D12_BLEND_ONE;
    b.DestBlendAlpha = D3D12_BLEND_ZERO;
    b.LogicOp = D3D12_LOGIC_OP_NOOP;
    switch (mode) {
    case BlendMode::None: return;
    case BlendMode::Alpha:
        b.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        b.SrcBlendAlpha = D3D12_BLEND_ZERO;
        b.DestBlendAlpha = D3D12_BLEND_ONE;
        break;
    case BlendMode::PremultipliedFadeAlpha:
        b.SrcBlend = D3D12_BLEND_ONE;
        b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        b.SrcBlendAlpha = D3D12_BLEND_ZERO;
        b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        break;
    case BlendMode::Additive:
        b.SrcBlend = D3D12_BLEND_ONE;
        b.DestBlend = D3D12_BLEND_ONE;
        b.SrcBlendAlpha = D3D12_BLEND_ZERO;
        b.DestBlendAlpha = D3D12_BLEND_ONE;
        break;
    case BlendMode::ProbeLerp:
        b.SrcBlend = D3D12_BLEND_ONE;
        b.DestBlend = D3D12_BLEND_SRC_ALPHA;
        b.SrcBlendAlpha = D3D12_BLEND_ZERO;
        b.DestBlendAlpha = D3D12_BLEND_ONE;
        break;
    case BlendMode::ProbeAccumulate:
        b.SrcBlend = D3D12_BLEND_ONE;
        b.DestBlend = D3D12_BLEND_SRC_ALPHA;
        b.SrcBlendAlpha = D3D12_BLEND_ZERO;
        b.DestBlendAlpha = D3D12_BLEND_SRC_ALPHA;
        break;
    case BlendMode::Multiply:
        b.SrcBlend = D3D12_BLEND_ZERO;
        b.DestBlend = D3D12_BLEND_SRC_COLOR;
        b.SrcBlendAlpha = D3D12_BLEND_ZERO;
        b.DestBlendAlpha = D3D12_BLEND_ONE;
        break;
    case BlendMode::AdditiveAlpha:
        b.SrcBlend = D3D12_BLEND_ONE;
        b.DestBlend = D3D12_BLEND_ONE;
        b.SrcBlendAlpha = D3D12_BLEND_ONE;
        b.DestBlendAlpha = D3D12_BLEND_ONE;
        break;
    }
    b.BlendEnable = TRUE;
}

D3D12_BLEND ToD3D(BlendFactor f, bool alpha) {
    switch (f) {
    case BlendFactor::Zero: return D3D12_BLEND_ZERO;
    case BlendFactor::One: return D3D12_BLEND_ONE;
    case BlendFactor::SrcColor: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
    case BlendFactor::OneMinusSrcColor: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
    case BlendFactor::DstColor: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
    case BlendFactor::OneMinusDstColor: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
    case BlendFactor::SrcAlpha: return D3D12_BLEND_SRC_ALPHA;
    case BlendFactor::OneMinusSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
    case BlendFactor::DstAlpha: return D3D12_BLEND_DEST_ALPHA;
    case BlendFactor::OneMinusDstAlpha: return D3D12_BLEND_INV_DEST_ALPHA;
    }
    return D3D12_BLEND_ONE;
}

D3D12_BLEND_OP ToD3D(BlendOp op) {
    switch (op) {
    case BlendOp::Add: return D3D12_BLEND_OP_ADD;
    case BlendOp::Subtract: return D3D12_BLEND_OP_SUBTRACT;
    case BlendOp::ReverseSubtract: return D3D12_BLEND_OP_REV_SUBTRACT;
    case BlendOp::Min: return D3D12_BLEND_OP_MIN;
    case BlendOp::Max: return D3D12_BLEND_OP_MAX;
    }
    return D3D12_BLEND_OP_ADD;
}

void ExplicitBlend(const BlendState& s, D3D12_RENDER_TARGET_BLEND_DESC& b) {
    b = {};
    b.BlendEnable = s.enable ? TRUE : FALSE;
    b.SrcBlend = ToD3D(s.src_color, false);
    b.DestBlend = ToD3D(s.dst_color, false);
    b.BlendOp = ToD3D(s.color_op);
    b.SrcBlendAlpha = ToD3D(s.src_alpha, true);
    b.DestBlendAlpha = ToD3D(s.dst_alpha, true);
    b.BlendOpAlpha = ToD3D(s.alpha_op);
    b.LogicOp = D3D12_LOGIC_OP_NOOP;
    b.RenderTargetWriteMask = static_cast<UINT8>(s.write_mask.bits);
}

uint64_t VariantKey(CullMode cull, FrontFace front, bool dynamic_bias, bool bias, const float v[3]) {
    uint64_t key = static_cast<uint64_t>(cull) | (static_cast<uint64_t>(front) << 2);
    if (bias && !dynamic_bias) {
        uint32_t bits[3];
        std::memcpy(bits, v, sizeof(bits));
        uint64_t h = 1469598103934665603ull;
        for (uint32_t b : bits) {
            h = (h ^ b) * 1099511628211ull;
        }
        key |= h << 3;
    }
    return key;
}

std::filesystem::path ShaderDir() {
    static const std::filesystem::path dir = ResourceDir("shaders", PT_SHADER_DIR);
    return dir;
}

}

std::unique_ptr<D3D12Device> Create(const CreateOptions& options, std::string& error) {
    auto device = std::make_unique<DeviceImpl>(options);
    if (!device->Init(error)) {
        return nullptr;
    }
    return device;
}

DeviceImpl::~DeviceImpl() {
    if (bridge_.Valid()) {
        WaitIdle();
    }
    presenter_.Destroy();
    if (xefg_) {
        xefg_->Destroy();
    }
    if (fsr_fg_) {
        fsr_fg_->Destroy();
    }
    if (dlss_fg_) {
        dlss_fg_->Destroy();
    }
    if (streamline::ActiveD3D12()) {
        streamline::Shutdown();
    }
    deferred_.clear();
    frames_.clear();
    spare_.clear();
    markers_.clear();
    marker_buffer_.Reset();
    marker_heap_.Reset();
    if (marker_memory_) {
        VirtualFree(marker_memory_, 0, MEM_RELEASE);
        marker_memory_ = nullptr;
    }
    Collect();
    for (Garbage& g : garbage_) {
        if (g.allocation) {
            g.allocation->Release();
        }
    }
    garbage_.clear();
    if (allocator_) {
        allocator_->Release();
        allocator_ = nullptr;
    }
    if (event_) {
        CloseHandle(event_);
    }
}

bool DeviceImpl::Init(std::string& error) {
    pt::d3d12::DeviceOptions o;
    o.debug = options_.debug;
    o.dred = options_.dred;
    o.feature_level = D3D_FEATURE_LEVEL_12_0;
    o.queue_name = L"pt direct queue";
    if (options_.use_adapter_luid) {
        o.luid = options_.adapter_luid;
    }
    if (streamline::ActiveD3D12()) {
        o.device_created = [](ID3D12Device* device, const uint8_t (&luid)[8]) {
            const auto native = ReShadeNative(device);
            streamline::DeviceCreatedD3D12(native.Get(), luid);
        };
    }
    if (!bridge_.Create(o, error)) {
        return false;
    }
    ID3D12Device* device = bridge_.Get();
    const std::string lack = os::GetEnv("PT_D3D12_TEST_LACK");
    auto lacks = [&lack](std::string_view what) {
        return (',' + lack + ',').find(',' + std::string(what) + ',') != std::string::npos;
    };
    std::vector<std::string> missing;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device2_))) || lacks("device2")) {
        device2_.Reset();
        missing.push_back("ID3D12Device2 (pipeline state streams)");
    }
    if (!bridge_.ShaderModel(D3D_SHADER_MODEL_6_6) || lacks("sm66")) {
        missing.push_back("shader model 6.6 (the backend's DXIL)");
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    const bool has_options = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
    if (!has_options || options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_2 || lacks("binding")) {
        missing.push_back("resource binding tier 2 (unbounded descriptor tables)");
    }
    if (!has_options || !options.TypedUAVLoadAdditionalFormats || lacks("typeduav")) {
        missing.push_back("typed UAV loads of the additional formats");
    }
    D3D12_FEATURE_DATA_ROOT_SIGNATURE root_version{D3D_ROOT_SIGNATURE_VERSION_1_1};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &root_version, sizeof(root_version))) ||
        root_version.HighestVersion < D3D_ROOT_SIGNATURE_VERSION_1_1 || lacks("rootsig")) {
        missing.push_back("root signature version 1.1");
    }
    if (!missing.empty()) {
        error = "the GPU or its driver lacks ";
        for (size_t i = 0; i < missing.size(); ++i) {
            error += (i ? (i + 1 == missing.size() ? " and " : ", ") : "") + missing[i];
        }
        return false;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS2 options2{};
    device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &options2, sizeof(options2));
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
    device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));
    D3D12_FEATURE_DATA_D3D12_OPTIONS16 options16{};
    const bool has16 = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &options16, sizeof(options16)));

    DXGI_ADAPTER_DESC1 adapter{};
    bridge_.Adapter()->GetDesc1(&adapter);
    info_.backend = gpu::Backend::D3D12;
    info_.adapter = bridge_.AdapterName();
    info_.vendor_id = adapter.VendorId;
    info_.device_id = adapter.DeviceId;
    std::memcpy(info_.luid, &adapter.AdapterLuid, sizeof(info_.luid));
    info_.luid_valid = true;
    info_.dedicated_video_memory = adapter.DedicatedVideoMemory;
    info_.ray_query = options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1;
    info_.depth_bounds = options2.DepthBoundsTestSupported != FALSE;
    info_.dynamic_depth_bias = has16 && options16.DynamicDepthBiasSupported;
    {
        const char* env = std::getenv("PT_D3D12_GPU_UPLOAD");
        gpu_upload_ = has16 && options16.GPUUploadHeapSupported && (!env || env[0] != '0');
    }
    UINT64 frequency = 0;
    if (SUCCEEDED(bridge_.Queue()->GetTimestampFrequency(&frequency)) && frequency) {
        info_.timestamp_period_ns = 1.0e9 / static_cast<double>(frequency);
    }
    info_.max_bindless_textures = 8192;

    D3D12MA::ALLOCATOR_DESC alloc{};
    alloc.pDevice = device;
    alloc.pAdapter = bridge_.Adapter();
    alloc.Flags = static_cast<D3D12MA::ALLOCATOR_FLAGS>(D3D12MA::ALLOCATOR_FLAG_MSAA_TEXTURES_ALWAYS_COMMITTED |
                                                        D3D12MA::ALLOCATOR_FLAG_DEFAULT_POOLS_NOT_ZEROED);
    if (FAILED(D3D12MA::CreateAllocator(&alloc, &allocator_))) {
        error = "the D3D12 memory allocator could not be created";
        return false;
    }
    if (const char* budget = std::getenv("PT_VRAM_BUDGET_MB"); budget && std::atoi(budget) > 0) {
        test_budget_bytes_ = static_cast<uint64_t>(std::atoi(budget)) * 1024 * 1024;
        LogWarn("d3d12: test video memory budget {} MB (PT_VRAM_BUDGET_MB): default heap allocations limited", std::atoi(budget));
    }
    if (const char* fail = std::getenv("PT_D3D12_TEST_UPLOAD_FAIL"); fail && *fail) {
        test_upload_fail_ = std::string_view(fail) == "always" ? 2 : 1;
        LogWarn("d3d12: test staging upload failures (PT_D3D12_TEST_UPLOAD_FAIL={}): {}", fail,
                test_upload_fail_ == 2 ? "every staging buffer fails" : "every other staging buffer fails");
    }
    if (const char* remove = std::getenv("PT_D3D12_TEST_REMOVE"); remove && std::atoi(remove) > 0) {
        test_remove_frame_ = static_cast<uint32_t>(std::atoi(remove));
    }
    if (const char* late = std::getenv("PT_D3D12_TEST_LATE_FENCE"); late && std::atoi(late) > 0) {
        test_late_fence_ = static_cast<uint32_t>(std::atoi(late));
    }
    if (!views_.Create(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, options_.view_descriptors, true, L"pt views") ||
        !samplers_.Create(device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, options_.sampler_descriptors, true, L"pt samplers") ||
        !rtvs_.Create(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4096, false, L"pt rtv") ||
        !dsvs_.Create(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1024, false, L"pt dsv") ||
        !cpu_views_.Create(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096, false, L"pt cpu views")) {
        error = "the descriptor heaps could not be created";
        return false;
    }
    if (!CreateRootSignature(error)) {
        return false;
    }
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
        error = "the frame fence could not be created";
        return false;
    }
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) {
        error = "the frame fence's event could not be created";
        return false;
    }
    frames_.resize(options_.frames_in_flight);
    for (Frame& f : frames_) {
        if (!NewFrame(f, L"pt frame")) {
            error = "the frame command lists could not be created";
            return false;
        }
    }
    CreatePassMarkers();
    LogInfo("d3d12 backend: {} ({}), shader model 6.6, binding tier {}, ray tracing tier {:#x}, depth bounds {}, dynamic depth bias {}, GPU upload heap {}, {} MB",
            info_.adapter, RuntimeDescription(), static_cast<int>(options.ResourceBindingTier), static_cast<int>(options5.RaytracingTier),
            info_.depth_bounds ? "yes" : "no", info_.dynamic_depth_bias ? "yes" : "no", gpu_upload_ ? "yes" : "no", info_.dedicated_video_memory >> 20);
    {
        LARGE_INTEGER umd{};
        if (SUCCEEDED(bridge_.Adapter()->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
            LogInfo("d3d12 backend: driver {}.{}.{}.{}, vendor {:04x} device {:04x}", HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart), LOWORD(umd.LowPart), info_.vendor_id,
                    info_.device_id);
        }
    }
    LogInfo("d3d12 backend: CPU-written shader inputs use {}; GPU-written CPU readbacks stay in system memory",
            gpu_upload_ ? "GPU upload heaps" : "staged video-memory buffers");
    return true;
}

std::string DeviceImpl::RuntimeDescription() const {
    HMODULE core = GetModuleHandleW(L"D3D12Core.dll");
    if (!core) {
        return "D3D12Core.dll not loaded";
    }
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(core, path, MAX_PATH);
    const std::filesystem::path p(path);
    std::string where = NarrowText(path);
    const bool agility = p.parent_path().filename() == L"D3D12" && p.parent_path().parent_path() != std::filesystem::path(L"C:\\Windows\\System32");
    return std::format("{} runtime, {}", agility ? "Agility SDK" : "system", where);
}

bool DeviceImpl::CreateRootSignature(std::string& error) {
    constexpr D3D12_DESCRIPTOR_RANGE_FLAGS kVolatile =
        D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    std::vector<D3D12_ROOT_PARAMETER1> params;
    std::vector<std::vector<D3D12_DESCRIPTOR_RANGE1>> ranges;
    ranges.reserve(32);
    D3D12_ROOT_PARAMETER1 push{};
    push.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    push.Constants = {0, 64, 32};
    push.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params.push_back(push);
    auto table = [&](std::vector<D3D12_DESCRIPTOR_RANGE1> r) {
        ranges.push_back(std::move(r));
        D3D12_ROOT_PARAMETER1 p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p.DescriptorTable.NumDescriptorRanges = static_cast<UINT>(ranges.back().size());
        p.DescriptorTable.pDescriptorRanges = ranges.back().data();
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(p);
    };
    for (UINT set = 0; set < kMaxSets; ++set) {
        table({{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kSlotsPerClass, 0, set, kVolatile, 0},
               {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kSlotsPerClass, 0, set, kVolatile, kSlotsPerClass},
               {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, kSlotsPerClass, 0, set, kVolatile, 2 * kSlotsPerClass}});
    }
    for (UINT set = 0; set < kMaxSets; ++set) {
        table({{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, kSlotsPerClass, 0, set, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0}});
    }
    for (const RootLayout::ArraySpace& a : RootLayout::kArraySpaces) {
        const D3D12_DESCRIPTOR_RANGE_FLAGS flags =
            a.type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER ? D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE : kVolatile;
        table({{a.type, a.count, 0, a.space, flags, 0}});
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = static_cast<UINT>(params.size());
    desc.Desc_1_1.pParameters = params.data();
    desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    using Serialize = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
    auto serialize = reinterpret_cast<Serialize>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeVersionedRootSignature")));
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> message;
    const HRESULT hr = serialize ? serialize(&desc, &blob, &message) : E_NOINTERFACE;
    if (FAILED(hr)) {
        error = "the root signature could not be serialized (" + Hr(hr) + ")";
        if (message) {
            error += ": " + std::string(static_cast<const char*>(message->GetBufferPointer()), message->GetBufferSize());
        }
        return false;
    }
    if (FAILED(bridge_.Get()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_)))) {
        error = "the root signature could not be created";
        return false;
    }
    root_->SetName(L"pt root signature");
    return true;
}

bool DeviceImpl::CheckRootSignature(const std::string& shader_dir, std::string& report) const {
    namespace fs = std::filesystem;
    int shaders = 0;
    int resources = 0;
    int misses = 0;
    for (const fs::directory_entry& e : fs::directory_iterator(pt::os::PathFromUtf8(shader_dir))) {
        const std::string name = pt::os::PathToUtf8(e.path().filename());
        if (!name.ends_with(".dxil.json")) {
            continue;
        }
        std::ifstream in(e.path(), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        json::Value doc;
        std::string parse_error;
        if (!json::Parse(text, doc, &parse_error)) {
            report += std::format("{}: unreadable ({})\n", name, parse_error);
            ++misses;
            continue;
        }
        ++shaders;
        if (const json::Value* push = doc.Find("push_constants"); push && push->IsObject()) {
            const double bytes = push->Number("bytes", 0.0);
            const double space = push->Number("space", -1.0);
            if (bytes > 128 || space != 64) {
                report += std::format("{}: push constants {} bytes in space {}\n", name, bytes, space);
                ++misses;
            }
        }
        const json::Value* list = doc.Find("resources");
        if (!list || !list->IsArray()) {
            continue;
        }
        for (const json::Value& r : list->array) {
            ++resources;
            const uint32_t space = static_cast<uint32_t>(r.Number("space", -1.0));
            const uint32_t reg = static_cast<uint32_t>(r.Number("register", -1.0));
            const uint32_t count = static_cast<uint32_t>(r.Number("count", -1.0));
            const std::string cls = r.String("class");
            bool covered = false;
            if (space < kMaxSets) {
                covered = reg < kSlotsPerClass && count == 1 && (cls == "t" || cls == "u" || cls == "b" || cls == "s");
            } else {
                for (const RootLayout::ArraySpace& a : RootLayout::kArraySpaces) {
                    if (a.space != space || reg != 0) {
                        continue;
                    }
                    const char* want = a.type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV ? "t" : a.type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV ? "u" : "s";
                    covered = cls == want && (a.count == UINT_MAX ? count == 0 : count <= a.count);
                }
            }
            if (!covered) {
                ++misses;
                report += std::format("{}: {}{} space{} count {} has no root parameter\n", name, cls, reg, space, count);
            }
        }
    }
    report += std::format("{} shaders, {} resources, {} not covered by the root signature\n", shaders, resources, misses);
    return shaders > 0 && misses == 0;
}

void DeviceImpl::ReportCounters() {
    ++counters_.frames;
    const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                   std::chrono::steady_clock::now().time_since_epoch()).count());
    if (!counters_start_ns_) {
        counters_start_ns_ = now;
    }
    const double seconds = static_cast<double>(now - counters_start_ns_) * 1e-9;
    if (seconds < 10.0) {
        return;
    }
    const double n = static_cast<double>(counters_.frames);
    LogInfo("d3d12 backend: {} frames in {:.1f} s, per frame: {:.2f} pipeline states made, {:.2f} fence waits ({:.2f} ms), {:.2f} blocking "
            "submissions, {:.2f} command lists executed, {:.1f} barriers",
            counters_.frames, seconds, counters_.pso_creates / n, counters_.fence_waits / n, counters_.fence_wait_ms / n, counters_.submits / n,
            counters_.executes / n, counters_.barriers / n);
    if (allocator_) {
        D3D12MA::Budget local{};
        D3D12MA::Budget system{};
        allocator_->GetBudget(&local, &system);
        LogInfo("d3d12 backend: video memory {} MB used of {} MB budget ({} MB allocated), system memory {} MB used ({} MB allocated)",
                local.UsageBytes >> 20, local.BudgetBytes >> 20, local.Stats.BlockBytes >> 20, system.UsageBytes >> 20,
                system.Stats.BlockBytes >> 20);
    }
    counters_ = {};
    counters_start_ns_ = now;
}

bool DeviceImpl::CreatePassMarkers() {
    if (const char* off = std::getenv("PT_D3D12_MARKERS"); off && off[0] == '0') {
        LogInfo("d3d12 backend: pass markers off (PT_D3D12_MARKERS=0)");
        return false;
    }
    ComPtr<ID3D12Device3> device3;
    if (FAILED(bridge_.Get()->QueryInterface(IID_PPV_ARGS(&device3)))) {
        LogInfo("d3d12 backend: no pass markers: the device has no ID3D12Device3");
        return false;
    }
    const size_t slot_bytes = PassMarkers::kLabels * 2 * sizeof(uint32_t);
    const size_t bytes = (slot_bytes * frames_.size() + 0xFFFF) & ~static_cast<size_t>(0xFFFF);
    marker_memory_ = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    HRESULT hr = marker_memory_ ? device3->OpenExistingHeapFromAddress(marker_memory_, IID_PPV_ARGS(&marker_heap_)) : E_OUTOFMEMORY;
    if (SUCCEEDED(hr)) {
        const D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(bytes, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
        hr = bridge_.Get()->CreatePlacedResource(marker_heap_.Get(), 0, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&marker_buffer_));
    }
    if (FAILED(hr)) {
        LogInfo("d3d12 backend: no pass markers: a heap over the process's memory could not be made ({})", Hr(hr));
        marker_buffer_.Reset();
        marker_heap_.Reset();
        if (marker_memory_) {
            VirtualFree(marker_memory_, 0, MEM_RELEASE);
            marker_memory_ = nullptr;
        }
        return false;
    }
    marker_buffer_->SetName(L"pt pass markers");
    markers_.resize(frames_.size());
    for (size_t i = 0; i < markers_.size(); ++i) {
        markers_[i].cpu = reinterpret_cast<volatile uint32_t*>(static_cast<uint8_t*>(marker_memory_) + i * slot_bytes);
        markers_[i].gpu = marker_buffer_->GetGPUVirtualAddress() + i * slot_bytes;
    }
    LogInfo("d3d12 backend: pass markers on (up to {} labels in each of the {} frame lists; PT_D3D12_MARKERS=0 turns them off)",
            PassMarkers::kLabels, markers_.size());
    return true;
}

void DeviceImpl::LogPassMarkers() {
    std::vector<const PassMarkers*> order;
    for (const PassMarkers& m : markers_) {
        if (m.serial) {
            order.push_back(&m);
        }
    }
    std::sort(order.begin(), order.end(), [](const PassMarkers* a, const PassMarkers* b) { return a->serial > b->serial; });
    for (const PassMarkers* m : order) {
        uint32_t begun = 0;
        uint32_t ended = 0;
        uint32_t open = 0;
        std::string open_names;
        const char* last_ended = nullptr;
        for (uint32_t i = 0; i < m->count; ++i) {
            const bool b = m->cpu[2 * i] == m->serial;
            const bool e = m->cpu[2 * i + 1] == m->serial;
            begun += b ? 1 : 0;
            ended += e ? 1 : 0;
            if (e) {
                last_ended = m->names[i];
            }
            if (b && !e && ++open <= 8) {
                open_names += open_names.empty() ? "" : ", ";
                open_names += m->names[i] ? m->names[i] : "?";
            }
        }
        if (m->count && ended == m->count) {
            LogError("d3d12 backend: pass markers of frame {}: all {} passes finished", m->serial, m->count);
            continue;
        }
        LogError("d3d12 backend: pass markers of frame {}: {} of {} passes begun, {} finished; begun and not finished: {}{}; the last "
                 "finished: {}",
                 m->serial, begun, m->count, ended, open_names.empty() ? "none" : open_names,
                 open > 8 ? std::format(" and {} more", open - 8) : std::string(), last_ended ? last_ended : "none");
    }
}

bool DeviceImpl::NewFrame(Frame& frame, const wchar_t* name) {
    if (FAILED(bridge_.Get()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator))) ||
        FAILED(bridge_.Get()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frame.allocator.Get(), nullptr, IID_PPV_ARGS(&frame.list)))) {
        return false;
    }
    frame.list->SetName(name);
    frame.list->Close();
    frame.recorder = std::make_unique<CommandListImpl>(*this, frame.list.Get());
    frame.value = 0;
    return true;
}

uint64_t DeviceImpl::Execute(Frame& frame) {
    frame.list->Close();
    ID3D12CommandList* lists[] = {frame.list.Get()};
    bridge_.Queue()->ExecuteCommandLists(1, lists);
    std::lock_guard lock(mutex_);
    ++counters_.executes;
    ++submitted_;
    bridge_.Queue()->Signal(fence_.Get(), submitted_);
    frame.value = submitted_;
    for (auto& [resource, allocation] : frame.recorder->TakeKept()) {
        Garbage g;
        g.value = submitted_;
        g.resource = std::move(resource);
        g.allocation = allocation;
        garbage_.push_back(std::move(g));
    }
    return submitted_;
}

void DeviceImpl::WaitValue(uint64_t value) {
    if (!fence_) {
        return;
    }
    if (fence_->GetCompletedValue() >= value || !event_) {
        return;
    }
    const auto wait_started = std::chrono::steady_clock::now();
    DWORD waited = WAIT_TIMEOUT;
    bool test_late = false;
    for (;;) {
        const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - wait_started).count();
        if (spent >= 10000) {
            waited = WAIT_TIMEOUT;
            break;
        }
        ResetEvent(event_);
        fence_->SetEventOnCompletion(value, event_);
        if (test_late_fence_ && --test_late_fence_ == 0) {
            LogWarn("d3d12: test: the wait for fence value {} ends as one that ran out (PT_D3D12_TEST_LATE_FENCE)", value);
            test_late = true;
            waited = WAIT_TIMEOUT;
            break;
        }
        waited = WaitForSingleObject(event_, static_cast<DWORD>(10000 - spent));
        if (waited != WAIT_OBJECT_0 || fence_->GetCompletedValue() >= value) {
            break;
        }
    }
    ++counters_.fence_waits;
    counters_.fence_wait_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wait_started).count();
    if (fence_->GetCompletedValue() < value) {
        std::string reason;
        if (!bridge_.Removed(&reason)) {
            if (!test_late) {
                LogError("d3d12 backend: the GPU did not reach fence value {} within 10 s (completed {})", value, fence_->GetCompletedValue());
            }
            for (int i = 0; i < 100 && !bridge_.Removed(&reason) && fence_->GetCompletedValue() < value; ++i) {
                Sleep(100);
            }
        }
        if (fence_->GetCompletedValue() < value || bridge_.Removed()) {
            hung_ = !bridge_.Removed();
            DeviceLost("a frame fence wait");
        } else {
            LogWarn("d3d12 backend: fence value {} was reached late, the device is not removed", value);
        }
    }
    bridge_.DrainDebugMessages();
}

void DeviceImpl::Collect() {
    const uint64_t done = fence_ ? fence_->GetCompletedValue() : UINT64_MAX;
    std::vector<Garbage> keep;
    for (Garbage& g : garbage_) {
        if (g.value > done) {
            keep.push_back(std::move(g));
            continue;
        }
        g.resource.Reset();
        if (g.allocation) {
            g.allocation->Release();
        }
        if (g.heap) {
            g.heap->Free(g.index, g.count);
        }
    }
    garbage_ = std::move(keep);
}

void DeviceImpl::Release(ComPtr<ID3D12Resource> resource, D3D12MA::Allocation* allocation, DescriptorHeap* heap, uint32_t index, uint32_t count) {
    Garbage g;
    g.value = submitted_;
    g.resource = std::move(resource);
    g.allocation = allocation;
    g.heap = heap;
    g.index = index;
    g.count = count;
    garbage_.push_back(std::move(g));
}

void DeviceImpl::ReleaseDescriptors(DescriptorHeap& heap, uint32_t index, uint32_t count) {
    if (index != kNone) {
        Release(nullptr, nullptr, &heap, index, count);
    }
}

bool DeviceImpl::CreateRawBuffer(uint64_t size, D3D12_RESOURCE_STATES state, bool uav, ComPtr<ID3D12Resource>& out, D3D12MA::Allocation*& allocation,
                                 const char* name) {
    D3D12MA::ALLOCATION_DESC a{};
    a.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    if (test_budget_bytes_ && OverTestBudget(size, name)) {
        return false;
    }
    const D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(size, uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
    const HRESULT hr = allocator_->CreateResource(&a, &desc, state, nullptr, &allocation, IID_PPV_ARGS(&out));
    if (FAILED(hr)) {
        LogError("d3d12 backend: buffer of {} bytes ({}) failed: {}", size, name ? name : "unnamed", Hr(hr));
        LostResult(hr, "a buffer creation");
        return false;
    }
    if (name) {
        out->SetName(Wide(name).c_str());
    }
    return true;
}

bool DeviceImpl::CreateTempBuffer(uint64_t size, ComPtr<ID3D12Resource>& out, D3D12MA::Allocation*& allocation) {
    D3D12MA::ALLOCATION_DESC a{};
    a.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    const D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(size);
    return SUCCEEDED(allocator_->CreateResource(&a, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, &allocation, IID_PPV_ARGS(&out)));
}

bool DeviceImpl::CreateUploadBuffer(uint64_t size, ComPtr<ID3D12Resource>& out, D3D12MA::Allocation*& allocation, void** mapped) {
    if (test_upload_fail_ == 2 || (test_upload_fail_ == 1 && (test_upload_attempts_++ & 1) == 0)) {
        last_upload_hr_ = E_OUTOFMEMORY;
        return false;
    }
    D3D12MA::ALLOCATION_DESC a{};
    a.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    const D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(size);
    if (const HRESULT hr = allocator_->CreateResource(&a, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, &allocation, IID_PPV_ARGS(&out)); FAILED(hr)) {
        last_upload_hr_ = hr;
        return false;
    }
    const D3D12_RANGE none{0, 0};
    if (const HRESULT hr = out->Map(0, &none, mapped); FAILED(hr)) {
        last_upload_hr_ = hr;
        out.Reset();
        if (allocation) {
            allocation->Release();
            allocation = nullptr;
        }
        return false;
    }
    return true;
}

bool DeviceImpl::ReclaimUploadMemory() {
    if (bridge_.Removed()) {
        DeviceLost("a staging upload");
        return false;
    }
    WaitValue(submitted_);
    Collect();
    return true;
}

void DeviceImpl::UploadRetried(uint64_t size, bool succeeded) {
    uint64_t& count = succeeded ? upload_retries_ : upload_failures_;
    std::chrono::steady_clock::time_point& logged = succeeded ? upload_retry_logged_ : upload_failure_logged_;
    ++count;
    const auto now = std::chrono::steady_clock::now();
    if (count > 8 && now - logged < std::chrono::seconds(10)) {
        return;
    }
    logged = now;
    if (succeeded) {
        LogWarn("d3d12 backend: staging memory for a {} byte upload was found after waiting for the GPU ({} times so far)", size, count);
    } else {
        LogError("d3d12 backend: no staging memory for a {} byte upload even after waiting for the GPU ({}, {} times so far): the buffer keeps "
                 "its earlier contents until a later upload succeeds",
                 size, Hr(last_upload_hr_), count);
    }
}

void DeviceImpl::LostResult(HRESULT hr, const char* where) {
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR ||
        bridge_.Removed()) {
        DeviceLost(where);
    }
}

void DeviceImpl::DeviceLost(const char* where) {
    std::string reason;
    if (!bridge_.Removed(&reason)) {
        reason = hung_ ? "the GPU stopped making progress (fence timeout), the device is not removed" : "the present reported it, no removal reason";
    }
    if (!lost_reported_) {
        lost_reported_ = true;
        LogError("d3d12 backend: device lost at {}: {}", where, reason);
        bridge_.LogDred();
        LogPassMarkers();
    }
    if (!rendering_) {
        return;
    }
    FatalError(std::string("the graphics device was lost (") + where +
                   "). The GPU driver stopped or reset the device: a driver timeout, an unstable overclock or a fault in a pass.",
               presenting_);
}

void DeviceImpl::Submit(const std::function<void(CommandList&)>& record) {
    Frame frame;
    {
        std::lock_guard lock(mutex_);
        if (!spare_.empty()) {
            frame = std::move(spare_.back());
            spare_.pop_back();
        }
    }
    if (!frame.allocator && !NewFrame(frame, L"pt one-time list")) {
        LogError("d3d12 backend: no command list for a submission");
        return;
    }
    frame.allocator->Reset();
    frame.list->Reset(frame.allocator.Get(), nullptr);
    frame.recorder->Begin();
    record(*frame.recorder);
    ++counters_.submits;
    WaitValue(Execute(frame));
    Collect();
    std::lock_guard lock(mutex_);
    spare_.push_back(std::move(frame));
}

void DeviceImpl::SubmitDeferred(const std::function<void(CommandList&)>& record, BufferResource staging) {
    Deferred d;
    if (!NewFrame(d.frame, L"pt deferred list")) {
        LogError("d3d12 backend: no command list for a deferred submission");
        return;
    }
    d.frame.allocator->Reset();
    d.frame.list->Reset(d.frame.allocator.Get(), nullptr);
    d.frame.recorder->Begin();
    record(*d.frame.recorder);
    d.value = Execute(d.frame);
    d.staging = staging;
    deferred_.push_back(std::move(d));
    ReapDeferred(false);
}

void DeviceImpl::ReapDeferred(bool wait) {
    std::vector<Deferred> keep;
    for (Deferred& d : deferred_) {
        if (wait) {
            WaitValue(d.value);
        }
        if (!fence_ || fence_->GetCompletedValue() >= d.value) {
            DestroyBuffer(d.staging);
        } else {
            keep.push_back(std::move(d));
        }
    }
    deferred_ = std::move(keep);
    Collect();
}

void DeviceImpl::WaitIdle() {
    WaitValue(submitted_);
    bridge_.WaitIdle();
    Collect();
}

CommandList* DeviceImpl::BeginFrame(uint32_t slot) {
    rendering_ = true;
    if (test_remove_frame_ && ++test_frames_begun_ == test_remove_frame_) {
        ComPtr<ID3D12Device5> device5;
        if (SUCCEEDED(bridge_.Get()->QueryInterface(IID_PPV_ARGS(&device5)))) {
            LogWarn("d3d12: test device removal at frame {} (PT_D3D12_TEST_REMOVE)", test_remove_frame_);
            device5->RemoveDevice();
        }
    }
    Frame& f = frames_[slot % frames_.size()];
    WaitValue(f.value);
    Collect();
    f.allocator->Reset();
    f.list->Reset(f.allocator.Get(), nullptr);
    f.recorder->Begin();
    if (!markers_.empty()) {
        PassMarkers& m = markers_[slot % markers_.size()];
        static const uint32_t test_markers = [] {
            const char* v = std::getenv("PT_D3D12_TEST_MARKERS");
            return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10)) : 0u;
        }();
        if (test_markers && m.serial == test_markers) {
            std::string labels;
            for (uint32_t i = 0; i < m.count; ++i) {
                labels += std::format("{}{} {}/{}", i ? ", " : "", m.names[i] ? m.names[i] : "?", m.cpu[2 * i] == m.serial ? "begun" : "-",
                                      m.cpu[2 * i + 1] == m.serial ? "ended" : "-");
            }
            LogInfo("d3d12 backend: test pass markers of frame {} ({} labels): {}", m.serial, m.count, labels);
            LogPassMarkers();
        }
        m.serial = ++marker_serial_;
        m.count = 0;
        f.recorder->SetMarkers(&m);
    }
    if (active_->Alive()) {
        active_->FrameStart(frame_id_ + 1);
        active_->Marker(pt::d3d12::LatencyMarker::SimulationEnd, frame_id_ + 1);
        active_->Marker(pt::d3d12::LatencyMarker::RenderSubmitStart, frame_id_ + 1);
    }
    return f.recorder.get();
}

bool DeviceImpl::EndFrame(uint32_t slot, RenderTarget* final_image) {
    Frame& f = frames_[slot % frames_.size()];
    ImageRecord* image = final_image && final_image->Valid() ? Rec<ImageRecord>(final_image->image.image) : nullptr;
    const bool present = active_->Alive() && image;
    pt::d3d12::FrameInputs in;
    if (present) {
        f.recorder->Transition(*image, active_->InputState());
        final_image->layout = Layout::Undefined;
        in.color = image->resource.Get();
        if (fg_inputs_set_ && active_ != &presenter_) {
            auto hand_over = [&](RenderTarget* t) -> ID3D12Resource* {
                ImageRecord* rec = t && t->Valid() ? Rec<ImageRecord>(t->image.image) : nullptr;
                if (!rec) {
                    return nullptr;
                }
                f.recorder->Transition(*rec, active_->InputState());
                t->layout = Layout::Undefined;
                return rec->resource.Get();
            };
            in.hudless = hand_over(fg_inputs_.hudless);
            in.ui = hand_over(fg_inputs_.ui);
            in.depth = hand_over(fg_inputs_.depth);
            in.motion = hand_over(fg_inputs_.motion);
            in.input_width = fg_inputs_.input_width;
            in.input_height = fg_inputs_.input_height;
            std::memcpy(in.constants.view, fg_inputs_.view, sizeof(in.constants.view));
            std::memcpy(in.constants.projection, fg_inputs_.projection, sizeof(in.constants.projection));
            in.constants.jitter[0] = fg_inputs_.jitter[0];
            in.constants.jitter[1] = fg_inputs_.jitter[1];
            in.constants.motion_scale[0] = fg_inputs_.motion_scale[0];
            in.constants.motion_scale[1] = fg_inputs_.motion_scale[1];
            in.constants.reset = fg_inputs_.reset;
            in.constants.frame_ms = fg_inputs_.frame_ms;
            in.camera.near_plane = fg_inputs_.near_plane;
            in.camera.fov_y = fg_inputs_.fov_y;
            for (int i = 0; i < 3; ++i) {
                in.camera.position[i] = fg_inputs_.position[i];
                in.camera.up[i] = fg_inputs_.up[i];
                in.camera.right[i] = fg_inputs_.right[i];
                in.camera.forward[i] = fg_inputs_.forward[i];
            }
        }
    }
    fg_inputs_set_ = false;
    const uint64_t value = Execute(f);
    ReportCounters();
    if (!present) {
        if (bridge_.Removed()) {
            DeviceLost("frame submission");
        }
        return true;
    }
    in.wait_fence = fence_.Get();
    in.wait_value = value;
    in.frame_id = ++frame_id_;
    active_->Marker(pt::d3d12::LatencyMarker::RenderSubmitEnd, in.frame_id);
    uint64_t consumed = 0;
    const pt::d3d12::PresentResult r = active_->Present(in, consumed);
    if (r == pt::d3d12::PresentResult::Lost) {
        DeviceLost("present");
    }
    if (bridge_.Removed()) {
        DeviceLost("frame submission");
    }
    return r == pt::d3d12::PresentResult::Ok || r == pt::d3d12::PresentResult::Occluded;
}

bool DeviceImpl::CreateSwapchain(HWND__* window, uint32_t width, uint32_t height, Format format, bool vsync, std::string& error,
                                 float hdr_peak_nits) {
    pt::d3d12::SwapchainDesc d;
    d.window = window;
    d.width = width;
    d.height = height;
    d.format = ToDxgi(format);
    d.buffer_count = 3;
    d.vsync = vsync;
    d.color_space = format == Format::R16G16B16A16Sfloat       ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                    : format == Format::A2B10G10R10UnormPack32 ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                                               : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    d.max_nits = hdr_peak_nits;
    d.max_frame_average_nits = hdr_peak_nits * 0.5f;
    bridge_.LogPresentEnvironment(window);
    if (wanted_owner_ != FrameGenOwner::None) {
        if (CreateOwnerSwapchain(d, error)) {
            swapchain_format_ = format;
            swapchain_extent_ = {width, height};
            presenting_ = true;
            return true;
        }
        LogError("d3d12 backend: frame generation swapchain: {}", error);
        error.clear();
    }
    active_ = &presenter_;
    active_owner_ = FrameGenOwner::None;
    if (!presenter_.Create(d, error)) {
        return false;
    }
    swapchain_format_ = format;
    swapchain_extent_ = {width, height};
    presenting_ = true;
    return true;
}

bool DeviceImpl::DisplayHdr(HWND__* window, float& peak_nits) {
    peak_nits = 0.0f;
    RECT r{};
    if (!GetWindowRect(window, &r)) {
        return false;
    }
    ComPtr<IDXGIOutput> best;
    long best_area = -1;
    ComPtr<IDXGIOutput> output;
    for (UINT i = 0; bridge_.Adapter() && SUCCEEDED(bridge_.Adapter()->EnumOutputs(i, &output)); ++i) {
        DXGI_OUTPUT_DESC od{};
        output->GetDesc(&od);
        const long w = std::max(0L, std::min(r.right, od.DesktopCoordinates.right) - std::max(r.left, od.DesktopCoordinates.left));
        const long h = std::max(0L, std::min(r.bottom, od.DesktopCoordinates.bottom) - std::max(r.top, od.DesktopCoordinates.top));
        if (w * h > best_area) {
            best_area = w * h;
            best = output;
        }
        output.Reset();
    }
    ComPtr<IDXGIOutput6> output6;
    if (!best || FAILED(best.As(&output6))) {
        return false;
    }
    DXGI_OUTPUT_DESC1 desc{};
    if (FAILED(output6->GetDesc1(&desc))) {
        return false;
    }
    peak_nits = desc.MaxLuminance;
    return desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
}

void DeviceImpl::DestroySwapchain() {
    active_->Destroy();
    presenter_.Destroy();
    active_ = &presenter_;
    active_owner_ = FrameGenOwner::None;
}

bool DeviceImpl::CreateImage(const ImageDesc& desc, ImageResource& out) {
    out = {};
    auto rec = std::make_unique<ImageRecord>();
    rec->desc = desc;
    const bool sampled = desc.usage.Has(ImageUsage::Sampled) || desc.usage.Has(ImageUsage::Storage);
    const bool depth = Describe(desc.format).depth;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = desc.extent.depth > 1 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = desc.extent.width;
    rd.Height = std::max(desc.extent.height, 1u);
    rd.DepthOrArraySize = static_cast<UINT16>(desc.extent.depth > 1 ? desc.extent.depth : desc.layers);
    rd.MipLevels = static_cast<UINT16>(desc.mip_levels);
    rd.Format = ResourceFormat(desc.format, sampled);
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (desc.usage.Has(ImageUsage::ColorAttachment)) {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }
    if (desc.usage.Has(ImageUsage::DepthStencilAttachment)) {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        if (!sampled) {
            rd.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        }
    }
    if (desc.usage.Has(ImageUsage::Storage)) {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    D3D12MA::ALLOCATION_DESC a{};
    a.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    if (rd.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) {
        a.Flags = D3D12MA::ALLOCATION_FLAG_COMMITTED;
    }
    if (test_budget_bytes_ && OverTestBudget(bridge_.Get()->GetResourceAllocationInfo(0, 1, &rd).SizeInBytes, desc.name)) {
        return false;
    }
    const HRESULT hr = allocator_->CreateResource(&a, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, &rec->allocation, IID_PPV_ARGS(&rec->resource));
    if (FAILED(hr)) {
        LogError("d3d12 backend: image {}x{}x{} format {} ({}) failed: {}", desc.extent.width, desc.extent.height, rd.DepthOrArraySize,
                 static_cast<uint32_t>(desc.format), desc.name ? desc.name : "unnamed", Hr(hr));
        LostResult(hr, "an image creation");
        return false;
    }
    if (desc.name) {
        rec->resource->SetName(Wide(desc.name).c_str());
    }
    rec->view_format = ShaderViewFormat(desc.format);
    ID3D12Device* device = bridge_.Get();
    if (desc.usage.Has(ImageUsage::ColorAttachment)) {
        rec->rtv = rtvs_.Allocate(1);
        D3D12_RENDER_TARGET_VIEW_DESC v{};
        v.Format = ToDxgi(desc.format);
        v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(rec->resource.Get(), &v, rtvs_.Cpu(rec->rtv));
    }
    if (depth) {
        rec->dsv = dsvs_.Allocate(1);
        rec->dsv_read_only = dsvs_.Allocate(1);
        D3D12_DEPTH_STENCIL_VIEW_DESC v{};
        v.Format = ToDxgi(desc.format);
        v.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(rec->resource.Get(), &v, dsvs_.Cpu(rec->dsv));
        v.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
        device->CreateDepthStencilView(rec->resource.Get(), &v, dsvs_.Cpu(rec->dsv_read_only));
    }
    if (desc.usage.Has(ImageUsage::Storage)) {
        rec->clear_uav_cpu = cpu_views_.Allocate(1);
        rec->clear_uav_gpu = views_.Allocate(1);
        D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
        v.Format = rec->view_format;
        v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(rec->resource.Get(), nullptr, &v, cpu_views_.Cpu(rec->clear_uav_cpu));
        device->CreateUnorderedAccessView(rec->resource.Get(), nullptr, &v, views_.Cpu(rec->clear_uav_gpu));
    }
    out.image = MakeHandle<ImageHandle>(rec.get());
    out.view = ImageViewHandle{out.image.bits};
    out.allocation = AllocationHandle{reinterpret_cast<uintptr_t>(rec->allocation)};
    out.format = desc.format;
    out.extent = desc.extent;
    out.mip_levels = desc.mip_levels;
    out.layers = desc.layers;
    out.usage = desc.usage;
    rec.release();
    return true;
}

bool DeviceImpl::WrapRenderTarget(void* resource, Format format, Extent2D extent, ImageResource& out) {
    out = {};
    if (!resource) {
        return false;
    }
    auto rec = std::make_unique<ImageRecord>();
    rec->desc.format = format;
    rec->desc.extent = {extent.width, extent.height, 1};
    rec->desc.usage = ImageUsage::ColorAttachment;
    rec->resource = static_cast<ID3D12Resource*>(resource);
    rec->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    rec->view_format = ShaderViewFormat(format);
    rec->rtv = rtvs_.Allocate(1);
    D3D12_RENDER_TARGET_VIEW_DESC v{};
    v.Format = ToDxgi(format);
    v.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    bridge_.Get()->CreateRenderTargetView(rec->resource.Get(), &v, rtvs_.Cpu(rec->rtv));
    out.image = MakeHandle<ImageHandle>(rec.get());
    out.view = ImageViewHandle{out.image.bits};
    out.format = format;
    out.extent = rec->desc.extent;
    out.usage = rec->desc.usage;
    rec.release();
    return true;
}

void DeviceImpl::DestroyImage(ImageResource& image) {
    ImageRecord* rec = Rec<ImageRecord>(image.image);
    if (!rec) {
        return;
    }
    NoteImageDestroyed();
    rtvs_.Free(rec->rtv, 1);
    dsvs_.Free(rec->dsv, 1);
    dsvs_.Free(rec->dsv_read_only, 1);
    cpu_views_.Free(rec->clear_uav_cpu, 1);
    ReleaseDescriptors(views_, rec->clear_uav_gpu, 1);
    Release(std::move(rec->resource), rec->allocation);
    delete rec;
    image = {};
}

uint64_t DeviceImpl::ImageBytes(const ImageResource& image) const {
    const ImageRecord* rec = Rec<ImageRecord>(image.image);
    return rec && rec->allocation ? rec->allocation->GetSize() : 0;
}

bool DeviceImpl::CreateBuffer(const BufferDesc& desc, BufferResource& out) {
    out = {};
    auto rec = std::make_unique<BufferRecord>();
    rec->size = std::max<uint64_t>(desc.size, 4);
    rec->usage = desc.usage;
    D3D12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Buffer(rec->size);
    D3D12MA::ALLOCATION_DESC a{};
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    const bool storage = desc.usage.Has(BufferUsage::StorageBuffer);
    rec->gpu_read_only = desc.gpu_read_only;
    const bool cpu_reads = desc.host_visible && desc.usage == BufferUsageFlags(BufferUsage::TransferDst);
    if (desc.readback || cpu_reads) {
        rec->heap = HeapKind::Readback;
        a.HeapType = D3D12_HEAP_TYPE_READBACK;
        state = D3D12_RESOURCE_STATE_COPY_DEST;
    } else if (desc.host_visible && desc.gpu_read_only && storage) {
        if (gpu_upload_) {
            rec->heap = HeapKind::Upload;
            a.HeapType = D3D12_HEAP_TYPE_GPU_UPLOAD;
            state = D3D12_RESOURCE_STATE_GENERIC_READ;
        } else {
            a.HeapType = D3D12_HEAP_TYPE_DEFAULT;
            rec->host_data.resize(rec->size);
            rec->mapped = rec->host_data.data();
            rec->dirty_end = rec->size;
        }
    } else if (desc.host_visible && storage) {
        rec->heap = HeapKind::HostStorage;
        a.HeapType = D3D12_HEAP_TYPE_CUSTOM;
        a.CustomPool = nullptr;
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    } else if (desc.host_visible) {
        rec->heap = HeapKind::Upload;
        a.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        state = D3D12_RESOURCE_STATE_GENERIC_READ;
    } else {
        a.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        if (storage && !desc.gpu_read_only) {
            rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }
    }
    HRESULT hr;
    if (rec->heap == HeapKind::HostStorage) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_CUSTOM;
        heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
        heap.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
        hr = bridge_.Get()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&rec->resource));
    } else {
        if (test_budget_bytes_ && a.HeapType == D3D12_HEAP_TYPE_DEFAULT && OverTestBudget(rec->size, desc.name)) {
            return false;
        }
        hr = allocator_->CreateResource(&a, &rd, state, nullptr, &rec->allocation, IID_PPV_ARGS(&rec->resource));
    }
    if (FAILED(hr)) {
        LogError("d3d12 backend: buffer of {} bytes ({}) failed: {}", rec->size, desc.name ? desc.name : "unnamed", Hr(hr));
        LostResult(hr, "a buffer creation");
        return false;
    }
    rec->state = state;
    if (desc.name) {
        rec->resource->SetName(Wide(desc.name).c_str());
    }
    if (rec->heap != HeapKind::Default) {
        const D3D12_RANGE none{0, 0};
        rec->resource->Map(0, rec->heap == HeapKind::Readback ? nullptr : &none, &rec->mapped);
    }
    out.buffer = MakeHandle<BufferHandle>(rec.get());
    out.allocation = AllocationHandle{reinterpret_cast<uintptr_t>(rec->allocation)};
    out.mapped = rec->mapped;
    out.size = rec->size;
    rec.release();
    return true;
}

void DeviceImpl::DestroyBuffer(BufferResource& buffer) {
    BufferRecord* rec = Rec<BufferRecord>(buffer.buffer);
    if (!rec) {
        return;
    }
    Release(std::move(rec->resource), rec->allocation);
    delete rec;
    buffer = {};
}

void DeviceImpl::FlushMapped(const BufferResource& buffer, uint64_t offset, uint64_t size) {
    BufferRecord* rec = Rec<BufferRecord>(buffer.buffer);
    if (!rec || rec->host_data.empty() || offset >= rec->size || size == 0) {
        return;
    }
    const uint64_t end = offset + std::min(size, rec->size - offset);
    rec->dirty_begin = rec->dirty_end ? std::min(rec->dirty_begin, offset) : offset;
    rec->dirty_end = std::max(rec->dirty_end, end);
}

SamplerHandle DeviceImpl::CreateSampler(const SamplerDesc& desc) {
    auto* rec = new SamplerRecord;
    rec->desc = ToSampler(desc);
    return MakeHandle<SamplerHandle>(rec);
}

void DeviceImpl::DestroySampler(SamplerHandle sampler) {
    delete Rec<SamplerRecord>(sampler);
}

BindingLayoutHandle DeviceImpl::CreateBindingLayout(const BindingLayoutDesc& desc) {
    auto* rec = new LayoutRecord;
    rec->desc = desc;
    return MakeHandle<BindingLayoutHandle>(rec);
}

void DeviceImpl::DestroyBindingLayout(BindingLayoutHandle layout) {
    delete Rec<LayoutRecord>(layout);
}

BindingPoolHandle DeviceImpl::CreateBindingPool(const BindingPoolDesc&) {
    return MakeHandle<BindingPoolHandle>(new PoolRecord);
}

void DeviceImpl::DestroyBindingPool(BindingPoolHandle pool) {
    PoolRecord* rec = Rec<PoolRecord>(pool);
    if (!rec) {
        return;
    }
    for (const std::unique_ptr<SetRecord>& set : rec->sets) {
        ReleaseDescriptors(views_, set->views, kSetBlock);
        ReleaseDescriptors(samplers_, set->samplers, kSlotsPerClass);
        for (const SetRecord::Array& a : set->arrays) {
            ReleaseDescriptors(a.sampler ? samplers_ : views_, a.base, a.count);
        }
    }
    delete rec;
}

void DeviceImpl::WriteNullView(uint32_t index, BindingType type, bool uav) {
    ID3D12Device* device = bridge_.Get();
    const D3D12_CPU_DESCRIPTOR_HANDLE at = views_.Cpu(index);
    const bool buffer = type == BindingType::StorageBuffer || type == BindingType::UniformBuffer;
    if (uav) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
        if (buffer) {
            v.Format = DXGI_FORMAT_R32_TYPELESS;
            v.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            v.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        } else {
            v.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        }
        device->CreateUnorderedAccessView(nullptr, nullptr, &v, at);
        return;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC v{};
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (buffer) {
        v.Format = DXGI_FORMAT_R32_TYPELESS;
        v.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        v.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    } else {
        v.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Texture2D.MipLevels = 1;
    }
    device->CreateShaderResourceView(nullptr, &v, at);
}

D3D12_CPU_DESCRIPTOR_HANDLE DeviceImpl::SamplerCpu(const D3D12_SAMPLER_DESC& desc, uint32_t index) {
    const D3D12_CPU_DESCRIPTOR_HANDLE at = samplers_.Cpu(index);
    bridge_.Get()->CreateSampler(&desc, at);
    return at;
}

void DeviceImpl::FillNull(SetRecord& set) {
    for (uint32_t i = 0; i < kSlotsPerClass; ++i) {
        WriteNullView(set.views + i, BindingType::SampledImage, false);
        WriteNullView(set.views + kSlotsPerClass + i, BindingType::StorageBuffer, true);
        bridge_.Get()->CreateConstantBufferView(nullptr, views_.Cpu(set.views + 2 * kSlotsPerClass + i));
        if (set.samplers != kNone) {
            SamplerCpu(DefaultSampler(), set.samplers + i);
        }
    }
    for (const SetRecord::Array& a : set.arrays) {
        for (uint32_t i = 0; i < a.count; ++i) {
            if (a.sampler) {
                SamplerCpu(DefaultSampler(), a.base + i);
            } else {
                WriteNullView(a.base + i, a.type, a.writable);
            }
        }
    }
}

BindingSetHandle DeviceImpl::AllocateBindingSet(BindingPoolHandle pool, BindingLayoutHandle layout, uint32_t variable_count) {
    PoolRecord* p = Rec<PoolRecord>(pool);
    const LayoutRecord* l = Rec<LayoutRecord>(layout);
    if (!p || !l) {
        return {};
    }
    auto set = std::make_unique<SetRecord>();
    set->layout = l;
    set->views = views_.Allocate(kSetBlock);
    bool any_sampler = false;
    for (size_t i = 0; i < l->desc.bindings.size(); ++i) {
        const BindingDesc& b = l->desc.bindings[i];
        const bool sampler_type = b.type == BindingType::Sampler || b.type == BindingType::CombinedImageSampler;
        if (!l->IsArray(b)) {
            any_sampler = any_sampler || sampler_type;
            continue;
        }
        SetRecord::Array a;
        a.binding = b.binding;
        a.type = b.type;
        a.sampler = b.type == BindingType::Sampler;
        a.writable = b.writable || b.type == BindingType::StorageImage;
        const bool last = i + 1 == l->desc.bindings.size();
        a.count = last && variable_count ? variable_count : b.count;
        a.base = (a.sampler ? samplers_ : views_).Allocate(a.count);
        if (a.base == kNone) {
            LogError("d3d12 backend: the descriptor heap is full ({} descriptors for binding {})", a.count, b.binding);
            return {};
        }
        set->arrays.push_back(a);
    }
    if (any_sampler) {
        set->samplers = samplers_.Allocate(kSlotsPerClass);
    }
    if (set->views == kNone || (any_sampler && set->samplers == kNone)) {
        LogError("d3d12 backend: the descriptor heaps are full ({} of {} views, {} of {} samplers)", views_.Used(), views_.Capacity(),
                 samplers_.Used(), samplers_.Capacity());
        return {};
    }
    FillNull(*set);
    SetRecord* raw = set.get();
    p->sets.push_back(std::move(set));
    return MakeHandle<BindingSetHandle>(raw);
}

void DeviceImpl::WriteBindings(std::span<const BindingWrite> writes) {
    ID3D12Device* device = bridge_.Get();
    for (const BindingWrite& w : writes) {
        SetRecord* set = Rec<SetRecord>(w.set);
        if (!set) {
            continue;
        }
        const SetRecord::Array* array = nullptr;
        for (const SetRecord::Array& a : set->arrays) {
            if (a.binding == w.binding && a.type == w.type) {
                array = &a;
            }
        }
        auto srv_at = [&](uint32_t i) { return array ? array->base + w.first + i : set->views + w.binding; };
        auto uav_at = [&](uint32_t i) { return array ? array->base + w.first + i : set->views + kSlotsPerClass + w.binding; };
        auto cbv_at = [&](uint32_t) { return set->views + 2 * kSlotsPerClass + w.binding; };
        auto smp_at = [&](uint32_t i) { return array && array->sampler ? array->base + w.first + i : set->samplers + w.binding; };
        if (!array && w.binding >= kSlotsPerClass) {
            LogError("d3d12 backend: binding {} is past the {} registers of a set", w.binding, kSlotsPerClass);
            continue;
        }
        for (size_t i = 0; i < w.images.size(); ++i) {
            const ImageBinding& ib = w.images[i];
            const ImageRecord* img = ib.image ? Rec<ImageRecord>(ib.image->image) : reinterpret_cast<ImageRecord*>(static_cast<uintptr_t>(ib.view.bits));
            if (!img) {
                continue;
            }
            const uint32_t idx = static_cast<uint32_t>(i);
            if (w.type == BindingType::StorageImage) {
                D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
                v.Format = img->view_format;
                v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                device->CreateUnorderedAccessView(img->resource.Get(), nullptr, &v, views_.Cpu(uav_at(idx)));
                continue;
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = img->view_format;
            v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (img->desc.cube) {
                v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                v.TextureCube.MipLevels = img->desc.mip_levels;
            } else if (img->desc.extent.depth > 1) {
                v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                v.Texture3D.MipLevels = img->desc.mip_levels;
            } else if (img->desc.layers > 1) {
                v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                v.Texture2DArray.MipLevels = img->desc.mip_levels;
                v.Texture2DArray.ArraySize = img->desc.layers;
            } else {
                v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                v.Texture2D.MipLevels = img->desc.mip_levels;
            }
            device->CreateShaderResourceView(img->resource.Get(), &v, views_.Cpu(srv_at(idx)));
            if (w.type == BindingType::CombinedImageSampler && !array && ib.sampler && set->samplers != kNone) {
                SamplerCpu(Rec<SamplerRecord>(ib.sampler)->desc, smp_at(idx));
            }
        }
        for (size_t i = 0; i < w.buffers.size(); ++i) {
            const BufferBinding& bb = w.buffers[i];
            BufferRecord* buf = Rec<BufferRecord>(bb.buffer);
            if (!buf) {
                continue;
            }
            const uint32_t idx = static_cast<uint32_t>(i);
            const uint64_t key = (uint64_t(w.binding) << 32) | (w.first + idx);
            if (!buf->host_data.empty()) {
                set->uploads[key] = buf;
            } else {
                set->uploads.erase(key);
            }
            const uint64_t range = bb.range == ~0ull ? buf->size - bb.offset : bb.range;
            if (w.type == BindingType::UniformBuffer) {
                D3D12_CONSTANT_BUFFER_VIEW_DESC v{};
                v.BufferLocation = buf->resource->GetGPUVirtualAddress() + bb.offset;
                v.SizeInBytes = static_cast<UINT>((range + 255) & ~255ull);
                device->CreateConstantBufferView(&v, views_.Cpu(cbv_at(idx)));
                continue;
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC s{};
            s.Format = DXGI_FORMAT_R32_TYPELESS;
            s.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            s.Buffer.FirstElement = bb.offset / 4;
            s.Buffer.NumElements = static_cast<UINT>(range / 4);
            s.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
            u.Format = DXGI_FORMAT_R32_TYPELESS;
            u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            u.Buffer.FirstElement = bb.offset / 4;
            u.Buffer.NumElements = static_cast<UINT>(range / 4);
            u.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            const bool uav_capable = !buf->gpu_read_only && (buf->heap == HeapKind::Default || buf->heap == HeapKind::HostStorage);
            if (!array) {
                device->CreateShaderResourceView(buf->resource.Get(), &s, views_.Cpu(srv_at(idx)));
                if (uav_capable) {
                    device->CreateUnorderedAccessView(buf->resource.Get(), nullptr, &u, views_.Cpu(uav_at(idx)));
                }
            } else if (array->writable) {
                if (uav_capable) {
                    device->CreateUnorderedAccessView(buf->resource.Get(), nullptr, &u, views_.Cpu(uav_at(idx)));
                } else {
                    LogError("d3d12 backend: a buffer in an upload or readback heap cannot be written by shaders (binding {})", w.binding);
                }
            } else {
                device->CreateShaderResourceView(buf->resource.Get(), &s, views_.Cpu(srv_at(idx)));
            }
        }
        for (size_t i = 0; i < w.samplers.size(); ++i) {
            const SamplerRecord* s = Rec<SamplerRecord>(w.samplers[i]);
            if (s && (array ? array->sampler : set->samplers != kNone)) {
                SamplerCpu(s->desc, smp_at(static_cast<uint32_t>(i)));
            }
        }
    }
}

PipelineLayoutHandle DeviceImpl::CreatePipelineLayout(const PipelineLayoutDesc& desc) {
    auto* rec = new PipelineLayoutRecord;
    for (BindingLayoutHandle h : desc.sets) {
        rec->sets.push_back(Rec<LayoutRecord>(h));
    }
    rec->push_bytes = desc.push_bytes;
    if (desc.push_bytes > 128) {
        LogError("d3d12 backend: a push block of {} bytes is more than the 32 root constants", desc.push_bytes);
    }
    return MakeHandle<PipelineLayoutHandle>(rec);
}

void DeviceImpl::DestroyPipelineLayout(PipelineLayoutHandle layout) {
    delete Rec<PipelineLayoutRecord>(layout);
}

bool DeviceImpl::LoadDxil(const std::string& name, std::vector<uint8_t>& out) const {
    const std::filesystem::path path = ShaderDir() / (name + ".dxil");
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        LogError("d3d12 backend: shader {} not found", pt::os::PathToUtf8(path));
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !out.empty();
}

PipelineHandle DeviceImpl::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    auto rec = std::make_unique<PipelineRecord>();
    rec->vertex_name = desc.vertex ? desc.vertex : "";
    rec->fragment_name = desc.fragment ? desc.fragment : "";
    rec->desc = desc;
    rec->desc.vertex = rec->vertex_name.c_str();
    rec->desc.fragment = desc.fragment ? rec->fragment_name.c_str() : nullptr;
    if (!LoadDxil(rec->vertex_name, rec->vs) || (desc.fragment && !LoadDxil(rec->fragment_name, rec->ps))) {
        return {};
    }
    if (desc.mesh_input) {
        const D3D12_INPUT_ELEMENT_DESC mesh[] = {
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, normal), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, tangent), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 3, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, uv0), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 4, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, uv1), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 5, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, color), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 6, DXGI_FORMAT_R8G8B8A8_UINT, 0, offsetof(Vertex, joints), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 7, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(Vertex, weights), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 8, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, uv2), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        rec->inputs.assign(std::begin(mesh), std::end(mesh));
        rec->stride = sizeof(Vertex);
    } else {
        for (size_t i = 0; i < desc.vertex_attributes.size(); ++i) {
            rec->inputs.push_back({"TEXCOORD", static_cast<UINT>(i), ToDxgi(desc.vertex_attributes[i].format), 0, desc.vertex_attributes[i].offset,
                                   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0});
        }
        rec->stride = desc.vertex_stride;
    }
    rec->dynamic_bias = desc.depth_bias && info_.dynamic_depth_bias;
    const float no_bias[3] = {};
    if (!Variant(*rec, CullMode::None, FrontFace::CounterClockwise, no_bias)) {
        return {};
    }
    return MakeHandle<PipelineHandle>(rec.release());
}

ID3D12PipelineState* DeviceImpl::Variant(PipelineRecord& p, CullMode cull, FrontFace front, const float bias[3]) {
    const uint64_t key = VariantKey(cull, front, p.dynamic_bias, p.desc.depth_bias, bias);
    if (auto it = p.variants.find(key); it != p.variants.end()) {
        return it->second.Get();
    }
    const GraphicsPipelineDesc& d = p.desc;
    struct Stream {
        CD3DX12_PIPELINE_STATE_STREAM_FLAGS flags;
        CD3DX12_PIPELINE_STATE_STREAM_ROOT_SIGNATURE root;
        CD3DX12_PIPELINE_STATE_STREAM_INPUT_LAYOUT inputs;
        CD3DX12_PIPELINE_STATE_STREAM_PRIMITIVE_TOPOLOGY topology;
        CD3DX12_PIPELINE_STATE_STREAM_VS vs;
        CD3DX12_PIPELINE_STATE_STREAM_PS ps;
        CD3DX12_PIPELINE_STATE_STREAM_BLEND_DESC blend;
        CD3DX12_PIPELINE_STATE_STREAM_RASTERIZER raster;
        CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL1 depth;
        CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL_FORMAT dsv;
        CD3DX12_PIPELINE_STATE_STREAM_RENDER_TARGET_FORMATS rtvs;
        CD3DX12_PIPELINE_STATE_STREAM_SAMPLE_DESC sample;
    } s;
    s.flags = p.dynamic_bias ? D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS : D3D12_PIPELINE_STATE_FLAG_NONE;
    s.root = root_.Get();
    s.inputs = D3D12_INPUT_LAYOUT_DESC{p.inputs.data(), static_cast<UINT>(p.inputs.size())};
    s.topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    s.vs = D3D12_SHADER_BYTECODE{p.vs.data(), p.vs.size()};
    s.ps = D3D12_SHADER_BYTECODE{p.ps.empty() ? nullptr : p.ps.data(), p.ps.size()};
    D3D12_BLEND_DESC blend{};
    blend.IndependentBlendEnable = TRUE;
    for (size_t i = 0; i < d.colors.size() && i < 8; ++i) {
        if (d.fixed_state) {
            ExplicitBlend(i < d.blend_states.size() ? d.blend_states[i] : BlendState{}, blend.RenderTarget[i]);
        } else {
            Blend(i < d.blends.size() ? d.blends[i] : d.blend, i < d.write_masks.size() ? d.write_masks[i] : d.write_mask, blend.RenderTarget[i]);
        }
    }
    s.blend = CD3DX12_BLEND_DESC(blend);
    D3D12_RASTERIZER_DESC raster{};
    raster.FillMode = D3D12_FILL_MODE_SOLID;
    raster.CullMode = cull == CullMode::Front ? D3D12_CULL_MODE_FRONT : cull == CullMode::Back ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    raster.FrontCounterClockwise = front == FrontFace::CounterClockwise ? TRUE : FALSE;
    raster.DepthClipEnable = TRUE;
    if (d.depth_bias && !p.dynamic_bias) {
        raster.DepthBias = static_cast<INT>(bias[0]);
        raster.DepthBiasClamp = bias[1];
        raster.SlopeScaledDepthBias = bias[2];
    }
    s.raster = CD3DX12_RASTERIZER_DESC(raster);
    D3D12_DEPTH_STENCIL_DESC1 ds{};
    const bool has_depth = d.depth != Format::Undefined;
    ds.DepthEnable = has_depth && d.depth_test ? TRUE : FALSE;
    ds.DepthWriteMask = has_depth && d.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = ToD3D(d.depth_compare);
    ds.DepthBoundsTestEnable = has_depth && d.depth_bounds && info_.depth_bounds ? TRUE : FALSE;
    s.depth = CD3DX12_DEPTH_STENCIL_DESC1(ds);
    s.dsv = has_depth ? ToDxgi(d.depth) : DXGI_FORMAT_UNKNOWN;
    D3D12_RT_FORMAT_ARRAY rt{};
    rt.NumRenderTargets = static_cast<UINT>(std::min<size_t>(d.colors.size(), 8));
    for (UINT i = 0; i < rt.NumRenderTargets; ++i) {
        rt.RTFormats[i] = ToDxgi(d.colors[i]);
    }
    s.rtvs = rt;
    s.sample = DXGI_SAMPLE_DESC{1, 0};
    D3D12_PIPELINE_STATE_STREAM_DESC stream{sizeof(s), &s};
    ComPtr<ID3D12PipelineState> state;
    ++counters_.pso_creates;
    const HRESULT hr = device2_->CreatePipelineState(&stream, IID_PPV_ARGS(&state));
    bridge_.DrainDebugMessages();
    if (FAILED(hr)) {
        LogError("d3d12 backend: pipeline {} + {} failed: {}", p.vertex_name, p.fragment_name.empty() ? "(no fragment)" : p.fragment_name, Hr(hr));
        LostResult(hr, "a pipeline creation");
        return nullptr;
    }
    state->SetName(Wide((p.vertex_name + " " + p.fragment_name).c_str()).c_str());
    ID3D12PipelineState* raw = state.Get();
    p.variants.emplace(key, std::move(state));
    return raw;
}

PipelineHandle DeviceImpl::CreateComputePipeline(const ComputePipelineDesc& desc) {
    auto rec = std::make_unique<PipelineRecord>();
    rec->compute = true;
    rec->vertex_name = desc.shader ? desc.shader : "";
    if (!LoadDxil(rec->vertex_name, rec->vs)) {
        return {};
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
    d.pRootSignature = root_.Get();
    d.CS = {rec->vs.data(), rec->vs.size()};
    ++counters_.pso_creates;
    const HRESULT hr = bridge_.Get()->CreateComputePipelineState(&d, IID_PPV_ARGS(&rec->compute_state));
    bridge_.DrainDebugMessages();
    if (FAILED(hr)) {
        LogError("d3d12 backend: compute pipeline {} failed: {}", rec->vertex_name, Hr(hr));
        LostResult(hr, "a pipeline creation");
        return {};
    }
    rec->compute_state->SetName(Wide(rec->vertex_name.c_str()).c_str());
    return MakeHandle<PipelineHandle>(rec.release());
}

void DeviceImpl::DestroyPipeline(PipelineHandle pipeline) {
    delete Rec<PipelineRecord>(pipeline);
}

QueryPoolHandle DeviceImpl::CreateTimestampPool(uint32_t count) {
    auto rec = std::make_unique<QueryPoolRecord>();
    D3D12_QUERY_HEAP_DESC q{};
    q.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    q.Count = count;
    if (FAILED(bridge_.Get()->CreateQueryHeap(&q, IID_PPV_ARGS(&rec->heap)))) {
        return {};
    }
    D3D12MA::ALLOCATION_DESC a{};
    a.HeapType = D3D12_HEAP_TYPE_READBACK;
    const D3D12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Buffer(sizeof(uint64_t) * count);
    if (FAILED(allocator_->CreateResource(&a, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, &rec->allocation, IID_PPV_ARGS(&rec->readback)))) {
        return {};
    }
    rec->readback->Map(0, nullptr, reinterpret_cast<void**>(&rec->mapped));
    rec->count = count;
    return MakeHandle<QueryPoolHandle>(rec.release());
}

void DeviceImpl::DestroyQueryPool(QueryPoolHandle pool) {
    QueryPoolRecord* rec = Rec<QueryPoolRecord>(pool);
    if (!rec) {
        return;
    }
    Release(std::move(rec->readback), rec->allocation);
    delete rec;
}

bool DeviceImpl::ReadTimestamps(QueryPoolHandle pool, uint32_t first, uint32_t count, uint64_t* ticks) {
    const QueryPoolRecord* rec = Rec<QueryPoolRecord>(pool);
    if (!rec || first + count > rec->count) {
        return false;
    }
    std::memcpy(ticks, rec->mapped + first, sizeof(uint64_t) * count);
    return true;
}

bool DeviceImpl::ReadImage(RenderTarget& target, std::vector<uint8_t>& out) {
    ImageRecord* img = Rec<ImageRecord>(target.image.image);
    if (!img) {
        return false;
    }
    D3D12_RESOURCE_DESC rd = img->resource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 row_bytes = 0;
    UINT64 total = 0;
    bridge_.Get()->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_bytes, &total);
    BufferResource readback;
    BufferDesc bd;
    bd.size = total;
    bd.readback = true;
    bd.name = "pt image readback";
    if (!CreateBuffer(bd, readback)) {
        return false;
    }
    BufferRecord* rb = Rec<BufferRecord>(readback.buffer);
    const D3D12_RESOURCE_STATES back = StateFor(target.layout) == D3D12_RESOURCE_STATE_COMMON ? img->state : StateFor(target.layout);
    Submit([&](CommandList& list) {
        auto& l = static_cast<CommandListImpl&>(list);
        l.Transition(*img, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{rb->resource.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        dst.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION src{img->resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        src.SubresourceIndex = 0;
        l.List()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        l.Transition(*img, back);
    });
    const FormatInfo info = Describe(target.image.format);
    const uint32_t width = target.image.extent.width;
    const uint32_t tight = info.block ? ((width + 3) / 4) * info.bytes : width * info.bytes;
    out.resize(static_cast<size_t>(tight) * rows);
    const auto* src = static_cast<const uint8_t*>(rb->mapped);
    for (UINT r = 0; r < rows; ++r) {
        std::memcpy(out.data() + static_cast<size_t>(r) * tight, src + fp.Offset + static_cast<size_t>(r) * fp.Footprint.RowPitch, tight);
    }
    DestroyBuffer(readback);
    return true;
}

ID3D12Device* NativeDevice(gpu::Device& device) {
    return static_cast<DeviceImpl&>(device).Dev();
}

ID3D12CommandQueue* NativeQueue(gpu::Device& device) {
    return static_cast<DeviceImpl&>(device).Bridge().Queue();
}

pt::d3d12::Device& NativeBridgeDevice(gpu::Device& device) {
    return static_cast<DeviceImpl&>(device).Bridge();
}

ID3D12GraphicsCommandList* Native(gpu::CommandList& list) {
    return static_cast<CommandListImpl&>(list).List();
}

ID3D12Resource* Native(ImageHandle image) {
    ImageRecord* r = Rec<ImageRecord>(image);
    return r ? r->resource.Get() : nullptr;
}

ID3D12Resource* Native(BufferHandle buffer) {
    BufferRecord* r = Rec<BufferRecord>(buffer);
    return r ? r->resource.Get() : nullptr;
}

D3D12_RESOURCE_STATES TrackedState(ImageHandle image) {
    ImageRecord* r = Rec<ImageRecord>(image);
    return r ? r->state : D3D12_RESOURCE_STATE_COMMON;
}

uint64_t DeviceImpl::AllocatedBytes() const {
    if (!allocator_) {
        return 0;
    }
    D3D12MA::Budget local{};
    D3D12MA::Budget system{};
    allocator_->GetBudget(&local, &system);
    return local.Stats.BlockBytes + system.Stats.BlockBytes;
}

void DeviceImpl::DeviceLocalUsage(double& used, double& budget) const {
    used = budget = 0.0;
    if (!allocator_) {
        return;
    }
    D3D12MA::Budget local{};
    allocator_->GetBudget(&local, nullptr);
    used = static_cast<double>(local.UsageBytes);
    budget = static_cast<double>(local.BudgetBytes);
    if (test_budget_bytes_) {
        budget = std::min(budget, static_cast<double>(test_budget_bytes_));
    }
}

bool DeviceImpl::OverTestBudget(uint64_t bytes, const char* what) const {
    D3D12MA::Budget local{};
    allocator_->GetBudget(&local, nullptr);
    if (local.Stats.BlockBytes + bytes <= test_budget_bytes_) {
        return false;
    }
    LogWarn("d3d12 backend: {} ({} bytes) over the test video memory budget", what ? what : "unnamed", bytes);
    return true;
}

void ResumeAfterNative(gpu::CommandList& list) {
    static_cast<CommandListImpl&>(list).ResumeAfterNative();
}

}
