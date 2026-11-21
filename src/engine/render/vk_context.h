#pragma once

#include <chrono>
#include <functional>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/render/pipeline_cache_store.h"
#include "engine/render/vk.h"

struct SDL_Window;

namespace pt::vk {

class Context;

class ContextHooks {
public:
    virtual ~ContextHooks() = default;
    virtual void InstanceExtensions(VkInstanceCreateInfo& info, std::vector<const char*>& extensions) = 0;
    virtual void DeviceSetup(VkInstance instance, VkPhysicalDevice physical, std::vector<const char*>& extensions,
                             VkPhysicalDeviceFeatures2& features) = 0;
    virtual void DeviceQueues(VkPhysicalDevice physical, VkSurfaceKHR surface, uint32_t family,
                              std::vector<VkDeviceQueueCreateInfo>& queues) = 0;
    virtual void DeviceCreated(Context& ctx) = 0;
};

class ContextCreator {
public:
    virtual ~ContextCreator() = default;
    virtual VkResult CreateInstance(const VkInstanceCreateInfo& info, VkInstance& instance) = 0;
    virtual VkPhysicalDevice PhysicalDevice(VkInstance instance) = 0;
    virtual VkResult CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo& info, VkDevice& device) = 0;
};

class SwapchainHooks {
public:
    virtual ~SwapchainHooks() = default;
    virtual VkResult CreateSwapchain(const VkSwapchainCreateInfoKHR& info, VkSwapchainKHR& handle) = 0;
    virtual void DestroySwapchain(VkSwapchainKHR handle) = 0;
    virtual VkResult SwapchainImages(VkSwapchainKHR handle, uint32_t* count, VkImage* images) = 0;
    virtual VkResult AcquireNextImage(VkSwapchainKHR handle, VkSemaphore semaphore, uint32_t* index) = 0;
    virtual VkResult QueuePresent(VkQueue queue, const VkPresentInfoKHR& info) = 0;
    virtual VkImageLayout PresentLayout() const = 0;
};

struct Swapchain {
    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkSemaphore> render_finished;
    uint32_t min_image_count = 2;
};

class AllocTag {
public:
    explicit AllocTag(const char* tag);
    ~AllocTag();
    AllocTag(const AllocTag&) = delete;
    AllocTag& operator=(const AllocTag&) = delete;

private:
    const char* previous_;
};

class Context {
public:
    bool Init(SDL_Window* window, bool validation, bool want_hdr = false);
    void Shutdown();

    bool CreateSwapchain(uint32_t width, uint32_t height, bool vsync, bool want_hdr = false);
    void DestroySwapchain();
    VkResult AcquireNextImage(VkSemaphore semaphore, uint32_t* index);
    VkResult QueuePresent(const VkPresentInfoKHR& info);
    const SwapchainHooks* SwapchainOwner() const { return swapchain_owner_; }
    VkImageLayout PresentLayout() const { return swapchain_owner_ ? swapchain_owner_->PresentLayout() : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; }

    bool CreateImage(Image& out, VkFormat format, VkExtent3D extent, VkImageUsageFlags usage, uint32_t mip_levels = 1,
                     uint32_t layers = 1, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT, bool cube = false);
    void DestroyImage(Image& image);
    bool CreateImage(gpu::ImageResource& out, VkFormat format, VkExtent3D extent, VkImageUsageFlags usage, uint32_t mip_levels = 1,
                     uint32_t layers = 1, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT, bool cube = false);
    void DestroyImage(gpu::ImageResource& image);
    bool CreateBuffer(Buffer& out, VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible);
    void DestroyBuffer(Buffer& buffer);

    void Submit(const std::function<void(VkCommandBuffer)>& record);
    void SubmitDeferred(const std::function<void(VkCommandBuffer)>& record, Buffer staging);
    void ReapDeferred(bool wait);
    bool Upload(Buffer& dst, const void* data, VkDeviceSize size);

    VkDeviceSize DeviceLocalBytes() const;
    void DeviceLocalUsage(double& used, double& budget) const;
    void LogMemoryReport(const char* reason);
    bool SetAllocFailure(std::string_view spec);
    uint64_t AllocationCount() const { return alloc_calls_; }
    uint32_t vram_budget_mb = 0;

    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    bool debug_utils = false;
    VkDebugUtilsMessengerEXT address_messenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    bool intel_first_frame_driver = false;
    std::string intel_driver_name;
    VkDevice device = VK_NULL_HANDLE;
    VkPipelineCache pipeline_cache = VK_NULL_HANDLE;
    std::filesystem::path pipeline_cache_dir;
    bool pipeline_cache_cold = false;
    PipelineCacheSaver pipeline_saver;
    uint32_t queue_family = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VmaAllocator allocator = nullptr;
    Swapchain swapchain;
    ContextHooks* hooks = nullptr;
    SwapchainHooks* swapchain_hooks = nullptr;
    ContextCreator* creator = nullptr;
    PFN_vkGetInstanceProcAddr loader = nullptr;
    std::function<void()> before_device_destroy;
    bool force_vsync_off = false;
    bool force_vsync = false;
    bool swapchain_refused = false;

    bool want_ray_query = false;
    bool ray_query_supported = false;
    bool depth_bounds = false;
    bool ray_query = false;
    std::string ray_query_missing;
    VkDeviceSize scratch_alignment = 256;

    bool memory_budget = false;
    bool device_fault = false;
    bool checkpoints = false;
    bool address_report = false;
    bool hdr_metadata = false;
    int hdr_peak_setting = 0;
    float display_peak_nits = 0.0f;
    float hdr_paper_nits = 200.0f;
    float hdr_min_nits = 0.0f;
    float HdrPeakNits() const;
    void SetHdrMetadata();
    void CheckDeviceLost(VkResult result, const char* where);

private:
    bool InjectFailure(const char* what);
    void TrackAllocation(VmaAllocation allocation);
    void UntrackAllocation(VmaAllocation allocation);
    void LogAllocationFailure(const char* what, VkResult result, const std::string& detail);

    struct Tracked {
        const char* tag = "other";
        VkDeviceSize size = 0;
        uint32_t memory_type = 0;
    };
    mutable std::mutex alloc_mutex_;
    std::unordered_map<VmaAllocation, Tracked> tracked_;
    uint64_t alloc_calls_ = 0;
    uint64_t fail_at_ = 0;
    bool fail_after_ = false;
    double fail_rate_ = 0.0;
    uint64_t fail_rng_ = 0x9E3779B97F4A7C15ull;
    std::chrono::steady_clock::time_point last_failure_report_{};
    std::chrono::steady_clock::time_point last_failure_line_{};
    uint64_t failures_logged_ = 0;
    uint64_t failures_hidden_ = 0;
    bool hdr_off_logged_ = false;
    SDL_Window* window_ = nullptr;
    VkCommandPool upload_pool_ = VK_NULL_HANDLE;
    struct DeferredSubmit {
        VkFence fence = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        Buffer staging;
    };
    std::vector<DeferredSubmit> deferred_;
    VkDeviceSize deferred_bytes_ = 0;
    static constexpr VkDeviceSize kDeferredStagingLimit = 256ull << 20;
    SwapchainHooks* swapchain_owner_ = nullptr;
};

/* The Vulkan library to load: on macOS MoltenVK, shipped with the game (docs/macos.md); empty elsewhere, where volk finds the
   system's loader. The window's surface must come from the same library, so it is also SDL's (SDL_HINT_VULKAN_LIBRARY). */
std::string VulkanLibraryPath();

}
