#pragma once

#include <functional>
#include <string>
#include <vector>

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
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkSemaphore> render_finished;
    uint32_t min_image_count = 2;
};

class Context {
public:
    bool Init(SDL_Window* window, bool validation);
    void Shutdown();

    bool CreateSwapchain(uint32_t width, uint32_t height, bool vsync);
    void DestroySwapchain();
    VkResult AcquireNextImage(VkSemaphore semaphore, uint32_t* index);
    VkResult QueuePresent(const VkPresentInfoKHR& info);
    const SwapchainHooks* SwapchainOwner() const { return swapchain_owner_; }
    VkImageLayout PresentLayout() const { return swapchain_owner_ ? swapchain_owner_->PresentLayout() : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; }

    bool CreateImage(Image& out, VkFormat format, VkExtent3D extent, VkImageUsageFlags usage, uint32_t mip_levels = 1,
                     uint32_t layers = 1, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT, bool cube = false);
    void DestroyImage(Image& image);
    bool CreateBuffer(Buffer& out, VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible);
    void DestroyBuffer(Buffer& buffer);

    void Submit(const std::function<void(VkCommandBuffer)>& record);
    bool Upload(Buffer& dst, const void* data, VkDeviceSize size);

    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    VkDevice device = VK_NULL_HANDLE;
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

    bool want_ray_query = false;
    bool ray_query_supported = false;
    bool ray_query = false;
    std::string ray_query_missing;
    VkDeviceSize scratch_alignment = 256;

    bool memory_budget = false;
    bool device_fault = false;
    bool checkpoints = false;
    void CheckDeviceLost(VkResult result, const char* where);

private:
    VkCommandPool upload_pool_ = VK_NULL_HANDLE;
    SwapchainHooks* swapchain_owner_ = nullptr;
};

/* The Vulkan library to load: on macOS MoltenVK, shipped with the game (docs/macos.md); empty elsewhere, where volk finds the
   system's loader. The window's surface must come from the same library, so it is also SDL's (SDL_HINT_VULKAN_LIBRARY). */
std::string VulkanLibraryPath();

}
