#pragma once

#include <glm/glm.hpp>

#include <filesystem>
#include <functional>
#include <utility>
#include <vector>

#include "engine/render/vk_context.h"

struct SDL_Window;

namespace pt {

enum class FrameStartAction;
enum class RendererOutputMode { Sdr, ScRgb, Hdr10 };

struct RendererSettings {
    bool validation = false;
    bool vsync = true;
    bool headless = false;
    bool hdr = false;
    std::filesystem::path pipeline_cache_dir;
    uint32_t width = 1600;
    uint32_t height = 900;
};

// The VR mode's outputs of one frame (docs/vr.md): images of the OpenXR runtime's swapchains that EndFrame fills besides the
// window or the headless target. Colour goes in as linear values (an sRGB swapchain encodes them again on write).
struct XrTarget {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    // the part of the frame the target shows: uv offset (xy) and size (zw)
    glm::vec4 rect{0.0f, 0.0f, 1.0f, 1.0f};
};

struct XrFrame {
    // the composited frame (fade, brightness, film grain as the window's), cropped to the target's rect
    const XrTarget* eye = nullptr;    // the overlay (the game's UI) alone on a transparent image of kHudExtent, premultiplied by its coverage
    const XrTarget* hud = nullptr;
    // the overlay over the frame as without VR (the virtual screen), else only on the hud
    bool overlay_on_frame = false;
};

// Apple MetalFX frame interpolation (metalfx_backend.mm) does not own the swapchain: the renderer presents the generated frame
// itself, one extra swapchain image per rendered frame, before the rendered one. Set from scene_upscale.cpp only while the
// MetalFX frame generation is generating.
struct GeneratedFrameHooks {
    // runs the interpolation and returns the generated frame (output extent) to composite; null repeats the rendered frame
    std::function<const vk::Image*()> generate;
    // the ordering semaphores of the frame's submit (signal) and of the generated frame's composite (wait)
    std::function<bool(VkSemaphore&, uint64_t&, VkSemaphore&, uint64_t&)> sync;
};

class Renderer {
public:
    bool Init(SDL_Window* window, const RendererSettings& settings);
    void Shutdown();

    // present: whether this frame goes to the window (VR draws the second eye without presenting)
    bool BeginFrame(bool present = true);
    void EndFrame(bool draw_ui);
    // Apple MetalFX: the upscaler runs on a Metal command buffer, which cannot be part of a Vulkan one. This ends and submits
    // the frame recorded so far, signaling `signal` at `signal_value`; the upscaler runs between the submits; the command
    // buffer that follows (and the rest of the frame with it) waits on `wait` at `wait_value`.
    bool SplitForExternal(VkSemaphore signal, uint64_t signal_value, VkSemaphore wait, uint64_t wait_value);
    void Resize(uint32_t width, uint32_t height);
    void SetVsync(bool enabled);
    bool SaveScreenshot(const std::filesystem::path& path, glm::vec4 crop = {0.0f, 0.0f, 1.0f, 1.0f});

    vk::Context& Context() { return ctx_; }
    // the command buffer the frame records into: the frame's own until SplitForExternal, then the resume buffer
    VkCommandBuffer Cmd() const { return active_cmd_; }
    const vk::Image& SceneColor() const { return scene_color_; }
    VkExtent2D RenderExtent() const { return {scene_color_.extent.width, scene_color_.extent.height}; }
    uint32_t FrameIndex() const { return frame_index_; }
    static constexpr uint32_t kFramesInFlight = 2;
    static constexpr VkFormat kSceneColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat SceneColorFormat() const { return output_mode_ == RendererOutputMode::Sdr ? kSceneColorFormat : VK_FORMAT_R16G16B16A16_SFLOAT; }
    RendererOutputMode OutputMode() const { return output_mode_; }

    int photo_filter = 0;
    float exposure = 1.0f;
    float output_brightness = 1.0f;
    // the start's title picture (GameUi::ShowBootSplash) is shown as it is: no film grain, no brightness setting
    bool plain_output = false;
    float fade[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float grain[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float grain_offset[2] = {0.0f, 0.0f};
    std::function<void(VkCommandBuffer, VkImageView, VkExtent2D)> overlay;
    std::function<const vk::Image*(uint32_t)> hudless;
    GeneratedFrameHooks generated_frame;
    // frame generation's hooks, applied before swapchain creation and image acquisition
    std::function<FrameStartAction(bool)> frame_start;
    std::function<bool()> swapchain_failed;

    void SetGrainNoise(VkImageView view);

    // VR: a fixed render size independent of the window (the eye images), applied at the next BeginFrame; {0, 0} follows the
    // window again. The window then shows the frame scaled to fit.
    void SetRenderExtent(VkExtent2D extent) { render_extent_ = extent; }
    // VR: the swapchain images the next EndFrame also fills (only for that frame)
    void SetXrFrame(const XrFrame& frame) { xr_frame_ = frame; xr_pending_ = true; }
    static constexpr VkExtent2D kHudExtent{1920, 1080};

private:
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        // the buffer the frame continues in after SplitForExternal (Apple MetalFX)
        VkCommandBuffer resume = VK_NULL_HANDLE;
        // the buffer the generated frame's composite records into (Apple MetalFX frame interpolation)
        VkCommandBuffer gen_cmd = VK_NULL_HANDLE;
        VkSemaphore image_available = VK_NULL_HANDLE;
        // the generated frame's swapchain image acquisition (Apple MetalFX frame interpolation)
        VkSemaphore gen_available = VK_NULL_HANDLE;
        VkFence in_flight = VK_NULL_HANDLE;
        // covers the generated frame's composite, which is submitted after the frame's own (the pool is reset at the next
        // use of this slot, when both must be done)
        VkFence gen_in_flight = VK_NULL_HANDLE;
        bool gen_pending = false;
    };

    bool CreateTargets(uint32_t width, uint32_t height);
    void DestroyTargets();
    // the generated frame's composite, submit and present (Apple MetalFX frame interpolation); source nullptr repeats the
    // rendered frame
    void PresentGeneratedFrame(Frame& frame, const vk::Image* source, VkSemaphore fg_wait, uint64_t fg_wait_value, bool draw_ui);    bool CreateCompositePipeline(VkFormat output_format);
    bool InitImGui(SDL_Window* window);
    void Composite(VkCommandBuffer cmd, VkDescriptorSet set, float mode, VkExtent2D extent, VkFormat target_format,
                   VkOffset2D offset = {0, 0});
    void WriteCompositeSets();
    void RecordXr(VkCommandBuffer cmd);
    void CopyToXr(VkCommandBuffer cmd, VkDescriptorSet set, const XrTarget& target, bool premultiplied);
    VkPipeline XrPipeline(VkFormat format);
    void DestroyXr();

    vk::Context ctx_;
    RendererSettings settings_;
    SDL_Window* window_ = nullptr;
    Frame frames_[kFramesInFlight];
    uint32_t frame_index_ = 0;
    uint32_t image_index_ = 0;
    // the swapchain image the generated frame is composited into, and whether one was acquired this frame (Apple MetalFX
    // frame interpolation)
    uint32_t gen_index_ = 0;
    bool gen_acquired_ = false;
    // the composite sets that bind the generated frame's image (one per frame slot)
    VkDescriptorSet gen_set_[kFramesInFlight] = {};
    // the command buffer the frame records into (frame.cmd until the split, frames_[frame_index_].resume after)
    VkCommandBuffer active_cmd_ = VK_NULL_HANDLE;
    // the wait the resume buffer is submitted with (SplitForExternal)
    VkSemaphore split_wait_ = VK_NULL_HANDLE;
    uint64_t split_wait_value_ = 0;
    bool swapchain_dirty_ = false;
    bool imgui_ready_ = false;
    bool output_ready_ = false;
    float brightness_override_ = 0.0f;

    vk::Image scene_color_;
    vk::Image final_;
    vk::Image output_;
    VkFormat output_format_ = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat final_format_ = VK_FORMAT_R8G8B8A8_UNORM;
    RendererOutputMode output_mode_ = RendererOutputMode::Sdr;
    VkSampler linear_sampler_ = VK_NULL_HANDLE;
    VkSampler wrap_sampler_ = VK_NULL_HANDLE;
    VkImageView grain_noise_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout composite_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool composite_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet composite_set_ = VK_NULL_HANDLE;
    VkDescriptorSet final_set_ = VK_NULL_HANDLE;
    VkPipelineLayout composite_layout_ = VK_NULL_HANDLE;
    VkPipeline composite_pipeline_ = VK_NULL_HANDLE;
    VkPipeline final_composite_pipeline_ = VK_NULL_HANDLE;

    // VR (SetRenderExtent, SetXrFrame)
    VkExtent2D render_extent_{0, 0};
    bool presenting_ = false;
    XrFrame xr_frame_;
    bool xr_pending_ = false;
    vk::Image hud_;
    VkDescriptorPool xr_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet hud_set_ = VK_NULL_HANDLE;
    VkPipelineLayout xr_layout_ = VK_NULL_HANDLE;
    std::vector<std::pair<VkFormat, VkPipeline>> xr_pipelines_;
};

}
