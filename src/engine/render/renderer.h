#pragma once

#include <glm/glm.hpp>

#include <filesystem>
#include <functional>
#include <utility>
#include <vector>

#include "engine/render/vk_context.h"

struct SDL_Window;

namespace pt {

struct RendererSettings {
    bool validation = false;
    bool vsync = true;
    bool headless = false;
    uint32_t width = 1600;
    uint32_t height = 900;
};

struct XrTarget {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    glm::vec4 rect{0.0f, 0.0f, 1.0f, 1.0f};
};

struct XrFrame {
    const XrTarget* eye = nullptr;
    const XrTarget* hud = nullptr;
    bool overlay_on_frame = false;
};

class Renderer {
public:
    bool Init(SDL_Window* window, const RendererSettings& settings);
    void Shutdown();

    bool BeginFrame(bool present = true);
    void EndFrame(bool draw_ui);
    void Resize(uint32_t width, uint32_t height);
    void SetVsync(bool enabled);
    bool SaveScreenshot(const std::filesystem::path& path);

    vk::Context& Context() { return ctx_; }
    VkCommandBuffer Cmd() const { return frames_[frame_index_].cmd; }
    const vk::Image& SceneColor() const { return scene_color_; }
    VkExtent2D RenderExtent() const { return {scene_color_.extent.width, scene_color_.extent.height}; }
    uint32_t FrameIndex() const { return frame_index_; }
    static constexpr uint32_t kFramesInFlight = 2;
    static constexpr VkFormat kSceneColorFormat = VK_FORMAT_R8G8B8A8_UNORM;

    float exposure = 1.0f;
    float output_brightness = 1.0f;
    float fade[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float grain[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float grain_offset[2] = {0.0f, 0.0f};
    std::function<void(VkCommandBuffer, VkImageView, VkExtent2D)> overlay;
    std::function<const vk::Image*(uint32_t)> hudless;

    void SetGrainNoise(VkImageView view);

    void SetRenderExtent(VkExtent2D extent) { render_extent_ = extent; }
    void SetXrFrame(const XrFrame& frame) { xr_frame_ = frame; xr_pending_ = true; }
    static constexpr VkExtent2D kHudExtent{1920, 1080};

private:
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore image_available = VK_NULL_HANDLE;
        VkFence in_flight = VK_NULL_HANDLE;
    };

    bool CreateTargets(uint32_t width, uint32_t height);
    void DestroyTargets();
    bool CreateCompositePipeline(VkFormat output_format);
    bool InitImGui(SDL_Window* window);
    void Composite(VkCommandBuffer cmd, VkDescriptorSet set, float mode, VkExtent2D extent, VkOffset2D offset = {0, 0});
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
    bool swapchain_dirty_ = false;
    bool imgui_ready_ = false;
    bool output_ready_ = false;
    float brightness_override_ = 0.0f;

    vk::Image scene_color_;
    vk::Image final_;
    vk::Image output_;
    VkFormat output_format_ = VK_FORMAT_R8G8B8A8_UNORM;
    VkSampler linear_sampler_ = VK_NULL_HANDLE;
    VkSampler wrap_sampler_ = VK_NULL_HANDLE;
    VkImageView grain_noise_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout composite_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool composite_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet composite_set_ = VK_NULL_HANDLE;
    VkDescriptorSet final_set_ = VK_NULL_HANDLE;
    VkPipelineLayout composite_layout_ = VK_NULL_HANDLE;
    VkPipeline composite_pipeline_ = VK_NULL_HANDLE;

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
