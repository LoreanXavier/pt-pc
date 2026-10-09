#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>

#include "engine/render/upscale/upscale.h"

namespace pt {

struct FrameGenQueue {
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = UINT32_MAX;
    uint32_t index = 0;
};

struct FrameGenQueues {
    FrameGenQueue present;
    FrameGenQueue acquire;
    FrameGenQueue compute;
};

struct FrameGenPrepare {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    UpscaleImage depth;
    UpscaleImage motion;
    VkExtent2D render{};
    glm::vec2 jitter{0.0f};
    glm::vec2 motion_scale{1.0f};
    float frame_ms = 16.6f;
    float near_plane = 0.05f;
    float fov_y = 1.0f;
    glm::vec3 position{0.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    glm::vec3 right{1.0f, 0.0f, 0.0f};
    glm::vec3 forward{0.0f, 0.0f, 1.0f};
    bool reset = false;
};

enum class FrameStartAction { Continue, RecreateSwapchain, KeepCurrentSwapchain };

class FrameGeneration {
public:
    virtual ~FrameGeneration() = default;
    virtual bool Available(std::string& reason) = 0;
    virtual bool Update(bool wanted) = 0;
    virtual bool Generating() const = 0;
    virtual void Prepare(const FrameGenPrepare& prepare) = 0;
    virtual const vk::Image* Present(uint32_t image_index) = 0;
    virtual void Shutdown() = 0;
    // The frame boundary, before image acquisition and command recording. KeepCurrentSwapchain leaves a dirty swapchain
    // pending while continuing to render against the existing handle.
    virtual FrameStartAction FrameStart(bool swapchain_recreation_pending) {
        (void)swapchain_recreation_pending;
        return FrameStartAction::Continue;
    }
    // the swapchain could not be created: true when this turned itself off and creating it again can work
    virtual bool SwapchainFailed() { return false; }
};

std::unique_ptr<FrameGeneration> CreateFrameGeneration(vk::Context& ctx, const FrameGenQueues& queues);
// NVIDIA DLSS Frame Generation through Streamline (streamline.cpp); null while Streamline is not loaded
std::unique_ptr<FrameGeneration> CreateDlssFrameGeneration(vk::Context& ctx);

}
