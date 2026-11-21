#include "engine/platform/os.h"
#include "engine/render/renderer.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#define STBIW_WINDOWS_UTF8
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/core/log.h"
#include "engine/core/crash_report.h"
#include "engine/render/hdr_output.h"
#include "engine/render/hdr_png.h"
#include "engine/render/pipeline_cache_store.h"
#include "engine/render/upscale/frame_generation.h"
#include "engine/render/upscale/streamline.h"
#include "engine/render/upscale/upscale.h"

namespace pt {
namespace {

struct RemakeTest {
    std::vector<uint32_t> frames;
    bool until_same = false;
    bool same_seen = false;
    uint32_t begun = 0;
};

RemakeTest& TestRemake() {
    static RemakeTest test = [] {
        RemakeTest t;
        const char* v = std::getenv("PT_TEST_REMAKE_TARGETS");
        if (v && std::string_view(v) == "aba") {
            t.until_same = true;
            return t;
        }
        for (const char* c = v; c && *c;) {
            char* end = nullptr;
            const unsigned long n = std::strtoul(c, &end, 10);
            if (end == c) {
                break;
            }
            t.frames.push_back(static_cast<uint32_t>(n));
            c = *end == ',' ? end + 1 : end;
        }
        return t;
    }();
    return test;
}

bool TestRemakeTargets() {
    RemakeTest& t = TestRemake();
    ++t.begun;
    if (t.until_same) {
        return !t.same_seen && t.begun >= 60 && t.begun % 2 == 0 && t.begun < 2000;
    }
    return std::find(t.frames.begin(), t.frames.end(), t.begun) != t.frames.end();
}

void TestRemadeTargets(uint64_t old_view, uint64_t new_view) {
    const bool same = old_view == new_view;
    TestRemake().same_seen = TestRemake().same_seen || same;
    LogInfo("renderer: test remake of the output targets (PT_TEST_REMAKE_TARGETS) at frame {}: scene colour view {:#x} -> {:#x}{}",
            TestRemake().begun, old_view, new_view, same ? ", the same bits" : "");
}

VkFormat g_imgui_format = VK_FORMAT_UNDEFINED;

gpu::SamplerDesc LinearClamp() {
    gpu::SamplerDesc s;
    s.mag = gpu::Filter::Linear;
    s.min = gpu::Filter::Linear;
    s.mipmap = gpu::MipmapMode::Nearest;
    s.address_u = s.address_v = s.address_w = gpu::AddressMode::ClampToEdge;
    s.max_anisotropy = 0.0f;
    s.max_lod = 0.0f;
    return s;
}

gpu::ImageBinding Bind(gpu::ImageViewHandle view, gpu::SamplerHandle sampler) {
    return {view, gpu::Layout::ShaderReadOnly, sampler};
}

gpu::ImageBarrier ColorBarrier(gpu::ImageHandle image, gpu::PipelineStage src_stage, uint64_t src_access, gpu::Layout old_layout,
                               gpu::PipelineStage dst_stage, uint64_t dst_access, gpu::Layout new_layout) {
    return {image, gpu::Aspect::Color, src_stage, src_access, old_layout, dst_stage, dst_access, new_layout};
}

void ImGuiCheck(VkResult result) {
    if (result != VK_SUCCESS) {
        LogError("imgui vulkan: {}", vk::ResultName(result));
    }
}

float HalfToFloat(uint16_t h) {
    const uint32_t sign = uint32_t(h & 0x8000u) << 16;
    uint32_t exponent = (h >> 10) & 0x1fu;
    uint32_t mantissa = h & 0x03ffu;
    uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            int shift = 0;
            while ((mantissa & 0x0400u) == 0) {
                mantissa <<= 1;
                ++shift;
            }
            mantissa &= 0x03ffu;
            bits = sign | (uint32_t(127 - 15 - shift) << 23) | (mantissa << 13);
        }
    } else if (exponent == 0x1fu) {
        bits = sign | 0x7f800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
    }
    return std::bit_cast<float>(bits);
}

}

bool Renderer::Init(SDL_Window* window, const RendererSettings& settings) {
    settings_ = settings;
    if (const char* brightness = std::getenv("PT_OUTPUT_BRIGHTNESS")) {
        brightness_override_ = static_cast<float>(std::atof(brightness));
    }
    ctx_.pipeline_cache_dir = settings.pipeline_cache_dir;
    ctx_.hdr_paper_nits = render::ClampHdrPaperWhite(static_cast<float>(settings.hdr_paper_white));
    ctx_.hdr_peak_setting = settings.hdr_peak;
    hdr_ui_nits_ = settings.hdr_ui_white > 0 ? render::ClampHdrPaperWhite(static_cast<float>(settings.hdr_ui_white)) : 0.0f;
    SetHdrReach(settings.hdr_reach);
    SetHdrLook(settings.hdr_look);
    window_ = settings.headless ? nullptr : window;
    if (settings.backend == gpu::Backend::D3D12) {
        return InitD3D12(settings);
    }
    if (!ctx_.Init(window_, settings.validation, settings.hdr && !settings.headless)) {
        return false;
    }
    gpu_ = std::make_unique<gpu::vk::Device>(ctx_);
    uint32_t width = settings.width;
    uint32_t height = settings.height;
    gpu::Format output_format = gpu::Format::R8G8B8A8Unorm;
    if (settings.headless) {
        const char* forced = std::getenv("PT_HDR_HEADLESS");
        if (forced && std::string_view(forced) == "scrgb") {
            output_format = gpu::Format::R16G16B16A16Sfloat;
            output_mode_ = RendererOutputMode::ScRgb;
        } else if (forced && std::string_view(forced) == "hdr10") {
            output_format = gpu::Format::A2B10G10R10UnormPack32;
            output_mode_ = RendererOutputMode::Hdr10;
        }
    }
    if (window_) {
        int w = 0;
        int h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        width = static_cast<uint32_t>(w);
        height = static_cast<uint32_t>(h);
        bool created = ctx_.CreateSwapchain(width, height, settings.vsync, settings.hdr);
        if (!created && settings.hdr) {
            LogWarn("vulkan: the HDR swapchain was refused, SDR is used");
            created = ctx_.CreateSwapchain(width, height, settings.vsync, false);
        }
        if (!created) {
            /* DLSS-G's swapchain hook can refuse the window (borderless fullscreen on some setups); the game goes on without it. */
            if (!streamline::DisableFrameGenFeature()) {
                return false;
            }
            LogError("frame generation: DLSS Frame Generation refused the swapchain, turned off for this run");
            UpscaleHost::Get().SetDlssFrameGenFailed(true);
            if (!ctx_.CreateSwapchain(width, height, settings.vsync, settings.hdr)) {
                return false;
            }
        }
        output_format = gpu::vk::FromNative(ctx_.swapchain.format);
        UpdateOutputMode();
        width = ctx_.swapchain.extent.width;
        height = ctx_.swapchain.extent.height;
    }
    output_format_ = output_format;
    final_format_ = gpu::Format::R16G16B16A16Sfloat;
    for (Frame& frame : frames_) {
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.queueFamilyIndex = ctx_.queue_family;
        vkCreateCommandPool(ctx_.device, &pool_info, nullptr, &frame.pool);
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = frame.pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        VkCommandBuffer buffers[3] = {};
        alloc.commandBufferCount = 3;
        vkAllocateCommandBuffers(ctx_.device, &alloc, buffers);
        frame.cmd = buffers[0];
        frame.resume = buffers[1];
        frame.gen_cmd = buffers[2];
        VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(ctx_.device, &semaphore_info, nullptr, &frame.image_available);
        vkCreateSemaphore(ctx_.device, &semaphore_info, nullptr, &frame.gen_available);
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(ctx_.device, &fence_info, nullptr, &frame.in_flight);
        vkCreateFence(ctx_.device, &fence_info, nullptr, &frame.gen_in_flight);
    }
    gpu::SamplerDesc sampler_info = LinearClamp();
    linear_sampler_ = gpu_->CreateSampler(sampler_info);
    sampler_info.address_u = sampler_info.address_v = sampler_info.address_w = gpu::AddressMode::Repeat;
    sampler_info.mipmap = gpu::MipmapMode::Linear;
    sampler_info.max_lod = 1000.0f;
    wrap_sampler_ = gpu_->CreateSampler(sampler_info);
    if (!CreateTargets(width, height) || !CreateCompositePipeline(output_format)) {
        return false;
    }
    if (window_ && !InitImGui(window_)) {
        return false;
    }
    return true;
}

void Renderer::ImGuiNewFrame() {
    if (!imgui_ready_) {
        return;
    }
#ifdef _WIN32
    if (d3d12_) {
        d3d12_->ImGuiNewFrame();
        return;
    }
#endif
    ImGui_ImplVulkan_NewFrame();
}

bool Renderer::InitImGui(SDL_Window* window) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = "pt_imgui.ini";
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForVulkan(window);
    g_imgui_format = gpu::vk::Native(final_format_);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = ctx_.instance;
    info.PhysicalDevice = ctx_.physical;
    info.Device = ctx_.device;
    info.QueueFamily = ctx_.queue_family;
    info.Queue = ctx_.queue;
    info.DescriptorPoolSize = 256;
    info.MinImageCount = ctx_.swapchain.min_image_count;
    info.ImageCount = static_cast<uint32_t>(ctx_.swapchain.images.size());
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_imgui_format;
    info.CheckVkResultFn = ImGuiCheck;
    if (!ImGui_ImplVulkan_Init(&info)) {
        LogError("ImGui_ImplVulkan_Init failed");
        return false;
    }
    imgui_ready_ = true;
    return true;
}

bool Renderer::CreateTargets(uint32_t width, uint32_t height) {
    const vk::AllocTag tag("output targets");
    const gpu::Extent3D extent{width, height, 1};
    auto make = [&](gpu::ImageResource& image, gpu::Format format, gpu::ImageUsageFlags usage) {
        gpu::ImageDesc desc;
        desc.format = format;
        desc.extent = extent;
        desc.usage = usage;
        return gpu_->CreateImage(desc, image);
    };
    using gpu::ImageUsage;
    if (!make(scene_color_, SceneColorFormat(), ImageUsage::ColorAttachment | ImageUsage::Sampled | ImageUsage::TransferSrc)) {
        return false;
    }
    if ((settings_.headless || d3d12_) && !make(output_, output_format_, ImageUsage::ColorAttachment | ImageUsage::TransferSrc)) {
        return false;
    }
    output_ready_ = false;
    if (!settings_.headless && !d3d12_) {
        const VkFormat format = ctx_.swapchain.format;
        const bool capture = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_UNORM ||
                             format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_R16G16B16A16_SFLOAT ||
                             format == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        const gpu::ImageUsageFlags usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc | ImageUsage::TransferDst;
        if (!capture) {
            LogWarn("renderer: swapchain format {} has no screenshot readback path", static_cast<int>(format));
        } else if (!make(output_, gpu::vk::FromNative(format), usage)) {
            LogWarn("renderer: no capture image, windowed screenshots disabled");
        }
    }
    if (!make(final_, final_format_, ImageUsage::ColorAttachment | ImageUsage::Sampled)) {
        return false;
    }
    if (!make(ui_top_, final_format_, ImageUsage::ColorAttachment | ImageUsage::Sampled)) {
        return false;
    }
    ui_top_ready_ = false;
    ui_top_dirty_ = false;
    present_target_ = gpu::RenderTarget{};
    present_target_.image = output_;
    WriteCompositeSets();
    return true;
}

void Renderer::WriteCompositeSets() {
    if (!composite_set_ || !final_set_) {
        return;
    }
    const gpu::ImageViewHandle noise = grain_noise_ ? grain_noise_ : scene_color_.view;
    const gpu::ImageBinding infos[4] = {Bind(scene_color_.view, linear_sampler_), Bind(final_.view, linear_sampler_),
                                        Bind(noise, wrap_sampler_), Bind(noise, wrap_sampler_)};
    gpu::BindingWrite writes[4];
    for (int i = 0; i < 4; ++i) {
        writes[i] = {i % 2 == 0 ? composite_set_ : final_set_, static_cast<uint32_t>(i < 2 ? 0 : 1), 0, gpu::BindingType::CombinedImageSampler,
                     {&infos[i], 1}};
    }
    gpu_->WriteBindings(writes);
    const gpu::ImageBinding top_info = Bind(ui_top_.view, linear_sampler_);
    const gpu::BindingSetHandle top_sets[4] = {composite_set_, final_set_, gen_set_[0], gen_set_[1]};
    gpu::BindingWrite top_writes[4];
    uint32_t top_count = 0;
    for (gpu::BindingSetHandle set : top_sets) {
        if (!set) {
            continue;
        }
        top_writes[top_count++] = {set, 2, 0, gpu::BindingType::CombinedImageSampler, {&top_info, 1}};
    }
    gpu_->WriteBindings({top_writes, top_count});
}

void Renderer::SetGrainNoise(gpu::ImageViewHandle view) {
    if (grain_noise_ == view) {
        return;
    }
    gpu_->WaitIdle();
    grain_noise_ = view;
    WriteCompositeSets();
}

void Renderer::DestroyTargets() {
    gpu_->DestroyImage(scene_color_);
    gpu_->DestroyImage(final_);
    gpu_->DestroyImage(ui_top_);
    gpu_->DestroyImage(output_);
    present_target_ = gpu::RenderTarget{};
}

bool Renderer::CreateCompositePipeline(gpu::Format output_format) {
    gpu::BindingLayoutDesc set_info;
    for (uint32_t b = 0; b < 3; ++b) {
        set_info.bindings.push_back({b, gpu::BindingType::CombinedImageSampler, 1, gpu::ShaderStage::Fragment});
    }
    composite_set_layout_ = gpu_->CreateBindingLayout(set_info);
    if (!composite_set_layout_) {
        LogError("renderer: composite set layout failed");
        return false;
    }
    gpu::BindingPoolDesc pool_info;
    pool_info.sizes = {{gpu::BindingType::CombinedImageSampler, 12}};
    pool_info.max_sets = 4;
    composite_pool_ = gpu_->CreateBindingPool(pool_info);
    if (!composite_pool_) {
        LogError("renderer: composite descriptor pool failed");
        return false;
    }
    const gpu::BindingLayoutHandle layouts[4] = {composite_set_layout_, composite_set_layout_, composite_set_layout_, composite_set_layout_};
    gpu::BindingSetHandle sets[4] = {};
    if (!gpu_->AllocateBindingSets(composite_pool_, layouts, sets)) {
        LogError("renderer: composite descriptor sets failed");
        return false;
    }
    composite_set_ = sets[0];
    final_set_ = sets[1];
    gen_set_[0] = sets[2];
    gen_set_[1] = sets[3];
    WriteCompositeSets();

    gpu::PipelineLayoutDesc layout_info;
    layout_info.sets = {composite_set_layout_};
    layout_info.push_bytes = 128;
    layout_info.push_stages = gpu::ShaderStage::Fragment;
    composite_layout_ = gpu_->CreatePipelineLayout(layout_info);
    return BuildCompositePipeline(output_format, composite_pipeline_) && BuildCompositePipeline(final_format_, final_composite_pipeline_) &&
           CreateLayerPipeline();
}

bool Renderer::CreateLayerPipeline() {
    gpu::BindingLayoutDesc set_info;
    for (uint32_t b = 0; b < 4; ++b) {
        set_info.bindings.push_back({b, gpu::BindingType::CombinedImageSampler, 1, gpu::ShaderStage::Fragment});
    }
    layer_set_layout_ = gpu_->CreateBindingLayout(set_info);
    if (!layer_set_layout_) {
        LogError("renderer: frame generation layer set layout failed");
        return false;
    }
    gpu::BindingPoolDesc pool_info;
    pool_info.sizes = {{gpu::BindingType::CombinedImageSampler, 4 * kFramesInFlight}};
    pool_info.max_sets = kFramesInFlight;
    layer_pool_ = gpu_->CreateBindingPool(pool_info);
    if (!layer_pool_) {
        LogError("renderer: frame generation layer descriptor pool failed");
        return false;
    }
    const gpu::BindingLayoutHandle layouts[kFramesInFlight] = {layer_set_layout_, layer_set_layout_};
    if (!gpu_->AllocateBindingSets(layer_pool_, layouts, layer_sets_)) {
        LogError("renderer: frame generation layer descriptor sets failed");
        return false;
    }
    gpu::PipelineLayoutDesc layout_info;
    layout_info.sets = {layer_set_layout_};
    layout_info.push_bytes = 128;
    layout_info.push_stages = gpu::ShaderStage::Fragment;
    layer_layout_ = gpu_->CreatePipelineLayout(layout_info);
    if (!layer_layout_) {
        LogError("renderer: frame generation layer pipeline layout failed");
        return false;
    }
    return BuildCompositePipeline(gpu::vk::FromNative(kUiLayerFormat), layer_pipeline_, "fg_ui_layer.frag", layer_layout_);
}

gpu::BindingSetHandle Renderer::LayerSet(gpu::ImageViewHandle hudless_view) {
    const gpu::BindingSetHandle set = layer_sets_[frame_index_];
    if (!set) {
        return {};
    }
    const gpu::ImageViewHandle noise = grain_noise_ ? grain_noise_ : scene_color_.view;
    const gpu::ImageBinding infos[4] = {Bind(final_.view, linear_sampler_), Bind(noise, wrap_sampler_), Bind(ui_top_.view, linear_sampler_),
                                        Bind(hudless_view, linear_sampler_)};
    gpu::BindingWrite writes[4];
    for (uint32_t i = 0; i < 4; ++i) {
        writes[i] = {set, i, 0, gpu::BindingType::CombinedImageSampler, {&infos[i], 1}};
    }
    gpu_->WriteBindings(writes);
    return set;
}

bool Renderer::BuildCompositePipeline(gpu::Format format, gpu::PipelineHandle& pipeline, const char* fragment, gpu::PipelineLayoutHandle layout) {
    gpu::GraphicsPipelineDesc desc;
    desc.vertex = "fullscreen.vert";
    desc.fragment = fragment;
    desc.layout = layout ? layout : composite_layout_;
    desc.colors = {format};
    desc.fixed_state = true;
    desc.depth_state = false;
    desc.cull = gpu::CullMode::None;
    gpu::BlendState blend;
    blend.src_color = blend.dst_color = blend.src_alpha = blend.dst_alpha = gpu::BlendFactor::Zero;
    desc.blend_states = {blend};
    pipeline = gpu_->CreateGraphicsPipeline(desc);
    return static_cast<bool>(pipeline);
}

void Renderer::AddPipelineWarmup(vk::PipelineWarmup& warmup) {
    if (d3d12_ || !ctx_.surface || !ctx_.physical) {
        return;
    }
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_.physical, ctx_.surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> offered(count);
    if (count > 0) vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_.physical, ctx_.surface, &count, offered.data());
    for (const VkFormat format : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                                  VK_FORMAT_R16G16B16A16_SFLOAT}) {
        const bool listed = std::any_of(offered.begin(), offered.end(), [&](const VkSurfaceFormatKHR& f) { return f.format == format; });
        if (!listed || gpu::vk::FromNative(format) == output_format_ || gpu::vk::FromNative(format) == final_format_) {
            continue;
        }
        warmup.Add([this, format]() -> uint32_t {
            gpu::PipelineHandle pipeline;
            if (!BuildCompositePipeline(gpu::vk::FromNative(format), pipeline)) {
                return 0;
            }
            gpu_->DestroyPipeline(pipeline);
            return 1;
        });
    }
}

void Renderer::Shutdown() {
    FinishScreenshots();
    if (!gpu_) {
        return;
    }
    gpu_->WaitIdle();
    if (imgui_ready_ && !d3d12_) {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        imgui_ready_ = false;
    }
    if (!d3d12_) {
        DestroyXr();
    }
    if (final_composite_pipeline_) gpu_->DestroyPipeline(final_composite_pipeline_);
    if (layer_pipeline_) gpu_->DestroyPipeline(layer_pipeline_);
    if (layer_layout_) gpu_->DestroyPipelineLayout(layer_layout_);
    if (layer_pool_) gpu_->DestroyBindingPool(layer_pool_);
    if (layer_set_layout_) gpu_->DestroyBindingLayout(layer_set_layout_);
    gpu_->DestroyPipeline(composite_pipeline_);
    gpu_->DestroyPipelineLayout(composite_layout_);
    gpu_->DestroyBindingPool(composite_pool_);
    gpu_->DestroyBindingLayout(composite_set_layout_);
    gpu_->DestroySampler(linear_sampler_);
    gpu_->DestroySampler(wrap_sampler_);
    DestroyTargets();
    if (d3d12_) {
#ifdef _WIN32
        if (imgui_ready_) {
            d3d12_->ShutdownImGui();
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();
            imgui_ready_ = false;
        }
        gpu_->DestroyImage(fg_hudless_.image);
        gpu_->DestroyImage(fg_ui_.image);
        DestroyXrD3D12();
        if (GetModuleHandleW(L"graphics-hook64.dll") || std::getenv("PT_TEST_OBS_EXIT")) {
            LogInfo("renderer: OBS's game capture hook is loaded: the DirectX 12 swapchains and device are left to the process's end");
            (void)gpu_.release();
            d3d12_ = nullptr;
            return;
        }
        d3d12_->DestroySwapchain();
        LogInfo("renderer: DirectX 12 debug layer errors: {}", d3d12_->DebugErrors());
#endif
        gpu_.reset();
        d3d12_ = nullptr;
        return;
    }
    for (Frame& frame : frames_) {
        vkDestroyFence(ctx_.device, frame.in_flight, nullptr);
        vkDestroyFence(ctx_.device, frame.gen_in_flight, nullptr);
        vkDestroySemaphore(ctx_.device, frame.image_available, nullptr);
        vkDestroySemaphore(ctx_.device, frame.gen_available, nullptr);
        vkDestroyCommandPool(ctx_.device, frame.pool, nullptr);
    }
    ctx_.Shutdown();
}

void Renderer::UpdateOutputMode() {
    output_mode_ = ctx_.swapchain.color_space == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT ? RendererOutputMode::ScRgb
                   : ctx_.swapchain.color_space == VK_COLOR_SPACE_HDR10_ST2084_EXT     ? RendererOutputMode::Hdr10
                                                                                       : RendererOutputMode::Sdr;
}

void Renderer::SetHdr(bool enabled) {
    if (settings_.hdr == enabled) return;
    settings_.hdr = enabled;
    if (window_) swapchain_dirty_ = true;
    LogInfo("display: HDR {} (applied at next frame)", enabled ? "on" : "off");
}

void Renderer::SetHdrLevels(int paper_white, int peak, int ui_white) {
    const float paper = render::ClampHdrPaperWhite(static_cast<float>(paper_white));
    hdr_ui_nits_ = ui_white > 0 ? render::ClampHdrPaperWhite(static_cast<float>(ui_white)) : 0.0f;
    if (ctx_.hdr_paper_nits == paper && ctx_.hdr_peak_setting == peak) return;
    ctx_.hdr_paper_nits = paper;
    ctx_.hdr_peak_setting = peak;
    if (d3d12_) {
        if (window_ && output_mode_ != RendererOutputMode::Sdr) swapchain_dirty_ = true;
        return;
    }
    ctx_.SetHdrMetadata();
}

void Renderer::RecheckOutput() {
    if (window_) swapchain_dirty_ = true;
}

void Renderer::SetVsync(bool enabled) {
    if(settings_.vsync == enabled) return;
    settings_.vsync = enabled;
    if(window_) swapchain_dirty_ = true;
    LogInfo("display: VSync {} (applied at next frame)", enabled ? "on" : "off");
}

void Renderer::Resize(uint32_t width, uint32_t height) {
    if (width > 0 && height > 0 && swapchain_window_w_ == static_cast<int>(width) && swapchain_window_h_ == static_cast<int>(height) &&
        !swapchain_dirty_) {
        return;
    }
    swapchain_dirty_ = true;
}

void Renderer::LogLongWait(const char* what, VkResult result, float ms) {
    if (ms < 1000.0f && result != VK_ERROR_DEVICE_LOST) {
        return;
    }
    if (result != VK_ERROR_DEVICE_LOST && ++long_waits_logged_ > 20) {
        return;
    }
    LogWarn("renderer: {} returned {} after {:.3f} s (window flags {:#x}, frame {})", what, vk::ResultName(result), ms / 1000.0f,
            window_ ? static_cast<uint64_t>(SDL_GetWindowFlags(window_)) : uint64_t{0}, frames_begun_);
}

bool Renderer::BeginFrame(bool present) {
    if (d3d12_) {
        return BeginFrameD3D12(present);
    }
    Frame& frame = frames_[frame_index_];
    ++frames_begun_;
    ctx_.pipeline_saver.Poll();
    using wait_ms = std::chrono::duration<float, std::milli>;
    const auto fence_started = std::chrono::steady_clock::now();
    const VkResult fence_result = vkWaitForFences(ctx_.device, 1, &frame.in_flight, VK_TRUE, UINT64_MAX);
    const float fence_ms = wait_ms(std::chrono::steady_clock::now() - fence_started).count();
    LogLongWait("frame fence wait", fence_result, fence_ms);
    ctx_.CheckDeviceLost(fence_result, "frame fence wait");
    cpu_waits_ = CpuWaits{};
    cpu_waits_.fence_ms = fence_ms;
    ctx_.ReapDeferred(false);
    bool keep_current_swapchain = false;
    if (window_ && present && !swapchain_dirty_ && swapchain_window_w_ > 0) {
        int w = 0;
        int h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        if (w > 0 && h > 0 && (w != swapchain_window_w_ || h != swapchain_window_h_)) {
            swapchain_dirty_ = true;
        }
    }
    if (window_ && present && frame_start) {
        const FrameStartAction action = frame_start(swapchain_dirty_);
        if (action == FrameStartAction::RecreateSwapchain) {
            swapchain_dirty_ = true;
        } else if (action == FrameStartAction::KeepCurrentSwapchain) {
            keep_current_swapchain = true;
        }
    }
    const auto sleep_started = std::chrono::steady_clock::now();
    streamline::BeginFrame();
    cpu_waits_.sleep_ms = wait_ms(std::chrono::steady_clock::now() - sleep_started).count();
    grain[0] = 0.0f;
    if (targets_missing_ && !swapchain_dirty_) {
        if (std::chrono::steady_clock::now() < targets_retry_at_) {
            return false;
        }
        vkDeviceWaitIdle(ctx_.device);
        DestroyTargets();
        const VkExtent2D size = render_extent_.width > 0 && render_extent_.height > 0 ? render_extent_ : ctx_.swapchain.extent;
        if (!CreateTargets(size.width, size.height)) {
            DestroyTargets();
            targets_retry_at_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            return false;
        }
        targets_missing_ = false;
        LogInfo("renderer: output targets made again at {}x{}", size.width, size.height);
    }
    const bool test_remake = TestRemakeTargets();
    if (render_extent_.width > 0 && render_extent_.height > 0 &&
        (test_remake || scene_color_.extent.width != render_extent_.width || scene_color_.extent.height != render_extent_.height ||
         scene_color_.format != SceneColorFormat())) {
        vkDeviceWaitIdle(ctx_.device);
        const uint64_t old_view = scene_color_.view.bits;
        DestroyTargets();
        if (!CreateTargets(render_extent_.width, render_extent_.height)) {
            DestroyTargets();
            targets_missing_ = true;
            targets_retry_at_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            return false;
        }
        LogInfo("renderer: render size {}x{}", render_extent_.width, render_extent_.height);
        if (test_remake) {
            TestRemadeTargets(old_view, scene_color_.view.bits);
        }
    }
    presenting_ = window_ && present;
    if (presenting_) {
        if (swapchain_dirty_ && !keep_current_swapchain) {
            int w = 0;
            int h = 0;
            SDL_GetWindowSizeInPixels(window_, &w, &h);
            if (w == 0 || h == 0) {
                return false;
            }
            const VkFormat previous_format = ctx_.swapchain.format;
            const RendererOutputMode previous_mode = output_mode_;
            bool created = ctx_.CreateSwapchain(static_cast<uint32_t>(w), static_cast<uint32_t>(h), settings_.vsync, settings_.hdr);
            if (!created && settings_.hdr && !ctx_.swapchain_refused) {
                created = ctx_.CreateSwapchain(static_cast<uint32_t>(w), static_cast<uint32_t>(h), settings_.vsync, false);
            }
            if (!created) {
                if (ctx_.swapchain_refused && swapchain_failed) {
                    swapchain_failed();
                }
                return false;
            }
            swapchain_window_w_ = w;
            swapchain_window_h_ = h;
            UpdateOutputMode();
            if (ctx_.swapchain.format != previous_format) {
                if (composite_pipeline_) gpu_->DestroyPipeline(composite_pipeline_);
                composite_pipeline_ = {};
                output_format_ = gpu::vk::FromNative(ctx_.swapchain.format);
                if (!BuildCompositePipeline(output_format_, composite_pipeline_)) {
                    return false;
                }
            }
            if (output_mode_ != previous_mode) {
                LogInfo("display: output {} while running", output_mode_ == RendererOutputMode::Sdr ? "SDR" : output_mode_ == RendererOutputMode::ScRgb ? "HDR (scRGB)" : "HDR (HDR10)");
            }
            DestroyTargets();
            if (render_extent_.width > 0 && render_extent_.height > 0) {
                targets_missing_ = !CreateTargets(render_extent_.width, render_extent_.height);
            } else {
                targets_missing_ = !CreateTargets(ctx_.swapchain.extent.width, ctx_.swapchain.extent.height);
            }
            if (targets_missing_) {
                DestroyTargets();
                targets_retry_at_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                swapchain_dirty_ = false;
                return false;
            }
            if (imgui_ready_) {
                ImGui_ImplVulkan_SetMinImageCount(ctx_.swapchain.min_image_count);
            }
            swapchain_dirty_ = false;
        }
        const auto acquire_started = std::chrono::steady_clock::now();
        const VkResult acquire = ctx_.AcquireNextImage(frame.image_available, &image_index_);
        const float acquire_ms = wait_ms(std::chrono::steady_clock::now() - acquire_started).count();
        cpu_waits_.acquire_ms += acquire_ms;
        LogLongWait("swapchain acquire", acquire, acquire_ms);
        ctx_.CheckDeviceLost(acquire, "swapchain acquire");
        if (acquire == VK_NOT_READY || acquire == VK_TIMEOUT) {
            return false;
        }
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
            if (keep_current_swapchain) {
                FatalError("DLSS Frame Generation could not safely change the swapchain, and the retained swapchain is no longer usable. Restart P.T. and try again.",
                           window_ != nullptr);
            }
            swapchain_dirty_ = true;
            return false;
        }
        if (acquire == VK_SUBOPTIMAL_KHR) {
            swapchain_dirty_ = true;
        }
    }
    vkResetFences(ctx_.device, 1, &frame.in_flight);
    if (frame.gen_pending) {
        const auto gen_started = std::chrono::steady_clock::now();
        ctx_.CheckDeviceLost(vkWaitForFences(ctx_.device, 1, &frame.gen_in_flight, VK_TRUE, UINT64_MAX), "generated frame fence wait");
        cpu_waits_.fence_ms += wait_ms(std::chrono::steady_clock::now() - gen_started).count();
        vkResetFences(ctx_.device, 1, &frame.gen_in_flight);
        frame.gen_pending = false;
    }
    vkResetFences(ctx_.device, 1, &frame.gen_in_flight);
    vkResetCommandPool(ctx_.device, frame.pool, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(frame.cmd, &begin);
    active_cmd_ = frame.cmd;
    active_list_ = gpu::vk::CommandList(active_cmd_);
    split_wait_ = VK_NULL_HANDLE;
    split_wait_value_ = 0;
    gen_acquired_ = false;
    if (presenting_ && generated_frame.generate) {
        const auto gen_acquire_started = std::chrono::steady_clock::now();
        const VkResult gen_acquire = ctx_.AcquireNextImage(frame.gen_available, &gen_index_);
        cpu_waits_.acquire_ms += wait_ms(std::chrono::steady_clock::now() - gen_acquire_started).count();
        if (gen_acquire == VK_SUCCESS || gen_acquire == VK_SUBOPTIMAL_KHR) {
            gen_acquired_ = true;
        } else if (gen_acquire != VK_NOT_READY && gen_acquire != VK_TIMEOUT) {
            ctx_.CheckDeviceLost(gen_acquire, "generated frame acquire");
            swapchain_dirty_ = true;
        }
    }

    using gpu::Layout;
    using gpu::PipelineStage;
    gpu::CommandList& cmd = active_list_;
    cmd.Barrier(ColorBarrier(scene_color_.image, PipelineStage::AllCommands, 0, Layout::Undefined, PipelineStage::ColorAttachmentOutput,
                             gpu::Bits(gpu::Access::ColorAttachmentWrite), Layout::ColorAttachment));
    gpu::Attachment color{scene_color_.view, gpu::LoadOp::Clear, {{0.0f, 0.0f, 0.0f, 1.0f}}};
    const gpu::Rect2D area{{0, 0}, RenderExtent()};
    cmd.BeginRendering(area, {&color, 1});
    cmd.EndPass();
    streamline::SetMarker(streamline::Marker::SimulationEnd);
    streamline::SetMarker(streamline::Marker::RenderSubmitStart);
    return true;
}

void Renderer::RecordTopUi(gpu::CommandList& cmd, gpu::Extent2D extent, bool draw, bool after_overlay) {
    if (!ui_top_.image || (!draw && ui_top_ready_ && (!ui_top_dirty_ || !after_overlay))) {
        return;
    }
    using gpu::Layout;
    using gpu::PipelineStage;
    cmd.Barrier(ColorBarrier(ui_top_.image, PipelineStage::FragmentShader, 0, Layout::Undefined, PipelineStage::ColorAttachmentOutput,
                             gpu::Bits(gpu::Access::ColorAttachmentWrite), Layout::ColorAttachment));
    const gpu::Attachment color{ui_top_.view, gpu::LoadOp::Clear, {{0.0f, 0.0f, 0.0f, 0.0f}}};
    cmd.BeginRendering({{0, 0}, extent}, {&color, 1});
    if (draw && overlay) {
        overlay(cmd, ui_top_.view, extent, OverlayPart::Top);
    }
    cmd.EndPass();
    cmd.Barrier(ColorBarrier(ui_top_.image, PipelineStage::ColorAttachmentOutput, gpu::Bits(gpu::Access::ColorAttachmentWrite),
                             Layout::ColorAttachment, PipelineStage::FragmentShader, gpu::Bits(gpu::Access::ShaderSampledRead),
                             Layout::ShaderReadOnly));
    ui_top_ready_ = true;
    ui_top_dirty_ = draw;
}

void Renderer::CompositePush(float (&out)[32], float mode, gpu::Extent2D extent, bool with_top_ui, bool grain_off) const {
    const float brightness = plain_output ? 1.0f : std::clamp(brightness_override_ > 0.0f ? brightness_override_ : output_brightness, 0.1f, 4.0f);
    const float across = extent.height ? (static_cast<float>(extent.width) / static_cast<float>(extent.height)) / (16.0f / 9.0f) : 1.0f;
    const bool hdr_output = output_mode_ != RendererOutputMode::Sdr;
    const float ui_white = HdrUiWhite();
    const float scene_scale = hdr_output && !plain_output ? std::pow(ctx_.hdr_paper_nits / std::max(ui_white, 1.0f), 1.0f / 2.2f) : 1.0f;
    const float first = mode > 0.5f ? (hdr_output && !plain_output ? static_cast<float>(hdr_pattern_) : 0.0f) : exposure;
    const bool look = hdr_output && !plain_output;
    const render::HdrLook& l = hdr_look_;
    const bool graded = look && l.Graded();
    const float push[32] = {first, brightness, mode, static_cast<float>(photo_filter), fade[0], fade[1], fade[2], fade[3],
                            plain_output || grain_off ? 0.0f : grain[0], grain[1], grain[2], grain[3], grain_offset[0], grain_offset[1], across,
                            static_cast<float>(output_mode_), ui_white, HdrPeak(), scene_scale, with_top_ui ? 1.0f : 0.0f,
                            look ? static_cast<float>(std::clamp(l.gamma, 0, 2)) : 0.0f, static_cast<float>(l.colour_range), graded ? 1.0f : 0.0f,
                            look && (graded || l.colour_range != 0) ? 1.0f : 0.0f,
                            std::exp2(l.exposure), l.contrast, l.highlights, l.shadows, l.saturation, l.highlight_saturation, l.blowout, 0.0f};
    std::copy(std::begin(push), std::end(push), std::begin(out));
}

void Renderer::Composite(gpu::CommandList& cmd, gpu::BindingSetHandle set, float mode, gpu::Extent2D extent, gpu::Format target_format,
                         gpu::Offset2D offset, bool with_top_ui, bool grain_off) {
    cmd.BeginLabel(mode > 0.5f ? "final composite" : "scene composite");
    cmd.SetViewport({offset, extent});
    const gpu::PipelineHandle pipeline = target_format == final_format_ ? final_composite_pipeline_ : composite_pipeline_;
    cmd.BindPipeline(gpu::BindPoint::Graphics, pipeline);
    cmd.BindSets(gpu::BindPoint::Graphics, composite_layout_, 0, {&set, 1});
    float push[32];
    CompositePush(push, mode, extent, with_top_ui, grain_off);
    cmd.PushConstants(composite_layout_, gpu::ShaderStage::Fragment, 0, sizeof(push), push);
    cmd.Draw(3, 1, 0, 0);
    cmd.EndLabel();
}

void Renderer::EndFrame(bool draw_ui) {
    if (d3d12_) {
        EndFrameD3D12(draw_ui);
        return;
    }
    using gpu::Access;
    using gpu::Layout;
    using gpu::PipelineStage;
    Frame& frame = frames_[frame_index_];
    VkCommandBuffer cmd = active_cmd_;
    gpu::CommandList& list = active_list_;
    using wait_ms = std::chrono::duration<float, std::milli>;
    const auto end_started = std::chrono::steady_clock::now();
    list.Barrier(ColorBarrier(scene_color_.image, PipelineStage::ColorAttachmentOutput | PipelineStage::ComputeShader,
                              Access::ColorAttachmentWrite | Access::ShaderStorageWrite, Layout::ColorAttachment, PipelineStage::FragmentShader,
                              gpu::Bits(Access::ShaderSampledRead), Layout::ShaderReadOnly));
    const gpu::ImageHandle target_image = window_ ? (presenting_ ? gpu::vk::Wrap<gpu::ImageTag>(ctx_.swapchain.images[image_index_]) : gpu::ImageHandle{})
                                                  : output_.image;
    const gpu::ImageViewHandle target_view =
        window_ ? (presenting_ ? gpu::vk::Wrap<gpu::ImageViewTag>(ctx_.swapchain.views[image_index_]) : gpu::ImageViewHandle{}) : output_.view;
    const gpu::Extent2D extent = RenderExtent();
    const bool xr = std::exchange(xr_pending_, false);
    static const bool ui_over_grain = [] {
        const char* value = std::getenv("PT_UI_OVER_GRAIN");
        return !(value && value[0] == '0');
    }();
    const bool split_ui = overlay && !xr && ui_over_grain;
    RecordTopUi(list, extent, false, false);
    list.Barrier(ColorBarrier(final_.image, PipelineStage::FragmentShader, 0, Layout::Undefined, PipelineStage::ColorAttachmentOutput,
                              gpu::Bits(Access::ColorAttachmentWrite), Layout::ColorAttachment));
    gpu::Attachment color{final_.view, gpu::LoadOp::DontCare, {}};
    gpu::Rect2D rendering{{0, 0}, extent};
    const vk::Image* hud = presenting_ && hudless && !xr ? hudless(image_index_) : nullptr;
    const bool layer_fits = hud && ui_layer && layer_pipeline_ && output_mode_ == RendererOutputMode::Sdr && hud->extent.width == extent.width &&
                            hud->extent.height == extent.height;
    const vk::Image* layer = layer_fits ? ui_layer(image_index_) : nullptr;
    const bool layer_over_rendered = layer && ui_layer_over_rendered && ui_layer_over_rendered();
    auto composite_fitted = [&](gpu::Extent2D area, gpu::Format format, bool with_top_ui) {
        const float scale = std::min(static_cast<float>(area.width) / static_cast<float>(extent.width),
                                     static_cast<float>(area.height) / static_cast<float>(extent.height));
        const gpu::Extent2D shown{std::max(1u, static_cast<uint32_t>(extent.width * scale)), std::max(1u, static_cast<uint32_t>(extent.height * scale))};
        Composite(list, final_set_, 1.0f, shown, format,
                  {static_cast<int32_t>((area.width - shown.width) / 2), static_cast<int32_t>((area.height - shown.height) / 2)}, with_top_ui);
    };
    list.BeginRendering(rendering, {&color, 1});
    Composite(list, composite_set_, 0.0f, extent, final_format_);
    if (overlay && !hud && (!xr || xr_frame_.overlay_on_frame)) {
        overlay(list, final_.view, extent, split_ui ? OverlayPart::Below : OverlayPart::All);
    }
    if (draw_ui && imgui_ready_ && presenting_) {
        ImDrawData* data = ImGui::GetDrawData();
        if (data && data->DisplaySize.x > 0.0f && data->DisplaySize.y > 0.0f) {
            const ImVec2 saved_scale = data->FramebufferScale;
            data->FramebufferScale = ImVec2(static_cast<float>(extent.width) / data->DisplaySize.x,
                                            static_cast<float>(extent.height) / data->DisplaySize.y);
            ImGui_ImplVulkan_RenderDrawData(data, cmd);
            data->FramebufferScale = saved_scale;
        }
    }
    list.EndPass();
    list.Barrier(ColorBarrier(final_.image, PipelineStage::ColorAttachmentOutput, gpu::Bits(Access::ColorAttachmentWrite), Layout::ColorAttachment,
                              PipelineStage::FragmentShader, gpu::Bits(Access::ShaderSampledRead), Layout::ShaderReadOnly));
    if (hud) {
        list.Barrier(ColorBarrier(gpu::vk::Wrap<gpu::ImageTag>(hud->image), PipelineStage::AllCommands, 0, Layout::Undefined, PipelineStage::ColorAttachmentOutput,
                                  gpu::Bits(Access::ColorAttachmentWrite), Layout::ColorAttachment));
        color.view = gpu::vk::Wrap<gpu::ImageViewTag>(hud->view);
        const gpu::Extent2D hud_extent{hud->extent.width, hud->extent.height};
        const bool hud_fitted = hud_extent.width != extent.width || hud_extent.height != extent.height;
        gpu::Rect2D hud_rendering = rendering;
        if (hud_fitted) {
            color.load = gpu::LoadOp::Clear;
            color.clear_value = {{0.0f, 0.0f, 0.0f, 1.0f}};
            hud_rendering = {{0, 0}, hud_extent};
        }
        list.BeginRendering(hud_rendering, {&color, 1});
        if (hud_fitted) {
            composite_fitted(hud_extent, gpu::vk::FromNative(hud->format), false);
        } else {
            Composite(list, final_set_, 1.0f, extent, gpu::vk::FromNative(hud->format), {0, 0}, false, layer != nullptr);
        }
        list.EndPass();
        color.load = gpu::LoadOp::DontCare;
        list.Barrier(ColorBarrier(gpu::vk::Wrap<gpu::ImageTag>(hud->image), PipelineStage::ColorAttachmentOutput, gpu::Bits(Access::ColorAttachmentWrite), Layout::ColorAttachment,
                                  PipelineStage::AllCommands, gpu::Bits(Access::ShaderRead), Layout::ShaderReadOnly));
        if (overlay) {
            list.Barrier(ColorBarrier(final_.image, PipelineStage::FragmentShader, gpu::Bits(Access::ShaderSampledRead), Layout::ShaderReadOnly,
                                      PipelineStage::ColorAttachmentOutput, Access::ColorAttachmentRead | Access::ColorAttachmentWrite,
                                      Layout::ColorAttachment));
            color.view = final_.view;
            color.load = gpu::LoadOp::Load;
            list.BeginRendering(rendering, {&color, 1});
            overlay(list, final_.view, extent, split_ui ? OverlayPart::Below : OverlayPart::All);
            list.EndPass();
            color.load = gpu::LoadOp::DontCare;
            list.Barrier(ColorBarrier(final_.image, PipelineStage::ColorAttachmentOutput, gpu::Bits(Access::ColorAttachmentWrite),
                                      Layout::ColorAttachment, PipelineStage::FragmentShader, gpu::Bits(Access::ShaderSampledRead),
                                      Layout::ShaderReadOnly));
        }
    }
    RecordTopUi(list, extent, split_ui && overlay_top_pending && overlay_top_pending(), true);
    if (layer) {
        const gpu::BindingSetHandle set = LayerSet(gpu::vk::Wrap<gpu::ImageViewTag>(hud->view));
        list.Barrier(ColorBarrier(gpu::vk::Wrap<gpu::ImageTag>(layer->image), PipelineStage::AllCommands, 0, Layout::Undefined, PipelineStage::ColorAttachmentOutput,
                                  gpu::Bits(Access::ColorAttachmentWrite), Layout::ColorAttachment));
        const gpu::Attachment layer_color{gpu::vk::Wrap<gpu::ImageViewTag>(layer->view), set ? gpu::LoadOp::DontCare : gpu::LoadOp::Clear, {{0.0f, 0.0f, 0.0f, 0.0f}}};
        list.BeginRendering({{0, 0}, extent}, {&layer_color, 1});
        if (set) {
            list.SetViewport({{0, 0}, extent});
            list.BindPipeline(gpu::BindPoint::Graphics, layer_pipeline_);
            list.BindSets(gpu::BindPoint::Graphics, layer_layout_, 0, {&set, 1});
            float push[32];
            CompositePush(push, 1.0f, extent, true, false);
            list.PushConstants(layer_layout_, gpu::ShaderStage::Fragment, 0, sizeof(push), push);
            list.Draw(3, 1, 0, 0);
        }
        list.EndPass();
        list.Barrier(ColorBarrier(gpu::vk::Wrap<gpu::ImageTag>(layer->image), PipelineStage::ColorAttachmentOutput, gpu::Bits(Access::ColorAttachmentWrite), Layout::ColorAttachment,
                                  PipelineStage::AllCommands, gpu::Bits(Access::ShaderRead), Layout::ShaderReadOnly));
    }
    if (xr) {
        RecordXr(cmd);
    }
    const gpu::Extent2D window_extent = presenting_ ? gpu::vk::FromNative(ctx_.swapchain.extent) : extent;
    const bool fitted = window_extent.width != extent.width || window_extent.height != extent.height;
    const gpu::Layout present_layout = gpu::vk::FromNative(ctx_.PresentLayout());
    if (target_image) {
        list.Barrier(ColorBarrier(target_image, PipelineStage::ColorAttachmentOutput, 0, Layout::Undefined, PipelineStage::ColorAttachmentOutput,
                                  gpu::Bits(Access::ColorAttachmentWrite), Layout::ColorAttachment));
        color.view = target_view;
        if (fitted) {
            color.load = gpu::LoadOp::Clear;
            color.clear_value = {{0.0f, 0.0f, 0.0f, 1.0f}};
            rendering = {{0, 0}, window_extent};
        }
        if (layer_over_rendered) {
            list.Barrier(ColorBarrier(gpu::vk::Wrap<gpu::ImageTag>(hud->image), PipelineStage::AllCommands, gpu::Bits(Access::ShaderRead), Layout::ShaderReadOnly,
                                      PipelineStage::Transfer, gpu::Bits(Access::TransferRead), Layout::TransferSrc));
            list.Barrier(ColorBarrier(target_image, PipelineStage::ColorAttachmentOutput, gpu::Bits(Access::ColorAttachmentWrite),
                                      Layout::ColorAttachment, PipelineStage::Transfer, gpu::Bits(Access::TransferWrite), Layout::TransferDst));
            gpu::ImageCopy copy;
            copy.src = {gpu::Aspect::Color, 0, 0, 1};
            copy.dst = copy.src;
            copy.extent = {extent.width, extent.height, 1};
            gpu::ImageResource hud_image = *hud;
            gpu::ImageResource target;
            target.image = target_image;
            list.CopyImage(hud_image, Layout::TransferSrc, target, Layout::TransferDst, {&copy, 1});
            list.Barrier(ColorBarrier(gpu::vk::Wrap<gpu::ImageTag>(hud->image), PipelineStage::Transfer, 0, Layout::TransferSrc, PipelineStage::AllCommands,
                                      gpu::Bits(Access::ShaderRead), Layout::ShaderReadOnly));
            list.Barrier(ColorBarrier(target_image, PipelineStage::Transfer, gpu::Bits(Access::TransferWrite), Layout::TransferDst,
                                      PipelineStage::ColorAttachmentOutput, gpu::Bits(Access::ColorAttachmentWrite), Layout::ColorAttachment));
        } else {
            list.BeginRendering(rendering, {&color, 1});
            if (fitted) {
                composite_fitted(window_extent, gpu::vk::FromNative(ctx_.swapchain.format), true);
            } else {
                Composite(list, final_set_, 1.0f, extent, window_ ? gpu::vk::FromNative(ctx_.swapchain.format) : gpu::Format(output_.format), {0, 0},
                          true);
            }
            list.EndPass();
        }
    }
    if (presenting_ && output_.image && !fitted && !layer_over_rendered) {
        list.Barrier(ColorBarrier(target_image, PipelineStage::ColorAttachmentOutput, gpu::Bits(Access::ColorAttachmentWrite),
                                  Layout::ColorAttachment, PipelineStage::Transfer, gpu::Bits(Access::TransferRead), Layout::TransferSrc));
        list.Barrier(ColorBarrier(output_.image, PipelineStage::Transfer, gpu::Bits(Access::TransferWrite), Layout::Undefined,
                                  PipelineStage::Transfer, gpu::Bits(Access::TransferWrite), Layout::TransferDst));
        gpu::ImageCopy copy;
        copy.src = {gpu::Aspect::Color, 0, 0, 1};
        copy.dst = copy.src;
        copy.extent = {extent.width, extent.height, 1};
        gpu::ImageResource target;
        target.image = target_image;
        list.CopyImage(target, Layout::TransferSrc, output_, Layout::TransferDst, {&copy, 1});
        list.Barrier(ColorBarrier(output_.image, PipelineStage::Transfer, gpu::Bits(Access::TransferWrite), Layout::TransferDst,
                                  PipelineStage::Transfer, gpu::Bits(Access::TransferRead), Layout::TransferSrc));
        list.Barrier(ColorBarrier(target_image, PipelineStage::Transfer, 0, Layout::TransferSrc, PipelineStage::BottomOfPipe, 0, present_layout));
        output_ready_ = true;
    } else if (presenting_ && output_.image && (fitted || layer_over_rendered)) {
        list.Barrier(ColorBarrier(output_.image, PipelineStage::AllCommands, 0, Layout::Undefined, PipelineStage::ColorAttachmentOutput,
                                  gpu::Bits(Access::ColorAttachmentWrite), Layout::ColorAttachment));
        color.view = output_.view;
        color.load = gpu::LoadOp::DontCare;
        rendering = {{0, 0}, extent};
        list.BeginRendering(rendering, {&color, 1});
        Composite(list, final_set_, 1.0f, extent, output_.format, {0, 0}, true);
        list.EndPass();
        list.Barrier(ColorBarrier(output_.image, PipelineStage::ColorAttachmentOutput, gpu::Bits(Access::ColorAttachmentWrite),
                                  Layout::ColorAttachment, PipelineStage::Transfer, gpu::Bits(Access::TransferRead), Layout::TransferSrc));
        list.Barrier(ColorBarrier(target_image, PipelineStage::ColorAttachmentOutput, gpu::Bits(Access::ColorAttachmentWrite),
                                  Layout::ColorAttachment, PipelineStage::BottomOfPipe, 0, present_layout));
        output_ready_ = true;
    } else if (presenting_) {
        list.Barrier(ColorBarrier(target_image, PipelineStage::ColorAttachmentOutput, gpu::Bits(Access::ColorAttachmentWrite),
                                  Layout::ColorAttachment, PipelineStage::BottomOfPipe, 0, present_layout));
    } else if (!window_) {
        output_ready_ = true;
        list.Barrier(ColorBarrier(target_image, PipelineStage::ColorAttachmentOutput, gpu::Bits(Access::ColorAttachmentWrite),
                                  Layout::ColorAttachment, PipelineStage::Transfer, gpu::Bits(Access::TransferRead), Layout::TransferSrc));
    }
    vkEndCommandBuffer(cmd);
    if (split_wait_) {
        VkSemaphoreWaitInfo split_wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        split_wait.semaphoreCount = 1;
        split_wait.pSemaphores = &split_wait_;
        split_wait.pValues = &split_wait_value_;
        ctx_.CheckDeviceLost(vkWaitSemaphores(ctx_.device, &split_wait, UINT64_MAX), "MetalFX upscale wait");
        split_wait_ = VK_NULL_HANDLE;
    }

    VkCommandBufferSubmitInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmd_info.commandBuffer = cmd;
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = frame.image_available;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSemaphoreSubmitInfo fg_signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    VkSemaphore fg_wait = VK_NULL_HANDLE;
    uint64_t fg_signal_value = 0;
    uint64_t fg_wait_value = 0;
    const bool fg_sync = presenting_ && gen_acquired_ && generated_frame.sync &&
                         generated_frame.sync(fg_signal.semaphore, fg_signal_value, fg_wait, fg_wait_value);
    fg_signal.value = fg_signal_value;
    fg_signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSemaphoreSubmitInfo waits[2] = {wait, fg_signal};
    VkSemaphoreSubmitInfo signals[2] = {signal, fg_signal};
    uint32_t wait_count = 0;
    uint32_t signal_count = 0;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;
    if (presenting_) {
        signal.semaphore = ctx_.swapchain.render_finished[image_index_];
        waits[wait_count++] = wait;
        signals[signal_count++] = signal;
    }
    if (fg_sync) {
        signals[signal_count++] = fg_signal;
    }
    submit.waitSemaphoreInfoCount = wait_count;
    submit.pWaitSemaphoreInfos = waits;
    submit.signalSemaphoreInfoCount = signal_count;
    submit.pSignalSemaphoreInfos = signals;
    active_cmd_ = VK_NULL_HANDLE;
    active_list_ = gpu::vk::CommandList(active_cmd_);
    const auto submit_started = std::chrono::steady_clock::now();
    cpu_waits_.end_record_ms = wait_ms(submit_started - end_started).count();
    ctx_.CheckDeviceLost(vkQueueSubmit2(ctx_.queue, 1, &submit, frame.in_flight), "frame submit");
    if (presenting_ && gen_acquired_) {
        const vk::Image* generated = fg_sync && !xr ? generated_frame.generate() : nullptr;
        PresentGeneratedFrame(frame, generated, generated ? fg_wait : VK_NULL_HANDLE, fg_wait_value, draw_ui);
    }
    streamline::SetMarker(streamline::Marker::RenderSubmitEnd);
    const auto present_started = std::chrono::steady_clock::now();
    cpu_waits_.submit_ms = wait_ms(present_started - submit_started).count();
    if (presenting_) {
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &ctx_.swapchain.render_finished[image_index_];
        present.swapchainCount = 1;
        present.pSwapchains = &ctx_.swapchain.handle;
        present.pImageIndices = &image_index_;
        streamline::SetMarker(streamline::Marker::PresentStart);
        const VkResult result = ctx_.QueuePresent(present);
        streamline::SetMarker(streamline::Marker::PresentEnd);
        ctx_.CheckDeviceLost(result, "present");
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            swapchain_dirty_ = true;
        }
    }
    cpu_waits_.present_ms = wait_ms(std::chrono::steady_clock::now() - present_started).count();
    frame_index_ = (frame_index_ + 1) % kFramesInFlight;
}

bool Renderer::SplitForExternal(VkSemaphore signal, uint64_t signal_value, VkSemaphore wait, uint64_t wait_value) {    if (!active_cmd_) {
        return false;
    }
    Frame& frame = frames_[frame_index_];
    const VkResult ended = vkEndCommandBuffer(active_cmd_);
    if (ended != VK_SUCCESS) {
        LogError("renderer: the upscale split could not end the frame's command buffer ({})", vk::ResultName(ended));
        return false;
    }
    VkSemaphoreSubmitInfo signal_info{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal_info.semaphore = signal;
    signal_info.value = signal_value;
    signal_info.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    VkCommandBufferSubmitInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmd_info.commandBuffer = active_cmd_;
    submit.pCommandBufferInfos = &cmd_info;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal_info;
    ctx_.CheckDeviceLost(vkQueueSubmit2(ctx_.queue, 1, &submit, VK_NULL_HANDLE), "upscale split submit");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    const VkResult begun = vkBeginCommandBuffer(frame.resume, &begin);
    if (begun != VK_SUCCESS) {
        LogError("renderer: the upscale split could not begin the resume command buffer ({})", vk::ResultName(begun));
        active_cmd_ = VK_NULL_HANDLE;
        active_list_ = gpu::vk::CommandList(active_cmd_);
        return false;
    }
    active_cmd_ = frame.resume;
    active_list_ = gpu::vk::CommandList(active_cmd_);
    split_wait_ = wait;
    split_wait_value_ = wait_value;
    return true;
}

void Renderer::PresentGeneratedFrame(Frame& frame, const vk::Image* source, VkSemaphore fg_wait, uint64_t fg_wait_value, bool draw_ui) {
    if (gen_index_ >= ctx_.swapchain.images.size() || !gen_set_[frame_index_]) {
        gen_acquired_ = false;
        return;
    }
    if (source && fg_wait) {
        VkSemaphoreWaitInfo wait_info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wait_info.semaphoreCount = 1;
        wait_info.pSemaphores = &fg_wait;
        wait_info.pValues = &fg_wait_value;
        const VkResult waited = vkWaitSemaphores(ctx_.device, &wait_info, UINT64_MAX);
        ctx_.CheckDeviceLost(waited, "MetalFX interpolation wait");
        if (waited != VK_SUCCESS) {
            source = nullptr;
            fg_wait = VK_NULL_HANDLE;
        }
    }
    VkImage target = ctx_.swapchain.images[gen_index_];
    VkImageView view = ctx_.swapchain.views[gen_index_];
    VkCommandBuffer cmd = frame.gen_cmd;
    gpu::vk::CommandList list(cmd);
    const gpu::ImageViewHandle from = source ? gpu::vk::Wrap<gpu::ImageViewTag>(source->view) : final_.view;
    const gpu::ImageViewHandle noise = grain_noise_ ? grain_noise_ : scene_color_.view;
    const gpu::ImageBinding infos[2] = {Bind(from, linear_sampler_), Bind(noise, wrap_sampler_)};
    gpu::BindingWrite writes[2];
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i] = {gen_set_[frame_index_], i, 0, gpu::BindingType::CombinedImageSampler, {&infos[i], 1}};
    }
    gpu_->WriteBindings(writes);

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) {
        gen_acquired_ = false;
        return;
    }
    const gpu::Extent2D extent = RenderExtent();
    const gpu::Extent2D window_extent = gpu::vk::FromNative(ctx_.swapchain.extent);
    const bool fitted = window_extent.width != extent.width || window_extent.height != extent.height;
    vk::ImageBarrier(cmd, target, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (source) {
        vk::ImageBarrier(cmd, source->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         VK_ACCESS_2_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = fitted ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, gpu::vk::Native(fitted ? window_extent : extent)};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &rendering);
    if (fitted) {
        const float scale = std::min(static_cast<float>(window_extent.width) / static_cast<float>(extent.width),
                                     static_cast<float>(window_extent.height) / static_cast<float>(extent.height));
        const gpu::Extent2D shown{std::max(1u, static_cast<uint32_t>(extent.width * scale)), std::max(1u, static_cast<uint32_t>(extent.height * scale))};
        Composite(list, gen_set_[frame_index_], 1.0f, shown, gpu::vk::FromNative(ctx_.swapchain.format),
                  {static_cast<int32_t>((window_extent.width - shown.width) / 2), static_cast<int32_t>((window_extent.height - shown.height) / 2)});
    } else {
        Composite(list, gen_set_[frame_index_], 1.0f, extent, gpu::vk::FromNative(ctx_.swapchain.format));
    }
    if (overlay) {
        overlay(list, gpu::vk::Wrap<gpu::ImageViewTag>(view), fitted ? window_extent : extent, OverlayPart::All);
    }
    if (draw_ui && imgui_ready_) {
        ImDrawData* data = ImGui::GetDrawData();
        if (data && data->DisplaySize.x > 0.0f && data->DisplaySize.y > 0.0f) {
            const ImVec2 saved_scale = data->FramebufferScale;
            data->FramebufferScale = ImVec2(static_cast<float>(window_extent.width) / data->DisplaySize.x,
                                            static_cast<float>(window_extent.height) / data->DisplaySize.y);
            ImGui_ImplVulkan_RenderDrawData(data, cmd);
            data->FramebufferScale = saved_scale;
        }
    }
    vkCmdEndRendering(cmd);
    vk::ImageBarrier(cmd, target, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0, ctx_.PresentLayout());
    const VkResult ended = vkEndCommandBuffer(cmd);
    if (ended != VK_SUCCESS) {
        LogError("renderer: the generated frame's composite failed ({})", vk::ResultName(ended));
        gen_acquired_ = false;
        return;
    }
    VkSemaphoreSubmitInfo waits[1]{};
    waits[0] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    waits[0].semaphore = frame.gen_available;
    waits[0].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = ctx_.swapchain.render_finished[gen_index_];
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmd_info.commandBuffer = cmd;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = waits;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;
    ctx_.CheckDeviceLost(vkQueueSubmit2(ctx_.queue, 1, &submit, frame.gen_in_flight), "generated frame submit");
    frame.gen_pending = true;
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &ctx_.swapchain.render_finished[gen_index_];
    present.swapchainCount = 1;
    present.pSwapchains = &ctx_.swapchain.handle;
    present.pImageIndices = &gen_index_;
    const VkResult result = ctx_.QueuePresent(present);
    ctx_.CheckDeviceLost(result, "generated frame present");
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        swapchain_dirty_ = true;
    }
}

namespace {

bool WriteScreenshot(const std::filesystem::path& path, const uint8_t* pixels, VkFormat format, uint32_t width, uint32_t height, float paper,
                     bool hdr_copy) {
    const uint64_t bytes_per_pixel = format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
    std::vector<uint8_t> cropped(size_t(width) * height * 4);
    const bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
    const bool hdr_capture = format == VK_FORMAT_R16G16B16A16_SFLOAT || format == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    const bool pfm = hdr_capture && std::getenv("PT_HDR_DUMP");
    std::vector<float> nits_dump;
    if (hdr_capture && (hdr_copy || pfm)) nits_dump.resize(size_t(width) * height * 3);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t src = (size_t(y) * width + x) * bytes_per_pixel;
            uint8_t* dst = cropped.data() + (size_t(y) * width + x) * 4;
            float* nits = nits_dump.empty() ? nullptr : nits_dump.data() + (size_t(y) * width + x) * 3;
            if (format == VK_FORMAT_R16G16B16A16_SFLOAT) {
                const uint16_t* half = reinterpret_cast<const uint16_t*>(pixels + src);
                for (int c = 0; c < 3; ++c) {
                    const float value = HalfToFloat(half[c]);
                    dst[c] = render::LinearToSrgb8(value * (80.0f / paper));
                    if (nits) nits[c] = value * 80.0f;
                }
            } else if (format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
                uint32_t packed;
                std::memcpy(&packed, pixels + src, sizeof(packed));
                float r = render::PqDecodeNits(float(packed & 0x3ffu) / 1023.0f);
                float g = render::PqDecodeNits(float((packed >> 10) & 0x3ffu) / 1023.0f);
                float b = render::PqDecodeNits(float((packed >> 20) & 0x3ffu) / 1023.0f);
                const float r709 = 1.660491f * r - 0.587641f * g - 0.072850f * b;
                const float g709 = -0.124550f * r + 1.132900f * g - 0.008349f * b;
                const float b709 = -0.018151f * r - 0.100579f * g + 1.118730f * b;
                dst[0] = render::LinearToSrgb8(r709 / paper);
                dst[1] = render::LinearToSrgb8(g709 / paper);
                dst[2] = render::LinearToSrgb8(b709 / paper);
                if (nits) {
                    nits[0] = r709;
                    nits[1] = g709;
                    nits[2] = b709;
                }
            } else {
                const uint8_t* p = pixels + src;
                dst[0] = p[bgra ? 2 : 0];
                dst[1] = p[1];
                dst[2] = p[bgra ? 0 : 2];
            }
            dst[3] = 255;
        }
    }
    const int ok = stbi_write_png(pt::os::PathToUtf8(path).c_str(), static_cast<int>(width), static_cast<int>(height), 4, cropped.data(), static_cast<int>(width * 4));
    if (hdr_copy && !nits_dump.empty() && ok) {
        std::filesystem::path hdr_path = path;
        hdr_path.replace_filename(hdr_path.stem().native() + std::filesystem::path("_HDR").native() + hdr_path.extension().native());
        const render::HdrPngStats levels = render::MeasureHdrLevels(nits_dump);
        if (render::WriteHdrPng(hdr_path, width, height, nits_dump)) {
            LogInfo("screenshot: HDR copy {} (brightest {:.0f} nits, mean {:.0f})", pt::os::PathToUtf8(hdr_path), levels.max_cll, levels.max_fall);
        } else {
            LogWarn("screenshot: the HDR copy {} could not be written", pt::os::PathToUtf8(hdr_path));
        }
    }
    if (pfm) {
        std::ofstream out(std::filesystem::path(path).concat(".pfm"), std::ios::binary);
        out << "PF\n" << width << " " << height << "\n-1.0\n";
        for (uint32_t y = height; y-- > 0;) {
            out.write(reinterpret_cast<const char*>(nits_dump.data() + size_t(y) * width * 3), std::streamsize(width) * 3 * sizeof(float));
        }
    }
    if (ok) {
        LogInfo("screenshot written to {}", pt::os::PathToUtf8(path));
    } else {
        LogError("screenshot {} write failed", pt::os::PathToUtf8(path));
    }
    return ok != 0;
}

}

bool Renderer::SaveScreenshot(const std::filesystem::path& path, glm::vec4 crop, bool background, bool hdr_copy) {
    if (!output_.image || !output_ready_) {
        LogError("screenshot {} not written: {}", pt::os::PathToUtf8(path), output_.image ? "no frame rendered yet" : "no capture image");
        return false;
    }
    gpu_->WaitIdle();
    const uint32_t source_width = output_.extent.width;
    const uint32_t source_height = output_.extent.height;
    render::PixelRect crop_rect;
    if (!render::MakePixelCrop(source_width, source_height, crop, crop_rect)) {
        LogError("screenshot {} not written: crop is empty", pt::os::PathToUtf8(path));
        return false;
    }
    const uint32_t x0 = crop_rect.x;
    const uint32_t y0 = crop_rect.y;
    const uint32_t width = crop_rect.width;
    const uint32_t height = crop_rect.height;
    const VkFormat format = gpu::vk::Native(output_.format);
    const uint64_t bytes_per_pixel = format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
    gpu::BufferResource readback;
    if (!gpu_->CreateBuffer({uint64_t(width) * height * bytes_per_pixel, gpu::BufferUsage::TransferDst, true}, readback)) {
        return false;
    }
    gpu_->Submit([&](gpu::CommandList& cmd) {
        gpu::BufferImageCopy region;
        region.image = {gpu::Aspect::Color, 0, 0, 1};
        region.image_offset[0] = static_cast<int32_t>(x0);
        region.image_offset[1] = static_cast<int32_t>(y0);
        region.extent = {width, height, 1};
        cmd.CopyImageToBuffer(output_, gpu::Layout::TransferSrc, readback, {&region, 1});
    });
    gpu_->InvalidateMapped(readback, 0, ~0ull);
    if (!readback.mapped) {
        gpu_->DestroyBuffer(readback);
        LogError("screenshot {} not written: readback buffer is not mapped", pt::os::PathToUtf8(path));
        return false;
    }
    const float paper = HdrUiWhite();
    const uint8_t* mapped = static_cast<const uint8_t*>(readback.mapped);
    if (!background) {
        const bool ok = WriteScreenshot(path, mapped, format, width, height, paper, hdr_copy);
        gpu_->DestroyBuffer(readback);
        return ok;
    }
    auto pixels = std::make_shared<std::vector<uint8_t>>(mapped, mapped + size_t(width) * height * bytes_per_pixel);
    gpu_->DestroyBuffer(readback);
    std::erase_if(screenshot_writes_, [](std::future<bool>& f) { return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; });
    if (screenshot_writes_.size() >= 24) {
        screenshot_writes_.front().wait();
        screenshot_writes_.erase(screenshot_writes_.begin());
    }
    screenshot_writes_.push_back(std::async(std::launch::async, [path, pixels, format, width, height, paper, hdr_copy] {
        return WriteScreenshot(path, pixels->data(), format, width, height, paper, hdr_copy);
    }));
    return true;
}

void Renderer::FinishScreenshots() {
    for (std::future<bool>& f : screenshot_writes_) f.wait();
    screenshot_writes_.clear();
}

/* XR_KHR_vulkan_enable2 hands the swapchain images over in colour attachment layout and wants them back the same way, hence no transitions here. */
void Renderer::RecordXr(VkCommandBuffer cmd) {
    if (!xr_layout_) {
        VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64};
        VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        const VkDescriptorSetLayout composite_set_layout = gpu::vk::Native(composite_set_layout_);
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &composite_set_layout;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push;
        if (!vk::Check(vkCreatePipelineLayout(ctx_.device, &layout_info, nullptr, &xr_layout_), "xr copy layout")) {
            return;
        }
    }
    if (xr_frame_.hud) {
        if (!hud_.image) {
            if (!ctx_.CreateImage(hud_, gpu::vk::Native(final_format_), {kHudExtent.width, kHudExtent.height, 1},
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
                return;
            }
            VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3};
            VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pool_info.maxSets = 1;
            pool_info.poolSizeCount = 1;
            pool_info.pPoolSizes = &pool_size;
            vkCreateDescriptorPool(ctx_.device, &pool_info, nullptr, &xr_pool_);
            VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            alloc.descriptorPool = xr_pool_;
            alloc.descriptorSetCount = 1;
            const VkDescriptorSetLayout composite_set_layout = gpu::vk::Native(composite_set_layout_);
            alloc.pSetLayouts = &composite_set_layout;
            vkAllocateDescriptorSets(ctx_.device, &alloc, &hud_set_);
            const VkSampler linear_sampler = gpu::vk::Native(linear_sampler_);
            VkDescriptorImageInfo infos[2] = {{linear_sampler, hud_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                              {linear_sampler, hud_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
            VkWriteDescriptorSet writes[2]{};
            for (uint32_t i = 0; i < 2; ++i) {
                writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                writes[i].dstSet = hud_set_;
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[i].pImageInfo = &infos[i];
            }
            vkUpdateDescriptorSets(ctx_.device, 2, writes, 0, nullptr);
        }
        vk::ImageBarrier(cmd, hud_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = hud_.view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, kHudExtent};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        vkCmdBeginRendering(cmd, &rendering);
        if (overlay) {
            gpu::vk::CommandList list(cmd);
            overlay(list, gpu::vk::Wrap<gpu::ImageViewTag>(hud_.view), gpu::vk::FromNative(kHudExtent), OverlayPart::All);
        }
        vkCmdEndRendering(cmd);
        vk::ImageBarrier(cmd, hud_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        CopyToXr(cmd, hud_set_, *xr_frame_.hud, true);
    }
    if (xr_frame_.eye) {
        CopyToXr(cmd, gpu::vk::Native(final_set_), *xr_frame_.eye, false);
    }
}

void Renderer::CopyToXr(VkCommandBuffer cmd, VkDescriptorSet set, const XrTarget& target, bool premultiplied) {
    const VkPipeline pipeline = XrPipeline(target.format);
    if (!pipeline || !target.image) {
        return;
    }
    vk::ImageBarrier(cmd, target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 1, 1);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = target.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, target.extent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &rendering);
    VkViewport viewport{0.0f, 0.0f, static_cast<float>(target.extent.width), static_cast<float>(target.extent.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, target.extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, xr_layout_, 0, 1, &set, 0, nullptr);
    const float brightness = plain_output ? 1.0f : std::clamp(brightness_override_ > 0.0f ? brightness_override_ : output_brightness, 0.1f, 4.0f);
    const float across = target.rect.w > 0.0f && target.extent.height
                             ? (static_cast<float>(target.extent.width) / static_cast<float>(target.extent.height)) / (16.0f / 9.0f)
                             : 1.0f;
    const float push[16] = {target.rect.x, target.rect.y, target.rect.z, target.rect.w,
                            premultiplied ? 1.0f : 0.0f, premultiplied ? 1.0f : brightness, 0.0f, 0.0f,
                            premultiplied || plain_output ? 0.0f : grain[0], grain[1], grain[2], grain[3],
                            grain_offset[0], grain_offset[1], across, 0.0f};
    vkCmdPushConstants(cmd, xr_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

VkPipeline Renderer::XrPipeline(VkFormat format) {
    for (const auto& [f, pipeline] : xr_pipelines_) {
        if (f == format) {
            return pipeline;
        }
    }
    VkShaderModule vert = vk::LoadShaderModule(ctx_.device, "fullscreen.vert");
    VkShaderModule frag = vk::LoadShaderModule(ctx_.device, "xr_copy.frag");
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vert && frag) {
        VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                     {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &blend_attachment;
        VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamic_states;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &format;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.pNext = &rendering;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertex_input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = xr_layout_;
        if (!vk::Check(vk::CreateGraphicsPipelinesCached(ctx_.device, 1, &info, nullptr, &pipeline, "xr copy"), "xr copy pipeline")) {
            pipeline = VK_NULL_HANDLE;
        }
    }
    if (vert) vkDestroyShaderModule(ctx_.device, vert, nullptr);
    if (frag) vkDestroyShaderModule(ctx_.device, frag, nullptr);
    xr_pipelines_.emplace_back(format, pipeline);
    return pipeline;
}

void Renderer::DestroyXr() {
    for (const auto& [format, pipeline] : xr_pipelines_) {
        if (pipeline) vkDestroyPipeline(ctx_.device, pipeline, nullptr);
    }
    xr_pipelines_.clear();
    if (xr_layout_) vkDestroyPipelineLayout(ctx_.device, xr_layout_, nullptr);
    xr_layout_ = VK_NULL_HANDLE;
    if (xr_pool_) vkDestroyDescriptorPool(ctx_.device, xr_pool_, nullptr);
    xr_pool_ = VK_NULL_HANDLE;
    hud_set_ = VK_NULL_HANDLE;
    ctx_.DestroyImage(hud_);
}

bool Renderer::InitD3D12(const RendererSettings& settings) {
#ifdef _WIN32
    if (!settings.create_d3d12) {
        LogError("renderer: DirectX 12 asked for, but this executable cannot make its device");
        return false;
    }
    std::string error;
    std::unique_ptr<gpu::d3d12::D3D12Device> device = settings.create_d3d12(error);
    if (!device) {
        LogError("renderer: no DirectX 12 device: {}", error);
        return false;
    }
    d3d12_ = device.get();
    gpu_ = std::move(device);
    LogInfo("renderer: DirectX 12 on {} ({})", gpu_->Info().adapter, d3d12_->RuntimeDescription());
    uint32_t width = settings.width;
    uint32_t height = settings.height;
    output_format_ = gpu::Format::R8G8B8A8Unorm;
    output_mode_ = RendererOutputMode::Sdr;
    final_format_ = gpu::Format::R16G16B16A16Sfloat;
    if (settings.headless) {
        const char* forced = std::getenv("PT_HDR_HEADLESS");
        if (forced && std::string_view(forced) == "scrgb") {
            output_format_ = gpu::Format::R16G16B16A16Sfloat;
            output_mode_ = RendererOutputMode::ScRgb;
        } else if (forced && std::string_view(forced) == "hdr10") {
            output_format_ = gpu::Format::A2B10G10R10UnormPack32;
            output_mode_ = RendererOutputMode::Hdr10;
        }
    }
    if (window_) {
        int w = 0;
        int h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        width = static_cast<uint32_t>(std::max(w, 1));
        height = static_cast<uint32_t>(std::max(h, 1));
        void* hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(window_), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        output_format_ = ChooseOutputD3D12(hwnd);
        if (!d3d12_->CreateSwapchain(static_cast<HWND__*>(hwnd), width, height, output_format_, settings.vsync, error, HdrPeak())) {
            LogError("renderer: DirectX 12 swapchain: {}", error);
            return false;
        }
        d3d12_window_w_ = w;
        d3d12_window_h_ = h;
    }
    gpu::SamplerDesc sampler_info = LinearClamp();
    linear_sampler_ = gpu_->CreateSampler(sampler_info);
    sampler_info.address_u = sampler_info.address_v = sampler_info.address_w = gpu::AddressMode::Repeat;
    sampler_info.mipmap = gpu::MipmapMode::Linear;
    sampler_info.max_lod = 1000.0f;
    wrap_sampler_ = gpu_->CreateSampler(sampler_info);
    if (!CreateTargets(width, height) || !CreateCompositePipeline(output_format_)) {
        return false;
    }
    if (window_) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::GetIO().IniFilename = "pt_imgui.ini";
        ImGui::StyleColorsDark();
        ImGui_ImplSDL3_InitForD3D(window_);
        if (!d3d12_->InitImGui(final_format_)) {
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();
            return false;
        }
        imgui_ready_ = true;
    }
    return true;
#else
    (void)settings;
    LogError("renderer: DirectX 12 exists on Windows only");
    return false;
#endif
}

gpu::Format Renderer::ChooseOutputD3D12(void* hwnd) {
#ifdef _WIN32
    float peak = 0.0f;
    const bool display_hdr = d3d12_->DisplayHdr(static_cast<HWND__*>(hwnd), peak);
    d3d12_display_peak_ = peak;
    output_mode_ = ChooseD3D12OutputMode(settings_.hdr, display_hdr, fg_wanted_d3d12_ == gpu::d3d12::FrameGenOwner::Dlss);
    if (output_mode_ != RendererOutputMode::Sdr) {
        const bool hdr10 = output_mode_ == RendererOutputMode::Hdr10;
        LogInfo("d3d12: Windows HDR on for this display, it reports {:.0f} nits at most: {} output", peak, hdr10 ? "HDR10" : "scRGB");
        return hdr10 ? gpu::Format::A2B10G10R10UnormPack32 : gpu::Format::R16G16B16A16Sfloat;
    }
    if (settings_.hdr) {
        LogInfo("d3d12: HDR asked for, but Windows HDR is off for this display: SDR output");
    }
#else
    (void)hwnd;
#endif
    output_mode_ = RendererOutputMode::Sdr;
    return gpu::Format::R8G8B8A8Unorm;
}

#ifdef _WIN32
void Renderer::RecordXrD3D12(gpu::CommandList& list) {
    using gpu::Layout;
    if (!xr_layout_d3d12_) {
        gpu::PipelineLayoutDesc layout_info;
        layout_info.sets = {composite_set_layout_};
        layout_info.push_bytes = 64;
        layout_info.push_stages = gpu::ShaderStage::Fragment;
        xr_layout_d3d12_ = gpu_->CreatePipelineLayout(layout_info);
        if (!xr_layout_d3d12_) {
            LogError("renderer: VR copy layout failed");
            return;
        }
    }
    if (xr_frame_.hud) {
        if (!xr_hud_d3d12_.Valid()) {
            gpu::ImageDesc desc;
            desc.format = final_format_;
            desc.extent = {kHudExtent.width, kHudExtent.height, 1};
            desc.usage = gpu::ImageUsage::ColorAttachment | gpu::ImageUsage::Sampled;
            desc.name = "VR HUD";
            if (!gpu_->CreateImage(desc, xr_hud_d3d12_.image)) {
                return;
            }
            gpu::BindingPoolDesc pool_info;
            pool_info.sizes = {{gpu::BindingType::CombinedImageSampler, 3}};
            pool_info.max_sets = 1;
            xr_pool_d3d12_ = gpu_->CreateBindingPool(pool_info);
            xr_hud_set_d3d12_ = xr_pool_d3d12_ ? gpu_->AllocateBindingSet(xr_pool_d3d12_, composite_set_layout_, 0) : gpu::BindingSetHandle{};
            if (!xr_hud_set_d3d12_) {
                LogError("renderer: VR HUD set failed");
                return;
            }
            const gpu::ImageBinding infos[2] = {Bind(xr_hud_d3d12_.image.view, linear_sampler_), Bind(xr_hud_d3d12_.image.view, linear_sampler_)};
            const gpu::BindingWrite writes[2] = {{xr_hud_set_d3d12_, 0, 0, gpu::BindingType::CombinedImageSampler, {&infos[0], 1}},
                                                 {xr_hud_set_d3d12_, 1, 0, gpu::BindingType::CombinedImageSampler, {&infos[1], 1}}};
            gpu_->WriteBindings(writes);
        }
        list.UseTargets({{&xr_hud_d3d12_, Layout::ColorAttachment}});
        const gpu::Attachment hud{xr_hud_d3d12_.image.view, gpu::LoadOp::Clear, {{0.0f, 0.0f, 0.0f, 0.0f}}};
        list.BeginRendering({{0, 0}, gpu::vk::FromNative(kHudExtent)}, {&hud, 1});
        if (overlay) {
            overlay(list, xr_hud_d3d12_.image.view, gpu::vk::FromNative(kHudExtent), OverlayPart::All);
        }
        list.EndPass();
        list.UseTargets({{&xr_hud_d3d12_, Layout::ShaderReadOnly}});
        CopyToXrD3D12(list, xr_hud_set_d3d12_, *xr_frame_.hud, true);
    }
    if (xr_frame_.eye) {
        CopyToXrD3D12(list, final_set_, *xr_frame_.eye, false);
    }
}

void Renderer::CopyToXrD3D12(gpu::CommandList& list, gpu::BindingSetHandle set, const XrTarget& target, bool premultiplied) {
    using gpu::Layout;
    if (!target.d3d12_image || !set) {
        return;
    }
    const gpu::Format format = gpu::vk::FromNative(target.format);
    gpu::RenderTarget* wrapped = nullptr;
    for (auto& [resource, rt] : xr_wrapped_) {
        if (resource == target.d3d12_image) {
            wrapped = &rt;
        }
    }
    if (!wrapped) {
        gpu::RenderTarget rt;
        if (!d3d12_->WrapRenderTarget(target.d3d12_image, format, gpu::vk::FromNative(target.extent), rt.image)) {
            return;
        }
        rt.layout = Layout::ColorAttachment;
        xr_wrapped_.emplace_back(target.d3d12_image, rt);
        wrapped = &xr_wrapped_.back().second;
    }
    gpu::PipelineHandle pipeline;
    for (const auto& [f, p] : xr_pipelines_d3d12_) {
        if (f == format) {
            pipeline = p;
        }
    }
    if (!pipeline) {
        gpu::GraphicsPipelineDesc desc;
        desc.vertex = "fullscreen.vert";
        desc.fragment = "xr_copy.frag";
        desc.layout = xr_layout_d3d12_;
        desc.colors = {format};
        desc.fixed_state = true;
        desc.depth_state = false;
        desc.cull = gpu::CullMode::None;
        gpu::BlendState blend;
        blend.src_color = blend.dst_color = blend.src_alpha = blend.dst_alpha = gpu::BlendFactor::Zero;
        desc.blend_states = {blend};
        pipeline = gpu_->CreateGraphicsPipeline(desc);
        xr_pipelines_d3d12_.emplace_back(format, pipeline);
        if (!pipeline) {
            LogError("renderer: VR copy pipeline for format {} failed", static_cast<int>(format));
        }
    }
    if (!pipeline) {
        return;
    }
    const gpu::Extent2D extent = gpu::vk::FromNative(target.extent);
    list.UseTargets({{wrapped, Layout::ColorAttachment}});
    const gpu::Attachment color{wrapped->image.view, gpu::LoadOp::DontCare, {}};
    list.BeginRendering({{0, 0}, extent}, {&color, 1});
    list.SetViewport({{0, 0}, extent});
    list.BindPipeline(gpu::BindPoint::Graphics, pipeline);
    list.BindSets(gpu::BindPoint::Graphics, xr_layout_d3d12_, 0, {&set, 1});
    const float brightness = plain_output ? 1.0f : std::clamp(brightness_override_ > 0.0f ? brightness_override_ : output_brightness, 0.1f, 4.0f);
    const float across = target.rect.w > 0.0f && target.extent.height
                             ? (static_cast<float>(target.extent.width) / static_cast<float>(target.extent.height)) / (16.0f / 9.0f)
                             : 1.0f;
    const float push[16] = {target.rect.x, target.rect.y, target.rect.z, target.rect.w,
                            premultiplied ? 1.0f : 0.0f, premultiplied ? 1.0f : brightness, 0.0f, 0.0f,
                            premultiplied || plain_output ? 0.0f : grain[0], grain[1], grain[2], grain[3],
                            grain_offset[0], grain_offset[1], across, 0.0f};
    list.PushConstants(xr_layout_d3d12_, gpu::ShaderStage::Fragment, 0, sizeof(push), push);
    list.Draw(3, 1, 0, 0);
    list.EndPass();
}

void Renderer::DestroyXrD3D12() {
    for (auto& [format, pipeline] : xr_pipelines_d3d12_) {
        if (pipeline) gpu_->DestroyPipeline(pipeline);
    }
    xr_pipelines_d3d12_.clear();
    if (xr_layout_d3d12_) gpu_->DestroyPipelineLayout(xr_layout_d3d12_);
    xr_layout_d3d12_ = {};
    if (xr_pool_d3d12_) gpu_->DestroyBindingPool(xr_pool_d3d12_);
    xr_pool_d3d12_ = {};
    xr_hud_set_d3d12_ = {};
    gpu_->DestroyImage(xr_hud_d3d12_.image);
    xr_hud_d3d12_ = gpu::RenderTarget{};
    for (auto& [resource, rt] : xr_wrapped_) {
        gpu_->DestroyImage(rt.image);
    }
    xr_wrapped_.clear();
    gpu_->DestroyImage(window_target_.image);
    window_target_ = gpu::RenderTarget{};
}

namespace {
const char* FrameGenOwnerName(gpu::d3d12::FrameGenOwner owner) {
    switch (owner) {
    case gpu::d3d12::FrameGenOwner::Xess: return "XeSS Frame Generation";
    case gpu::d3d12::FrameGenOwner::Fsr: return "FSR 3 frame generation";
    case gpu::d3d12::FrameGenOwner::Dlss: return "DLSS Frame Generation";
    case gpu::d3d12::FrameGenOwner::None: break;
    }
    return "frame generation";
}
}

std::string Renderer::FrameGenReasonD3D12(gpu::d3d12::FrameGenOwner owner) {
    std::string reason;
    if (!d3d12_ || owner == gpu::d3d12::FrameGenOwner::None) {
        return reason;
    }
    if (!window_) {
        return "needs a window";
    }
    if (fg_failed_d3d12_[static_cast<int>(owner)]) {
        return "could not start this session; restart to try again";
    }
    d3d12_->FrameGenAvailable(owner, reason);
    return reason;
}

void Renderer::UpdateFrameGenerationD3D12(gpu::d3d12::FrameGenOwner owner, bool paused) {
    using gpu::d3d12::FrameGenOwner;
    if (!d3d12_ || !window_) {
        return;
    }
    const char* name = FrameGenOwnerName(owner);
    if (owner != FrameGenOwner::None && owner != FrameGenOwner::Dlss && output_mode_ != RendererOutputMode::Sdr) {
        if (!fg_refusal_logged_[static_cast<int>(owner)]) {
            LogWarn("frame generation: {} off while the native HDR output is on", name);
            fg_refusal_logged_[static_cast<int>(owner)] = true;
        }
        owner = FrameGenOwner::None;
    }
    if (owner != FrameGenOwner::None) {
        const std::string reason = FrameGenReasonD3D12(owner);
        if (!reason.empty()) {
            if (!fg_refusal_logged_[static_cast<int>(owner)]) {
                LogInfo("frame generation: {} unavailable on DirectX 12: {}", name, reason);
                fg_refusal_logged_[static_cast<int>(owner)] = true;
            }
            owner = FrameGenOwner::None;
        }
    }
    if (owner != fg_wanted_d3d12_) {
        LogInfo("frame generation: {} on DirectX 12, the swapchain is made again", owner == FrameGenOwner::None ? "off" : name);
        fg_wanted_d3d12_ = owner;
        d3d12_->SetFrameGenOwner(owner);
        swapchain_dirty_ = true;
    }
    d3d12_->SetGenerating(!paused);
    fg_paused_d3d12_ = paused;
}

bool Renderer::FrameGeneratingD3D12() const {
    return d3d12_ && window_ && !fg_paused_d3d12_ && d3d12_->ActiveFrameGenOwner() != gpu::d3d12::FrameGenOwner::None &&
           d3d12_->ActiveFrameGenOwner() == fg_wanted_d3d12_ &&
           D3D12FrameGenSupportsOutput(output_mode_, fg_wanted_d3d12_ == gpu::d3d12::FrameGenOwner::Dlss) && layer_pipeline_;
}

bool Renderer::EnsureFrameGenTargetsD3D12(gpu::Extent2D extent) {
    const gpu::Format layer_format = gpu::vk::FromNative(kUiLayerFormat);
    if (fg_hudless_.Valid() && fg_hudless_.Extent().width == extent.width && fg_hudless_.Extent().height == extent.height &&
        fg_hudless_.image.format == output_format_) {
        return true;
    }
    gpu_->WaitIdle();
    gpu_->DestroyImage(fg_hudless_.image);
    gpu_->DestroyImage(fg_ui_.image);
    fg_hudless_ = gpu::RenderTarget{};
    fg_ui_ = gpu::RenderTarget{};
    gpu::ImageDesc desc;
    desc.extent = {extent.width, extent.height, 1};
    desc.format = output_format_;
    desc.usage = gpu::ImageUsage::ColorAttachment | gpu::ImageUsage::Sampled | gpu::ImageUsage::TransferSrc;
    if (!gpu_->CreateImage(desc, fg_hudless_.image)) {
        LogError("frame generation: no HUD-less target at {}x{}", extent.width, extent.height);
        return false;
    }
    desc.format = layer_format;
    if (!gpu_->CreateImage(desc, fg_ui_.image)) {
        LogError("frame generation: no UI layer target at {}x{}", extent.width, extent.height);
        gpu_->DestroyImage(fg_hudless_.image);
        fg_hudless_ = gpu::RenderTarget{};
        return false;
    }
    return true;
}
#endif

bool Renderer::ResizeSwapchainD3D12() {
#ifdef _WIN32
    int w = 0;
    int h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    if (w <= 0 || h <= 0) {
        return false;
    }
    gpu_->WaitIdle();
    d3d12_->DestroySwapchain();
    std::string error;
    void* hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(window_), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    const gpu::Format previous_format = output_format_;
    const RendererOutputMode previous_mode = output_mode_;
    output_format_ = ChooseOutputD3D12(hwnd);
    if (output_format_ != previous_format) {
        if (composite_pipeline_) gpu_->DestroyPipeline(composite_pipeline_);
        composite_pipeline_ = {};
        if (!BuildCompositePipeline(output_format_, composite_pipeline_)) {
            return false;
        }
    }
    if (output_mode_ != previous_mode) {
        LogInfo("display: output {} while running", output_mode_ == RendererOutputMode::Sdr ? "SDR" : output_mode_ == RendererOutputMode::ScRgb ? "HDR (scRGB)" : "HDR (HDR10)");
    }
    const bool created = d3d12_->CreateSwapchain(static_cast<HWND__*>(hwnd), static_cast<uint32_t>(w), static_cast<uint32_t>(h), output_format_,
                                                settings_.vsync, error, HdrPeak());
    if (fg_wanted_d3d12_ != gpu::d3d12::FrameGenOwner::None && d3d12_->ActiveFrameGenOwner() != fg_wanted_d3d12_) {
        LogWarn("frame generation: {} could not start on DirectX 12; off for this session",
                FrameGenOwnerName(fg_wanted_d3d12_));
        fg_failed_d3d12_[static_cast<int>(fg_wanted_d3d12_)] = true;
        fg_wanted_d3d12_ = gpu::d3d12::FrameGenOwner::None;
        d3d12_->SetFrameGenOwner(fg_wanted_d3d12_);
        if (D3D12FrameGenFallbackNeedsRecreate(output_mode_)) {
            return ResizeSwapchainD3D12();
        }
    }
    if (!created) {
        LogError("renderer: DirectX 12 swapchain: {}", error);
        return false;
    }
    d3d12_window_w_ = w;
    d3d12_window_h_ = h;
    DestroyTargets();
    const gpu::Extent2D size = render_extent_.width > 0 && render_extent_.height > 0 ? gpu::vk::FromNative(render_extent_)
                                                                                      : gpu::Extent2D{static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
    targets_missing_ = !CreateTargets(size.width, size.height);
    if (targets_missing_) {
        DestroyTargets();
        targets_retry_at_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        return false;
    }
    return true;
#else
    return false;
#endif
}

bool Renderer::BeginFrameD3D12(bool present) {
    using wait_ms = std::chrono::duration<float, std::milli>;
    cpu_waits_ = CpuWaits{};
    grain[0] = 0.0f;
    if (window_ && present) {
        int w = 0;
        int h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        if (w > 0 && h > 0 && (w != d3d12_window_w_ || h != d3d12_window_h_)) {
            swapchain_dirty_ = true;
        }
        if (swapchain_dirty_) {
            swapchain_dirty_ = false;
            if (!ResizeSwapchainD3D12()) {
                return false;
            }
        }
    }
    if (targets_missing_) {
        if (std::chrono::steady_clock::now() < targets_retry_at_) {
            return false;
        }
        gpu_->WaitIdle();
        DestroyTargets();
        const gpu::Extent2D size = RenderExtent().width ? RenderExtent() : gpu::Extent2D{settings_.width, settings_.height};
        if (!CreateTargets(size.width, size.height)) {
            DestroyTargets();
            targets_retry_at_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            return false;
        }
        targets_missing_ = false;
    }
    const bool test_remake = TestRemakeTargets();
    if (render_extent_.width > 0 && render_extent_.height > 0 &&
        (test_remake || scene_color_.extent.width != render_extent_.width || scene_color_.extent.height != render_extent_.height ||
         scene_color_.format != SceneColorFormat())) {
        gpu_->WaitIdle();
        const uint64_t old_view = scene_color_.view.bits;
        DestroyTargets();
        if (!CreateTargets(render_extent_.width, render_extent_.height)) {
            DestroyTargets();
            targets_missing_ = true;
            targets_retry_at_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            return false;
        }
        LogInfo("renderer: render size {}x{}", render_extent_.width, render_extent_.height);
        if (test_remake) {
            TestRemadeTargets(old_view, scene_color_.view.bits);
        }
    }
    if (scene_color_.Valid() && scene_color_.format != SceneColorFormat()) {
        gpu_->WaitIdle();
        const gpu::Extent3D size = scene_color_.extent;
        DestroyTargets();
        if (!CreateTargets(size.width, size.height)) {
            DestroyTargets();
            targets_missing_ = true;
            targets_retry_at_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            return false;
        }
    }
    presenting_ = window_ && present;
    const auto fence_started = std::chrono::steady_clock::now();
    gpu::CommandList* list = gpu_->BeginFrame(frame_index_);
    cpu_waits_.fence_ms = wait_ms(std::chrono::steady_clock::now() - fence_started).count();
    if (!list) {
        return false;
    }
    list_ = list;
    gpu_->ReapDeferred(false);
    using gpu::Layout;
    using gpu::PipelineStage;
    gpu::CommandList& cmd = *list_;
    cmd.Barrier(ColorBarrier(scene_color_.image, PipelineStage::AllCommands, 0, Layout::Undefined, PipelineStage::ColorAttachmentOutput,
                             gpu::Bits(gpu::Access::ColorAttachmentWrite), Layout::ColorAttachment));
    gpu::Attachment color{scene_color_.view, gpu::LoadOp::Clear, {{0.0f, 0.0f, 0.0f, 1.0f}}};
    const gpu::Rect2D area{{0, 0}, RenderExtent()};
    cmd.BeginRendering(area, {&color, 1});
    cmd.EndPass();
    return true;
}

void Renderer::EndFrameD3D12(bool draw_ui) {
    using gpu::Access;
    using gpu::Layout;
    using gpu::PipelineStage;
    using wait_ms = std::chrono::duration<float, std::milli>;
    const auto end_started = std::chrono::steady_clock::now();
    gpu::CommandList& list = *list_;
    list.Barrier(ColorBarrier(scene_color_.image, PipelineStage::ColorAttachmentOutput | PipelineStage::ComputeShader,
                              Access::ColorAttachmentWrite | Access::ShaderStorageWrite, Layout::ColorAttachment, PipelineStage::FragmentShader,
                              gpu::Bits(Access::ShaderSampledRead), Layout::ShaderReadOnly));
    const gpu::Extent2D extent = RenderExtent();
    const bool xr = std::exchange(xr_pending_, false);
    static const bool ui_over_grain = [] {
        const char* value = std::getenv("PT_UI_OVER_GRAIN");
        return !(value && value[0] == '0');
    }();
    const bool split_ui = overlay && !xr && ui_over_grain;
#ifdef _WIN32
    const bool generate = !xr && presenting_ && std::exchange(fg_inputs_ready_, false) && FrameGeneratingD3D12() &&
                          extent.width == static_cast<uint32_t>(d3d12_window_w_) && extent.height == static_cast<uint32_t>(d3d12_window_h_) &&
                          EnsureFrameGenTargetsD3D12(extent);
#else
    const bool generate = false;
#endif
    RecordTopUi(list, extent, false, false);
    list.Barrier(ColorBarrier(final_.image, PipelineStage::FragmentShader, 0, Layout::Undefined, PipelineStage::ColorAttachmentOutput,
                              gpu::Bits(Access::ColorAttachmentWrite), Layout::ColorAttachment));
    gpu::Attachment color{final_.view, gpu::LoadOp::DontCare, {}};
    list.BeginRendering({{0, 0}, extent}, {&color, 1});
    Composite(list, composite_set_, 0.0f, extent, final_format_);
    if (overlay && !generate && (!xr || xr_frame_.overlay_on_frame)) {
        overlay(list, final_.view, extent, split_ui ? OverlayPart::Below : OverlayPart::All);
    }
#ifdef _WIN32
    if (draw_ui && imgui_ready_ && presenting_) {
        ImDrawData* data = ImGui::GetDrawData();
        if (data && data->DisplaySize.x > 0.0f && data->DisplaySize.y > 0.0f) {
            const ImVec2 saved_scale = data->FramebufferScale;
            data->FramebufferScale = ImVec2(static_cast<float>(extent.width) / data->DisplaySize.x,
                                            static_cast<float>(extent.height) / data->DisplaySize.y);
            d3d12_->RenderImGui(list, data);
            data->FramebufferScale = saved_scale;
        }
    }
#endif
    list.EndPass();
    list.Barrier(ColorBarrier(final_.image, PipelineStage::ColorAttachmentOutput, gpu::Bits(Access::ColorAttachmentWrite), Layout::ColorAttachment,
                              PipelineStage::FragmentShader, gpu::Bits(Access::ShaderSampledRead), Layout::ShaderReadOnly));
#ifdef _WIN32
    if (generate) {
        list.UseTargets({{&fg_hudless_, Layout::ColorAttachment}});
        gpu::Attachment hud_color{fg_hudless_.image.view, gpu::LoadOp::DontCare, {}};
        list.BeginRendering({{0, 0}, extent}, {&hud_color, 1});
        Composite(list, final_set_, 1.0f, extent, output_format_, {0, 0}, false, true);
        list.EndPass();
        list.UseTargets({{&fg_hudless_, Layout::ShaderReadOnly}});
        if (overlay) {
            list.Barrier(ColorBarrier(final_.image, PipelineStage::FragmentShader, gpu::Bits(Access::ShaderSampledRead), Layout::ShaderReadOnly,
                                      PipelineStage::ColorAttachmentOutput, Access::ColorAttachmentRead | Access::ColorAttachmentWrite,
                                      Layout::ColorAttachment));
            color.load = gpu::LoadOp::Load;
            list.BeginRendering({{0, 0}, extent}, {&color, 1});
            overlay(list, final_.view, extent, split_ui ? OverlayPart::Below : OverlayPart::All);
            list.EndPass();
            color.load = gpu::LoadOp::DontCare;
            list.Barrier(ColorBarrier(final_.image, PipelineStage::ColorAttachmentOutput, gpu::Bits(Access::ColorAttachmentWrite),
                                      Layout::ColorAttachment, PipelineStage::FragmentShader, gpu::Bits(Access::ShaderSampledRead),
                                      Layout::ShaderReadOnly));
        }
    }
#endif
    RecordTopUi(list, extent, split_ui && overlay_top_pending && overlay_top_pending(), true);
#ifdef _WIN32
    if (generate) {
        const gpu::BindingSetHandle set = LayerSet(fg_hudless_.image.view);
        list.UseTargets({{&fg_ui_, Layout::ColorAttachment}});
        const gpu::Attachment layer_color{fg_ui_.image.view, set ? gpu::LoadOp::DontCare : gpu::LoadOp::Clear, {{0.0f, 0.0f, 0.0f, 0.0f}}};
        list.BeginRendering({{0, 0}, extent}, {&layer_color, 1});
        if (set) {
            list.SetViewport({{0, 0}, extent});
            list.BindPipeline(gpu::BindPoint::Graphics, layer_pipeline_);
            list.BindSets(gpu::BindPoint::Graphics, layer_layout_, 0, {&set, 1});
            float push[32];
            CompositePush(push, 1.0f, extent, true, false);
            list.PushConstants(layer_layout_, gpu::ShaderStage::Fragment, 0, sizeof(push), push);
            list.Draw(3, 1, 0, 0);
        }
        list.EndPass();
        list.UseTargets({{&fg_ui_, Layout::ShaderReadOnly}});
        fg_inputs_d3d12_.hudless = &fg_hudless_;
        fg_inputs_d3d12_.ui = &fg_ui_;
        d3d12_->SetFrameGenInputs(fg_inputs_d3d12_);
    }
#endif
#ifdef _WIN32
    if (xr) {
        RecordXrD3D12(list);
    }
#endif
    present_target_.image = output_;
    list.UseTargets({{&present_target_, Layout::ColorAttachment}});
    color.view = output_.view;
    list.BeginRendering({{0, 0}, extent}, {&color, 1});
    Composite(list, final_set_, 1.0f, extent, output_.format, {0, 0}, true);
    list.EndPass();
    list.UseTargets({{&present_target_, Layout::TransferSrc}});
    output_ready_ = true;
    gpu::RenderTarget* shown = &present_target_;
#ifdef _WIN32
    const gpu::Extent2D window_extent{static_cast<uint32_t>(std::max(d3d12_window_w_, 1)), static_cast<uint32_t>(std::max(d3d12_window_h_, 1))};
    if (presenting_ && (window_extent.width != extent.width || window_extent.height != extent.height)) {
        if (!window_target_.Valid() || window_target_.Extent().width != window_extent.width || window_target_.Extent().height != window_extent.height ||
            window_target_.image.format != output_format_) {
            gpu_->WaitIdle();
            gpu_->DestroyImage(window_target_.image);
            window_target_ = gpu::RenderTarget{};
            gpu::ImageDesc desc;
            desc.format = output_format_;
            desc.extent = {window_extent.width, window_extent.height, 1};
            desc.usage = gpu::ImageUsage::ColorAttachment | gpu::ImageUsage::TransferSrc;
            desc.name = "window target";
            if (!gpu_->CreateImage(desc, window_target_.image)) {
                LogError("renderer: no {}x{} window target", window_extent.width, window_extent.height);
            }
        }
        if (window_target_.Valid()) {
            list.UseTargets({{&window_target_, Layout::ColorAttachment}});
            gpu::Attachment window_color{window_target_.image.view, gpu::LoadOp::Clear, {{0.0f, 0.0f, 0.0f, 1.0f}}};
            list.BeginRendering({{0, 0}, window_extent}, {&window_color, 1});
            const float scale = std::min(static_cast<float>(window_extent.width) / static_cast<float>(extent.width),
                                         static_cast<float>(window_extent.height) / static_cast<float>(extent.height));
            const gpu::Extent2D fitted{std::max(1u, static_cast<uint32_t>(extent.width * scale)), std::max(1u, static_cast<uint32_t>(extent.height * scale))};
            Composite(list, final_set_, 1.0f, fitted, output_format_,
                      {static_cast<int32_t>((window_extent.width - fitted.width) / 2), static_cast<int32_t>((window_extent.height - fitted.height) / 2)},
                      true);
            list.EndPass();
            list.UseTargets({{&window_target_, Layout::TransferSrc}});
            shown = &window_target_;
        }
    }
#endif
    list_ = &active_list_;
    const auto submit_started = std::chrono::steady_clock::now();
    cpu_waits_.end_record_ms = wait_ms(submit_started - end_started).count();
    gpu_->EndFrame(frame_index_, presenting_ ? shown : nullptr);
    cpu_waits_.present_ms = wait_ms(std::chrono::steady_clock::now() - submit_started).count();
    frame_index_ = (frame_index_ + 1) % kFramesInFlight;
}

}
