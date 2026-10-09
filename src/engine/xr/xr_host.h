#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/render/vk_context.h"

// The experimental VR mode's OpenXR side (docs/vr.md). The Khronos loader (openxr_loader.dll next to pt.exe) is loaded at run
// time only when VR is on: without it, a runtime or a headset, Init fails with a log line and the game runs as without VR.
namespace pt::xr {

// a view's pose in the LOCAL reference space (x right, y up, -z forward, metres) and its field as tangents of its half angles
struct ViewPose {
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 position{0.0f};
    // tan of angleLeft (negative), angleRight, angleUp, angleDown (negative)
    glm::vec4 tangents{-1.0f, 1.0f, 1.0f, -1.0f};
    glm::vec4 angles{0.0f};
};

struct HandPose {
    bool valid = false;
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 position{0.0f};
};

// the controllers after this frame's xrSyncActions
struct ControllerState {
    bool active = false;
    glm::vec2 move{0.0f};
    glm::vec2 turn{0.0f};
    bool interact = false;
    bool back = false;
    bool menu = false;
    bool zoom = false;
    bool gouge = false;
    bool triangle = false;
    // the PC settings page (the pad's View / Share / Create)
    bool settings = false;
    // aim poses, 0 left, 1 right
    HandPose aim[2];
};

struct Swapchain {
    uint64_t handle = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    uint32_t index = 0;
    bool acquired = false;
};

// the layers of one xrEndFrame
struct FrameLayers {
    // the projection layer: the eye images and the poses and fields they were rendered with
    bool projection = false;
    ViewPose eyes[2];
    // the screen quad (the HUD or the virtual screen) in LOCAL: its pose and size in metres
    bool hud = false;
    glm::quat hud_orientation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 hud_position{0.0f};
    glm::vec2 hud_size{1.6f, 0.9f};
    bool screen = false;
    glm::quat screen_orientation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 screen_position{0.0f};
    glm::vec2 screen_size{3.2f, 1.8f};
};

class Host final : public vk::ContextCreator {
public:
    Host();
    ~Host() override;
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    // Loads the loader, creates the instance (XR_KHR_vulkan_enable2) and finds a head-mounted display. False leaves VR off;
    // Error() says why.
    bool Init(const std::string& application);
    bool Ready() const;
    const std::string& Error() const { return error_; }
    const std::string& RuntimeName() const { return runtime_name_; }

    // vk::ContextCreator: the Vulkan instance and device through the runtime
    VkResult CreateInstance(const VkInstanceCreateInfo& info, VkInstance& instance) override;
    VkPhysicalDevice PhysicalDevice(VkInstance instance) override;
    VkResult CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo& info, VkDevice& device) override;

    // After the renderer's device exists: the session, its spaces, actions and swapchains (eye size: the runtime's
    // recommendation times scale)
    bool StartSession(vk::Context& ctx, float scale);
    void Shutdown();

    // The frame loop: PollEvents each loop; WaitFrame (blocks to the headset's rate) while the session runs, then BeginFrame,
    // the views and the actions, the rendering, EndFrame
    void PollEvents();
    bool SessionRunning() const;
    // the runtime ended the session for good (the user quit from the headset's menu): the game quits
    bool ExitRequested() const;
    bool Focused() const;
    // the session had the input focus and lost it (the headset's system menu or another application in front)
    bool FocusLost() const;
    bool WaitFrame();
    bool ShouldRender() const;
    bool BeginFrame();
    void LocateViews();
    void SyncActions();
    void EndFrame(const FrameLayers& layers);
    bool FrameOpen() const { return frame_open_; }
    int64_t DisplayTime() const { return display_time_; }
    double DisplayPeriod() const { return display_period_; }

    const ViewPose& Eye(int i) const { return eyes_[i]; }
    const ViewPose& Head() const { return head_; }
    bool ViewsValid() const { return views_valid_; }
    const ControllerState& Controllers() const { return controllers_; }
    void Haptic(int hand, float amplitude, float seconds);

    Swapchain& EyeSwapchain(int i) { return eye_swapchains_[i]; }
    Swapchain& HudSwapchain() { return hud_swapchain_; }
    Swapchain& ScreenSwapchain() { return screen_swapchain_; }
    bool Acquire(Swapchain& swapchain);
    void Release(Swapchain& swapchain);
    VkExtent2D EyeExtent() const { return eye_swapchains_[0].extent; }

    // the count of frames ended with layers, for the log and the tests
    uint64_t FramesSubmitted() const { return frames_submitted_; }

    // the OpenXR handles and functions (xr_host.cpp)
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    std::string error_;
    std::string runtime_name_;
    ViewPose eyes_[2];
    ViewPose head_;
    bool views_valid_ = false;
    ControllerState controllers_;
    Swapchain eye_swapchains_[2];
    Swapchain hud_swapchain_;
    Swapchain screen_swapchain_;
    int64_t display_time_ = 0;
    double display_period_ = 1.0 / 90.0;
    bool frame_open_ = false;
    bool should_render_ = false;
    uint64_t frames_submitted_ = 0;
};

// whether this build has the VR mode (the OpenXR headers and loader were found at build time)
bool Available();

}
