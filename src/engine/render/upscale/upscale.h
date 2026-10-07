#pragma once

#include <glm/glm.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "engine/render/vk_context.h"

namespace pt {

class FrameGeneration;

enum class UpscalerKind : int { Off = 0, Fsr = 1, Dlss = 2, Xess = 3, Spatial = 4, Fsr4 = 5, Count = 6 };
enum class DlssModel : int { Auto = 0, K = 1, L = 2, M = 3, Count = 4 };
enum class FrameGenKind : int { Off = 0, Fsr = 1, Dlss = 2, Count = 3 };
enum class UpscaleQuality : int { NativeAA = 0, Quality = 1, Balanced = 2, Performance = 3, UltraPerformance = 4, Custom = 5, Count = 6 };

struct UpscaleSettings {
    UpscalerKind kind = UpscalerKind::Off;
    UpscaleQuality quality = UpscaleQuality::Quality;
    float scale = 0.667f;
    float sharpness = 0.0f;
    DlssModel dlss_model = DlssModel::Auto;
    FrameGenKind frame_generation = FrameGenKind::Off;
    bool operator==(const UpscaleSettings&) const = default;
};

const char* UpscalerName(UpscalerKind kind);
const char* UpscalerKey(UpscalerKind kind);
const char* UpscaleQualityName(UpscaleQuality quality);
const char* UpscaleQualityKey(UpscaleQuality quality);
bool ParseUpscaler(const std::string& text, UpscalerKind& out);
bool ParseUpscaleQuality(const std::string& text, UpscaleQuality& out);
const char* DlssModelKey(DlssModel model);
bool ParseDlssModel(const std::string& text, DlssModel& out);
const char* FrameGenKey(FrameGenKind kind);
bool ParseFrameGen(const std::string& text, FrameGenKind& out);

constexpr uint32_t kAmdVendor = 0x1002;
bool AmdGcnGpu(uint32_t vendor_id, uint32_t min_subgroup_size, uint32_t max_subgroup_size);
bool FsrFrameGenHardware(VkPhysicalDevice physical, const VkPhysicalDeviceProperties& properties, std::string& reason);

struct DlssFrameGenSupport {
    bool checked = false;
    bool hardware = false;
    bool built = false;
    std::string reason;
    std::string note;
};
float UpscaleRatio(UpscaleQuality quality, float custom_scale);
VkExtent2D UpscaleRenderExtent(VkExtent2D output, UpscaleQuality quality, float custom_scale);
int32_t JitterPhaseCount(uint32_t render_width, uint32_t output_width);
glm::vec2 JitterOffset(int32_t index, int32_t phase_count);

struct UpscaleImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags usage = 0;
    VkExtent2D extent{};
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool Valid() const { return image != VK_NULL_HANDLE; }
};

struct UpscaleDispatch {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    UpscaleImage color;
    UpscaleImage depth;
    UpscaleImage motion;
    UpscaleImage reactive;
    UpscaleImage transparency;
    UpscaleImage exposure;
    UpscaleImage output;
    VkExtent2D render{};
    VkExtent2D display{};
    glm::vec2 jitter{0.0f};
    glm::vec2 motion_scale{1.0f};
    float pre_exposure = 1.0f;
    float sharpness = 0.0f;
    float frame_ms = 16.6f;
    float near_plane = 0.05f;
    float fov_y = 1.0f;
    bool reset = false;
    uint32_t frame_index = 0;
};

struct DeviceFeatureSet {
    std::vector<std::string> extensions;
    VkPhysicalDeviceFeatures core{};
    VkPhysicalDeviceVulkan11Features v11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mutable_descriptor{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT};
    bool unknown = false;
};

struct UpscaleCreate {
    VkExtent2D render{};
    VkExtent2D display{};
    UpscaleQuality quality = UpscaleQuality::Quality;
    DlssModel dlss_model = DlssModel::Auto;
    VkFormat color_format = VK_FORMAT_UNDEFINED;
};

class UpscaleBackend {
public:
    virtual ~UpscaleBackend() = default;
    virtual UpscalerKind Kind() const = 0;
    virtual bool Supported(std::string& reason);
    virtual bool Available(std::string& reason) = 0;
    virtual bool Create(VkCommandBuffer cmd, const UpscaleCreate& create) = 0;
    virtual bool Dispatch(const UpscaleDispatch& dispatch) = 0;
    virtual void Release() = 0;
    virtual float MipBias(float render_over_display) const;
};

class UpscaleHost : public vk::ContextHooks {
public:
    static UpscaleHost& Get();
    void SetStartupUpscaler(UpscalerKind kind) { startup_ = kind; }
    void InstanceExtensions(VkInstanceCreateInfo& info, std::vector<const char*>& extensions) override;
    void DeviceSetup(VkInstance instance, VkPhysicalDevice physical, std::vector<const char*>& extensions,
                     VkPhysicalDeviceFeatures2& features) override;
    void DeviceQueues(VkPhysicalDevice physical, VkSurfaceKHR surface, uint32_t family, std::vector<VkDeviceQueueCreateInfo>& queues) override;
    void DeviceCreated(vk::Context& ctx) override;
    FrameGeneration* FrameGen();
    FrameGeneration* DlssFrameGenImpl();
    const DlssFrameGenSupport& DlssFrameGen() const { return dlss_fg_; }
    void SetMenuOpen(bool open) { menu_open_ = open; }
    bool MenuOpen() const { return menu_open_; }
    void SetDlssFrameGenFailed(bool failed) { dlss_fg_failed_ = failed; }
    void FrameTick();
    void Shutdown();
    UpscaleBackend* Backend(UpscalerKind kind);
    bool Available(UpscalerKind kind, std::string& reason, bool probe = true);
    bool Enabled(const char* name, bool device) const;
    std::string MissingFeature(const DeviceFeatureSet& wanted) const;

private:
    void CheckDlssFrameGen();
    bool InstanceHas(const char* name) const;
    bool DeviceHas(const char* name) const;
    bool AddExtension(std::vector<const char*>& extensions, const char* name, bool device);

    vk::Context* ctx_ = nullptr;
    std::unique_ptr<UpscaleBackend> backends_[static_cast<int>(UpscalerKind::Count)];
    bool probed_[static_cast<int>(UpscalerKind::Count)] = {};
    bool available_[static_cast<int>(UpscalerKind::Count)] = {};
    std::string reasons_[static_cast<int>(UpscalerKind::Count)];
    std::vector<VkExtensionProperties> instance_available_;
    std::vector<VkExtensionProperties> device_available_;
    std::vector<std::string> instance_enabled_;
    std::vector<std::string> device_enabled_;
    std::deque<std::string> names_;
    UpscalerKind startup_ = UpscalerKind::Off;
    std::vector<float> queue_priorities_;
    uint32_t fg_family_[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
    uint32_t fg_index_[3] = {};
    VkQueue fg_queue_[3] = {};
    std::unique_ptr<FrameGeneration> frame_gen_;
    std::unique_ptr<FrameGeneration> dlss_frame_gen_;
    bool dlss_frame_gen_created_ = false;
    DlssFrameGenSupport dlss_fg_;
    bool dlss_fg_failed_ = false;
    bool menu_open_ = false;
    bool frame_gen_created_ = false;
    DeviceFeatureSet enabled_features_;
    VkPhysicalDeviceVulkan11Features chain_v11_{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT chain_mutable_{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT};
    VkPhysicalDevicePresentIdFeaturesKHR chain_present_id_{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
    int frame_stats_ = -1;
    std::chrono::steady_clock::time_point last_tick_{};
    std::vector<float> frame_intervals_;
};

std::unique_ptr<UpscaleBackend> CreateFsrBackend(vk::Context& ctx, int generation);
std::unique_ptr<UpscaleBackend> CreateDlssBackend(vk::Context& ctx);
std::unique_ptr<UpscaleBackend> CreateStreamlineDlssBackend(vk::Context& ctx);
uint32_t DlssPresetHint(DlssModel model);
bool DlssAutoExposure();
std::unique_ptr<UpscaleBackend> CreateXessBackend(vk::Context& ctx);
void XessDeviceRequirements(bool query, VkInstance instance, VkPhysicalDevice physical, DeviceFeatureSet& out);
void DlssInstanceExtensions(bool query, std::vector<std::string>& out);
void DlssDeviceExtensions(bool query, VkInstance instance, VkPhysicalDevice physical, std::vector<std::string>& out);
bool DlssFrameGenRequirements(VkInstance instance, VkPhysicalDevice physical, uint32_t& unsupported, std::string& detail);

}
