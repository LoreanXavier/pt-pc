#pragma once

#include <algorithm>
#include <chrono>
#include <glm/glm.hpp>

#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <utility>
#include <vector>

#include "engine/render/hdr_output.h"
#include "engine/render/output_mode.h"
#include "engine/render/pipeline_warmup.h"
#include "engine/render/vk/vk_device.h"
#ifdef _WIN32
#include "engine/render/d3d12/backend/d3d12_backend.h"
#endif
#include "engine/render/vk_context.h"

struct SDL_Window;

namespace pt {

enum class FrameStartAction;

struct RendererSettings {
    gpu::Backend backend = gpu::Backend::Vulkan;
#ifdef _WIN32
    std::function<std::unique_ptr<gpu::d3d12::D3D12Device>(std::string& error)> create_d3d12;
#endif
    bool validation = false;
    bool vsync = true;
    bool headless = false;
    bool hdr = false;
    int hdr_paper_white = 200;
    int hdr_ui_white = 0;
    int hdr_reach = 100;
    int hdr_peak = 0;
    render::HdrLook hdr_look;
    std::filesystem::path pipeline_cache_dir;
    uint32_t width = 1600;
    uint32_t height = 900;
};

struct XrTarget {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    glm::vec4 rect{0.0f, 0.0f, 1.0f, 1.0f};
    void* d3d12_image = nullptr;
};

struct XrFrame {
    const XrTarget* eye = nullptr;
    const XrTarget* hud = nullptr;
    bool overlay_on_frame = false;
};

struct GeneratedFrameHooks {
    std::function<const vk::Image*()> generate;
    std::function<bool(VkSemaphore&, uint64_t&, VkSemaphore&, uint64_t&)> sync;
};

class Renderer {
public:
    bool Init(SDL_Window* window, const RendererSettings& settings);
    void Shutdown();
    void AddPipelineWarmup(vk::PipelineWarmup& warmup);

    bool BeginFrame(bool present = true);
    void EndFrame(bool draw_ui);
    bool SplitForExternal(VkSemaphore signal, uint64_t signal_value, VkSemaphore wait, uint64_t wait_value);
    void Resize(uint32_t width, uint32_t height);
    void SetVsync(bool enabled);
    void SetHdr(bool enabled);
    void SetHdrLevels(int paper_white, int peak, int ui_white = 0);
#ifdef _WIN32
    void UpdateFrameGenerationD3D12(gpu::d3d12::FrameGenOwner owner, bool paused);
    void SetFrameGenInputsD3D12(const gpu::d3d12::FrameGenInputs& inputs) {
        fg_inputs_d3d12_ = inputs;
        fg_inputs_ready_ = true;
    }
    bool FrameGeneratingD3D12() const;
    std::string FrameGenReasonD3D12(gpu::d3d12::FrameGenOwner owner);
#endif
    float HdrPaperWhite() const { return ctx_.hdr_paper_nits; }
    void SetHdrPattern(int kind) { hdr_pattern_ = kind; }
    void SetHdrLook(const render::HdrLook& look) { hdr_look_ = look; }
    const render::HdrLook& HdrLookSettings() const { return hdr_look_; }
    void SetHdrReach(int percent) { hdr_reach_ = std::clamp(percent, 100, 300) / 100.0f; }
    float HdrReach() const { return hdr_reach_; }
    float HdrUiWhite() const { return hdr_ui_nits_ > 0.0f ? hdr_ui_nits_ : ctx_.hdr_paper_nits; }
    float HdrPeak() const {
        return d3d12_ ? render::ResolveHdrPeak(static_cast<float>(ctx_.hdr_peak_setting), d3d12_display_peak_) : ctx_.HdrPeakNits();
    }
    float DisplayPeak() const { return d3d12_ ? d3d12_display_peak_ : ctx_.display_peak_nits; }
    void RecheckOutput();
    void ImGuiNewFrame();
    bool SaveScreenshot(const std::filesystem::path& path, glm::vec4 crop = {0.0f, 0.0f, 1.0f, 1.0f}, bool background = false, bool hdr_copy = true);
    void FinishScreenshots();

    vk::Context& Context() { return ctx_; }
    gpu::Device& Gpu() { return *gpu_; }
    gpu::Backend Backend() const { return d3d12_ ? gpu::Backend::D3D12 : gpu::Backend::Vulkan; }
    VkCommandBuffer Cmd() const { return active_cmd_; }
    gpu::CommandList& List() { return *list_; }
    const gpu::ImageResource& SceneColor() const { return scene_color_; }
    gpu::Extent2D RenderExtent() const { return {scene_color_.extent.width, scene_color_.extent.height}; }
    uint32_t FrameIndex() const { return frame_index_; }
    struct CpuWaits {
        float fence_ms = 0.0f;
        float sleep_ms = 0.0f;
        float acquire_ms = 0.0f;
        float end_record_ms = 0.0f;
        float submit_ms = 0.0f;
        float present_ms = 0.0f;
    };
    const CpuWaits& LastCpuWaits() const { return cpu_waits_; }
    static constexpr uint32_t kFramesInFlight = 2;
    gpu::Format SceneColorFormat() const {
        return output_mode_ == RendererOutputMode::Sdr ? gpu::Format::R8G8B8A8Unorm : gpu::Format::R16G16B16A16Sfloat;
    }
    gpu::Format FinalFormat() const { return final_format_; }
    RendererOutputMode OutputMode() const { return output_mode_; }
    float OutputBrightness() const {
        return plain_output ? 1.0f : std::clamp(brightness_override_ > 0.0f ? brightness_override_ : output_brightness, 0.1f, 4.0f);
    }

    int photo_filter = 0;
    float exposure = 1.0f;
    float output_brightness = 1.0f;
    bool plain_output = false;
    float fade[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float grain[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float grain_offset[2] = {0.0f, 0.0f};
    enum class OverlayPart { All, Below, Top };
    std::function<void(gpu::CommandList&, gpu::ImageViewHandle, gpu::Extent2D, OverlayPart)> overlay;
    std::function<bool()> overlay_top_pending;
    std::function<const vk::Image*(uint32_t)> hudless;
    std::function<const vk::Image*(uint32_t)> ui_layer;
    std::function<bool()> ui_layer_over_rendered;
    static constexpr VkFormat kUiLayerFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    GeneratedFrameHooks generated_frame;
    std::function<FrameStartAction(bool)> frame_start;
    std::function<bool()> swapchain_failed;

    void SetGrainNoise(gpu::ImageViewHandle view);

    void SetRenderExtent(VkExtent2D extent) { render_extent_ = extent; }
    void SetXrFrame(const XrFrame& frame) { xr_frame_ = frame; xr_pending_ = true; }
    static constexpr VkExtent2D kHudExtent{1920, 1080};

private:
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBuffer resume = VK_NULL_HANDLE;
        VkCommandBuffer gen_cmd = VK_NULL_HANDLE;
        VkSemaphore image_available = VK_NULL_HANDLE;
        VkSemaphore gen_available = VK_NULL_HANDLE;
        VkFence in_flight = VK_NULL_HANDLE;
        VkFence gen_in_flight = VK_NULL_HANDLE;
        bool gen_pending = false;
    };

    bool CreateTargets(uint32_t width, uint32_t height);
    void DestroyTargets();
    void PresentGeneratedFrame(Frame& frame, const vk::Image* source, VkSemaphore fg_wait, uint64_t fg_wait_value, bool draw_ui);
    bool CreateCompositePipeline(gpu::Format output_format);
    bool BuildCompositePipeline(gpu::Format format, gpu::PipelineHandle& pipeline, const char* fragment = "composite.frag",
                                gpu::PipelineLayoutHandle layout = {});
    bool CreateLayerPipeline();
    gpu::BindingSetHandle LayerSet(gpu::ImageViewHandle hudless_view);
    void CompositePush(float (&push)[32], float mode, gpu::Extent2D extent, bool with_top_ui, bool grain_off) const;
    void UpdateOutputMode();
    bool InitImGui(SDL_Window* window);
    void Composite(gpu::CommandList& cmd, gpu::BindingSetHandle set, float mode, gpu::Extent2D extent, gpu::Format target_format,
                   gpu::Offset2D offset = {0, 0}, bool with_top_ui = false, bool grain_off = false);
    void RecordTopUi(gpu::CommandList& cmd, gpu::Extent2D extent, bool draw, bool after_overlay);
    bool InitD3D12(const RendererSettings& settings);
    bool BeginFrameD3D12(bool present);
    void EndFrameD3D12(bool draw_ui);
    bool ResizeSwapchainD3D12();
    gpu::Format ChooseOutputD3D12(void* hwnd);
    void WriteCompositeSets();
    void RecordXr(VkCommandBuffer cmd);
    void CopyToXr(VkCommandBuffer cmd, VkDescriptorSet set, const XrTarget& target, bool premultiplied);
    VkPipeline XrPipeline(VkFormat format);
    void DestroyXr();
    void LogLongWait(const char* what, VkResult result, float ms);

    vk::Context ctx_;
    RendererSettings settings_;
    SDL_Window* window_ = nullptr;
    Frame frames_[kFramesInFlight];
    uint32_t frame_index_ = 0;
    uint64_t frames_begun_ = 0;
    uint32_t long_waits_logged_ = 0;
    CpuWaits cpu_waits_;
    uint32_t image_index_ = 0;
    uint32_t gen_index_ = 0;
    bool gen_acquired_ = false;
    gpu::BindingSetHandle gen_set_[kFramesInFlight] = {};
    VkCommandBuffer active_cmd_ = VK_NULL_HANDLE;
    gpu::vk::CommandList active_list_{VK_NULL_HANDLE};
    gpu::CommandList* list_ = &active_list_;
    std::unique_ptr<gpu::Device> gpu_;
#ifdef _WIN32
    gpu::d3d12::D3D12Device* d3d12_ = nullptr;
#else
    void* d3d12_ = nullptr;
#endif
    gpu::RenderTarget present_target_;
    int d3d12_window_w_ = 0;
    int d3d12_window_h_ = 0;
    float d3d12_display_peak_ = 0.0f;
#ifdef _WIN32
    gpu::d3d12::FrameGenOwner fg_wanted_d3d12_ = gpu::d3d12::FrameGenOwner::None;
    bool fg_failed_d3d12_[gpu::d3d12::kFrameGenOwners] = {};
    bool fg_paused_d3d12_ = false;
    bool fg_refusal_logged_[gpu::d3d12::kFrameGenOwners] = {};
    gpu::d3d12::FrameGenInputs fg_inputs_d3d12_;
    bool fg_inputs_ready_ = false;
    gpu::RenderTarget fg_hudless_;
    gpu::RenderTarget fg_ui_;
    bool EnsureFrameGenTargetsD3D12(gpu::Extent2D extent);
    gpu::RenderTarget xr_hud_d3d12_;
    gpu::BindingPoolHandle xr_pool_d3d12_;
    gpu::BindingSetHandle xr_hud_set_d3d12_;
    gpu::PipelineLayoutHandle xr_layout_d3d12_;
    std::vector<std::pair<gpu::Format, gpu::PipelineHandle>> xr_pipelines_d3d12_;
    std::vector<std::pair<void*, gpu::RenderTarget>> xr_wrapped_;
    gpu::RenderTarget window_target_;
    void RecordXrD3D12(gpu::CommandList& list);
    void CopyToXrD3D12(gpu::CommandList& list, gpu::BindingSetHandle set, const XrTarget& target, bool premultiplied);
    void DestroyXrD3D12();
#endif
    VkSemaphore split_wait_ = VK_NULL_HANDLE;
    uint64_t split_wait_value_ = 0;
    bool swapchain_dirty_ = false;
    int swapchain_window_w_ = 0;
    int swapchain_window_h_ = 0;
    bool imgui_ready_ = false;
    bool output_ready_ = false;
    bool targets_missing_ = false;
    std::chrono::steady_clock::time_point targets_retry_at_{};
    float brightness_override_ = 0.0f;

    gpu::ImageResource scene_color_;
    gpu::ImageResource final_;
    gpu::ImageResource ui_top_;
    bool ui_top_ready_ = false;
    bool ui_top_dirty_ = false;
    gpu::ImageResource output_;
    gpu::Format output_format_ = gpu::Format::R8G8B8A8Unorm;
    gpu::Format final_format_ = gpu::Format::R16G16B16A16Sfloat;
    RendererOutputMode output_mode_ = RendererOutputMode::Sdr;
    float hdr_ui_nits_ = 0.0f;
    int hdr_pattern_ = 0;
    float hdr_reach_ = 1.0f;
    render::HdrLook hdr_look_;
    gpu::SamplerHandle linear_sampler_;
    gpu::SamplerHandle wrap_sampler_;
    gpu::ImageViewHandle grain_noise_;
    gpu::BindingLayoutHandle composite_set_layout_;
    gpu::BindingPoolHandle composite_pool_;
    gpu::BindingSetHandle composite_set_;
    gpu::BindingSetHandle final_set_;
    gpu::PipelineLayoutHandle composite_layout_;
    gpu::PipelineHandle composite_pipeline_;
    gpu::PipelineHandle final_composite_pipeline_;
    gpu::BindingLayoutHandle layer_set_layout_;
    gpu::BindingPoolHandle layer_pool_;
    gpu::PipelineLayoutHandle layer_layout_;
    gpu::PipelineHandle layer_pipeline_;
    gpu::BindingSetHandle layer_sets_[kFramesInFlight] = {};

    VkExtent2D render_extent_{0, 0};
    bool presenting_ = false;
    XrFrame xr_frame_;
    bool xr_pending_ = false;
    vk::Image hud_;
    VkDescriptorPool xr_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet hud_set_ = VK_NULL_HANDLE;
    VkPipelineLayout xr_layout_ = VK_NULL_HANDLE;
    std::vector<std::pair<VkFormat, VkPipeline>> xr_pipelines_;
    std::vector<std::future<bool>> screenshot_writes_;
};

}
