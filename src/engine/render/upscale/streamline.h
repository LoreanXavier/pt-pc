#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

#include "engine/render/vk.h"

// NVIDIA Streamline 2.14.1 (upscaling.md, DLSS Frame Generation): loaded only for a start whose pt.ini selects DLSS Frame
// Generation (or with PT_STREAMLINE=1, for tests). It then owns the Vulkan loader (the signed sl.interposer.dll), so
// Streamline's proxies create the instance, the device and the swapchain with what its plugins need; DLSS Super Resolution
// runs through sl.dlss for that start, so NGX has one owner; and the frame carries Streamline's frame token, the Reflex sleep
// and the PCL markers that DLSS Frame Generation requires.
namespace pt::streamline {

enum class Marker : uint32_t { SimulationStart = 0, SimulationEnd = 1, RenderSubmitStart = 2, RenderSubmitEnd = 3, PresentStart = 4, PresentEnd = 5 };

struct FrameConstants {
    glm::mat4 view_to_clip{1.0f};
    glm::mat4 world_to_clip{1.0f};
    glm::mat4 previous_world_to_clip{1.0f};
    glm::vec2 jitter{0.0f};
    glm::vec2 motion_scale{1.0f};
    glm::vec3 position{0.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    glm::vec3 right{1.0f, 0.0f, 0.0f};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    float near_plane = 0.05f;
    float fov_y = 1.0f;
    float aspect = 1.0f;
    bool reset = false;
};

// Loads the signed interposer and calls slInit (sl.dlss, sl.dlss_g, sl.reflex, sl.pcl). Before any Vulkan call. data_dir
// holds the Streamline log. False (with the reason) leaves the game as without Streamline.
bool Start(const std::filesystem::path& data_dir, std::string& reason);
bool Active();
// the interposer's vkGetInstanceProcAddr for volkInitializeCustom
PFN_vkGetInstanceProcAddr InstanceProcAddr();
// after device creation: the feature checks (slIsFeatureSupported with the VkPhysicalDevice) and the Reflex options
void DeviceCreated(VkInstance instance, VkPhysicalDevice physical, VkDevice device);
// before the device is destroyed (after the swapchain)
void Shutdown();

bool DlssSupported();
// DLSS Frame Generation: supported on this adapter (reason: Streamline's), and whether it needs v-sync off (Vulkan)
bool FrameGenSupported(std::string* reason);
bool FrameGenNeedsVsyncOff();
// Queries the interposer on the render/present thread after a feature load-state change fails.
bool IsFrameGenLoaded(bool& loaded);
/* Disables sl.dlss_g's feature hooks for this run; the Streamline plug-in module remains process-loaded. */
bool DisableFrameGenFeature();

// per loop: a new frame token, the Reflex sleep and the simulation start marker
void BeginFrame();
void SetMarker(Marker marker);
// once per rendered frame, before the evaluation and the present that use them
void SetConstants(const FrameConstants& constants);
bool ConstantsSet();
// Reflex frame limit (0 off), in microseconds per rendered frame
void SetFrameLimit(uint32_t microseconds);

// the opaque handles the features use (sl::FrameToken*, a viewport id)
const void* FrameToken();
constexpr uint32_t kViewport = 0;

}
