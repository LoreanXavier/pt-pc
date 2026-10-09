#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

#include "engine/render/vk.h"

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

bool Start(const std::filesystem::path& data_dir, std::string& reason);
bool Active();
PFN_vkGetInstanceProcAddr InstanceProcAddr();
void DeviceCreated(VkInstance instance, VkPhysicalDevice physical, VkDevice device);
void Shutdown();

bool DlssSupported();
bool FrameGenSupported(std::string* reason);
bool FrameGenNeedsVsyncOff();
bool IsFrameGenLoaded(bool& loaded);
/* Disables sl.dlss_g's feature hooks for this run; the Streamline plug-in module remains process-loaded. */
bool DisableFrameGenFeature();

void BeginFrame();
void SetMarker(Marker marker);
void SetConstants(const FrameConstants& constants);
bool ConstantsSet();
void SetFrameLimit(uint32_t microseconds);

const void* FrameToken();
constexpr uint32_t kViewport = 0;

}
