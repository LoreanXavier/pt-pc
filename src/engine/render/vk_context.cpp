#include "engine/render/vk_context.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "engine/core/crash_report.h"
#include "engine/core/log.h"

namespace pt {
extern bool g_checkpoints;
}

namespace pt::vk {
namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    const char* label = data->cmdBufLabelCount > 0 && data->pCmdBufLabels[data->cmdBufLabelCount - 1].pLabelName
                            ? data->pCmdBufLabels[data->cmdBufLabelCount - 1].pLabelName
                            : "";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        LogError("vulkan: {}{}{}", data->pMessage, *label ? " [pass " : "", *label ? std::string(label) + "]" : std::string());
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        LogWarn("vulkan: {}{}{}", data->pMessage, *label ? " [pass " : "", *label ? std::string(label) + "]" : std::string());
    }
    return VK_FALSE;
}

constexpr const char* kRayQueryExtensions[] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME,
                                               VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};

bool RayQuerySupport(VkPhysicalDevice physical, std::string& missing) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, available.data());
    for (const char* name : kRayQueryExtensions) {
        if (std::none_of(available.begin(), available.end(), [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; })) {
            missing = name;
            return false;
        }
    }
    VkPhysicalDeviceRayQueryFeaturesKHR ray_query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    acceleration.pNext = &ray_query;
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.pNext = &acceleration;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &v12;
    vkGetPhysicalDeviceFeatures2(physical, &features);
    if (!acceleration.accelerationStructure || !ray_query.rayQuery || !v12.bufferDeviceAddress) {
        missing = !acceleration.accelerationStructure ? "accelerationStructure" : !ray_query.rayQuery ? "rayQuery" : "bufferDeviceAddress";
        return false;
    }
    missing.clear();
    return true;
}

bool HasLayer(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    return std::any_of(layers.begin(), layers.end(), [&](const VkLayerProperties& l) { return std::strcmp(l.layerName, name) == 0; });
}

}

bool Context::Init(SDL_Window* window, bool validation) {
    if (loader) {
        /* With Streamline loaded, instance, device and swapchain must come from its proxies, so volk takes the interposer's loader instead of vulkan-1.dll. */
        volkInitializeCustom(loader);
    } else if (!Check(volkInitialize(), "volkInitialize")) {
        return false;
    }
    std::vector<const char*> extensions;
    if (window) {
        Uint32 count = 0;
        const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
        extensions.assign(names, names + count);
    }
    std::vector<const char*> layers;
    if (validation && HasLayer("VK_LAYER_KHRONOS_validation")) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    } else {
        validation = false;
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "pt-port";
    app.pEngineName = "pt-port";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    instance_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    instance_info.ppEnabledExtensionNames = extensions.data();
    instance_info.enabledLayerCount = static_cast<uint32_t>(layers.size());
    instance_info.ppEnabledLayerNames = layers.data();
    if (hooks) {
        hooks->InstanceExtensions(instance_info, extensions);
        instance_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        instance_info.ppEnabledExtensionNames = extensions.data();
    }
    const VkResult instance_result = creator ? creator->CreateInstance(instance_info, instance) : vkCreateInstance(&instance_info, nullptr, &instance);
    if (!Check(instance_result, "vkCreateInstance")) {
        return false;
    }
    volkLoadInstance(instance);

    if (validation) {
        VkDebugUtilsMessengerCreateInfoEXT messenger_info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messenger_info.pfnUserCallback = DebugCallback;
        vkCreateDebugUtilsMessengerEXT(instance, &messenger_info, nullptr, &messenger);
    }

    if (window && !SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) {
        LogError("SDL_Vulkan_CreateSurface: {}", SDL_GetError());
        return false;
    }

    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(instance, &device_count, devices.data());
    if (creator) {
        if (const VkPhysicalDevice required = creator->PhysicalDevice(instance)) {
            devices.assign(1, required);
        }
    }
    int best_score = -1;
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(candidate, &props);
        if (props.apiVersion < VK_API_VERSION_1_3) {
            continue;
        }
        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
        for (uint32_t i = 0; i < family_count; ++i) {
            VkBool32 present = VK_TRUE;
            if (surface) {
                vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &present);
            }
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                const int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
                if (score > best_score) {
                    best_score = score;
                    physical = candidate;
                    queue_family = i;
                    properties = props;
                }
                break;
            }
        }
    }
    if (!physical) {
        LogError("vulkan: no Vulkan 1.3 device with a graphics queue");
        return false;
    }
    LogInfo("vulkan: using {} (driver {:X})", properties.deviceName, properties.driverVersion);

    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;
    features13.shaderDemoteToHelperInvocation = VK_TRUE;
    VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features12.pNext = &features13;
    features12.descriptorIndexing = VK_TRUE;
    features12.runtimeDescriptorArray = VK_TRUE;
    features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    features12.descriptorBindingPartiallyBound = VK_TRUE;
    features12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    features12.timelineSemaphore = VK_TRUE;
    features12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    features12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
    features12.scalarBlockLayout = VK_TRUE;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &features12;
    features.features.samplerAnisotropy = VK_TRUE;
    features.features.textureCompressionBC = VK_TRUE;
    features.features.fillModeNonSolid = VK_TRUE;
    features.features.shaderInt16 = VK_TRUE;
    features.features.shaderClipDistance = VK_TRUE;
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(physical, &supported);
    features.features.independentBlend = supported.independentBlend;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    std::vector<const char*> device_extensions;
    if (surface) {
        device_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
    std::vector<VkDeviceQueueCreateInfo> queue_infos{queue_info};
    if (hooks) {
        hooks->DeviceSetup(instance, physical, device_extensions, features);
        hooks->DeviceQueues(physical, surface, queue_family, queue_infos);
    }
    ray_query_supported = RayQuerySupport(physical, ray_query_missing);
    VkPhysicalDeviceFaultFeaturesEXT fault_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    {
        uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> available(count);
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, available.data());
        const auto has = [&](const char* name) {
            return std::any_of(available.begin(), available.end(), [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
        };
        const auto add = [&](const char* name) {
            if (std::none_of(device_extensions.begin(), device_extensions.end(), [&](const char* e) { return std::strcmp(e, name) == 0; })) {
                device_extensions.push_back(name);
            }
        };
        if (has(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) {
            add(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
            memory_budget = true;
        }
        if (has(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME)) {
            add(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
            checkpoints = true;
        }
        if (has(VK_EXT_DEVICE_FAULT_EXTENSION_NAME)) {
            VkPhysicalDeviceFaultFeaturesEXT query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
            VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features2.pNext = &query;
            vkGetPhysicalDeviceFeatures2(physical, &features2);
            if (query.deviceFault) {
                add(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
                fault_features.deviceFault = VK_TRUE;
                fault_features.pNext = features.pNext;
                features.pNext = &fault_features;
                device_fault = true;
            }
        }
        LogInfo("vulkan: diagnostics: memory budget {}, device fault {}, checkpoints {}", memory_budget, device_fault, checkpoints);
    }
    VkPhysicalDeviceRayQueryFeaturesKHR ray_query_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    ray_query = want_ray_query && ray_query_supported;
    if (ray_query) {
        for (const char* name : kRayQueryExtensions) {
            if (std::none_of(device_extensions.begin(), device_extensions.end(), [&](const char* e) { return std::strcmp(e, name) == 0; })) {
                device_extensions.push_back(name);
            }
        }
        ray_query_features.rayQuery = VK_TRUE;
        acceleration_features.accelerationStructure = VK_TRUE;
        acceleration_features.pNext = &ray_query_features;
        ray_query_features.pNext = features.pNext;
        features.pNext = &acceleration_features;
        features12.bufferDeviceAddress = VK_TRUE;
        VkPhysicalDeviceAccelerationStructurePropertiesKHR acceleration_properties{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties2.pNext = &acceleration_properties;
        vkGetPhysicalDeviceProperties2(physical, &properties2);
        scratch_alignment = std::max<VkDeviceSize>(acceleration_properties.minAccelerationStructureScratchOffsetAlignment, 1);
        LogInfo("vulkan: ray queries enabled (scratch alignment {})", scratch_alignment);
    } else if (want_ray_query) {
        LogInfo("vulkan: ray queries unavailable: {} missing", ray_query_missing);
    }
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = &features;
    device_info.queueCreateInfoCount = static_cast<uint32_t>(queue_infos.size());
    device_info.pQueueCreateInfos = queue_infos.data();
    device_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
    device_info.ppEnabledExtensionNames = device_extensions.data();
    const VkResult device_result = creator ? creator->CreateDevice(physical, device_info, device) : vkCreateDevice(physical, &device_info, nullptr, &device);
    if (!Check(device_result, "vkCreateDevice")) {
        return false;
    }
    volkLoadDevice(device);
    vkGetDeviceQueue(device, queue_family, 0, &queue);
    g_checkpoints = checkpoints && vkCmdSetCheckpointNV;

    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo allocator_info{};
    allocator_info.flags = (ray_query ? VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT : 0) |
                           (memory_budget ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0);
    allocator_info.vulkanApiVersion = VK_API_VERSION_1_3;
    allocator_info.physicalDevice = physical;
    allocator_info.device = device;
    allocator_info.instance = instance;
    allocator_info.pVulkanFunctions = &functions;
    if (!Check(vmaCreateAllocator(&allocator_info, &allocator), "vmaCreateAllocator")) {
        return false;
    }

    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family;
    if (!Check(vkCreateCommandPool(device, &pool_info, nullptr, &upload_pool_), "vkCreateCommandPool")) {
        return false;
    }
    if (hooks) {
        hooks->DeviceCreated(*this);
    }
    return true;
}

void Context::Shutdown() {
    if (device) {
        vkDeviceWaitIdle(device);
        DestroySwapchain();
        if (upload_pool_) {
            vkDestroyCommandPool(device, upload_pool_, nullptr);
        }
        if (allocator) {
            vmaDestroyAllocator(allocator);
        }
        if (before_device_destroy) {
            before_device_destroy();
        }
        vkDestroyDevice(device, nullptr);
    }
    if (surface) {
        vkDestroySurfaceKHR(instance, surface, nullptr);
    }
    if (messenger) {
        vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
    }
    if (instance) {
        vkDestroyInstance(instance, nullptr);
    }
    device = VK_NULL_HANDLE;
    instance = VK_NULL_HANDLE;
}

bool Context::CreateSwapchain(uint32_t width, uint32_t height, bool vsync) {
    vkDeviceWaitIdle(device);
    vsync = vsync && !force_vsync_off;
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps);
    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, formats.data());
    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats) {
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }
    uint32_t mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &mode_count, nullptr);
    std::vector<VkPresentModeKHR> modes(mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &mode_count, modes.data());
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync) {
        const bool immediate = std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_IMMEDIATE_KHR) != modes.end();
        const bool mailbox = std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end();
        present_mode = immediate ? VK_PRESENT_MODE_IMMEDIATE_KHR : mailbox ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
    }
    LogInfo("vulkan: swapchain present mode {} (v-sync {})",
            present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : present_mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo",
            vsync ? "on" : "off");
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        extent.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        return false;
    }
    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) {
        image_count = std::min(image_count, caps.maxImageCount);
    }

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface;
    info.minImageCount = image_count;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = present_mode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = swapchain.handle;
    VkSwapchainKHR handle = VK_NULL_HANDLE;
    if (swapchain_hooks || swapchain_owner_) {
        DestroySwapchain();
        info.oldSwapchain = VK_NULL_HANDLE;
        const VkResult result =
            swapchain_hooks ? swapchain_hooks->CreateSwapchain(info, handle) : vkCreateSwapchainKHR(device, &info, nullptr, &handle);
        if (!Check(result, "vkCreateSwapchainKHR")) {
            return false;
        }
        swapchain_owner_ = swapchain_hooks;
    } else {
        if (!Check(vkCreateSwapchainKHR(device, &info, nullptr, &handle), "vkCreateSwapchainKHR")) {
            return false;
        }
        DestroySwapchain();
    }
    swapchain.handle = handle;
    swapchain.format = chosen.format;
    swapchain.extent = extent;
    swapchain.min_image_count = image_count;
    uint32_t count = 0;
    if (swapchain_owner_) {
        swapchain_owner_->SwapchainImages(handle, &count, nullptr);
        swapchain.images.resize(count);
        swapchain_owner_->SwapchainImages(handle, &count, swapchain.images.data());
    } else {
        vkGetSwapchainImagesKHR(device, handle, &count, nullptr);
        swapchain.images.resize(count);
        vkGetSwapchainImagesKHR(device, handle, &count, swapchain.images.data());
    }
    for (VkImage image : swapchain.images) {
        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = chosen.format;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view;
        vkCreateImageView(device, &view_info, nullptr, &view);
        swapchain.views.push_back(view);
        VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore semaphore;
        vkCreateSemaphore(device, &semaphore_info, nullptr, &semaphore);
        swapchain.render_finished.push_back(semaphore);
    }
    return true;
}

void Context::DestroySwapchain() {
    for (VkImageView view : swapchain.views) {
        vkDestroyImageView(device, view, nullptr);
    }
    for (VkSemaphore semaphore : swapchain.render_finished) {
        vkDestroySemaphore(device, semaphore, nullptr);
    }
    swapchain.views.clear();
    swapchain.render_finished.clear();
    swapchain.images.clear();
    if (swapchain.handle) {
        if (swapchain_owner_) {
            swapchain_owner_->DestroySwapchain(swapchain.handle);
        } else {
            vkDestroySwapchainKHR(device, swapchain.handle, nullptr);
        }
        swapchain.handle = VK_NULL_HANDLE;
    }
    swapchain_owner_ = nullptr;
}

void Context::CheckDeviceLost(VkResult result, const char* where) {
    if (result != VK_ERROR_DEVICE_LOST) {
        return;
    }
    LogError("vulkan: device lost at {}", where);
    if (device_fault && vkGetDeviceFaultInfoEXT) {
        VkDeviceFaultCountsEXT counts{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
        if (vkGetDeviceFaultInfoEXT(device, &counts, nullptr) == VK_SUCCESS) {
            std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
            std::vector<VkDeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
            VkDeviceFaultInfoEXT info{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
            info.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
            info.pVendorInfos = vendors.empty() ? nullptr : vendors.data();
            counts.vendorBinarySize = 0;
            vkGetDeviceFaultInfoEXT(device, &counts, &info);
            {
                LogError("vulkan: device fault: {}", info.description);
                for (const VkDeviceFaultAddressInfoEXT& a : addresses) {
                    LogError("vulkan: device fault address type {} at {:#x} (precision {:#x})", static_cast<int>(a.addressType),
                             a.reportedAddress, a.addressPrecision);
                }
                for (const VkDeviceFaultVendorInfoEXT& v : vendors) {
                    LogError("vulkan: device fault vendor {:#x} data {:#x}: {}", v.vendorFaultCode, v.vendorFaultData, v.description);
                }
            }
        }
    }
    if (checkpoints && vkGetQueueCheckpointDataNV) {
        uint32_t count = 0;
        vkGetQueueCheckpointDataNV(queue, &count, nullptr);
        std::vector<VkCheckpointDataNV> data(count, VkCheckpointDataNV{VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV});
        vkGetQueueCheckpointDataNV(queue, &count, data.data());
        for (uint32_t i = 0; i < count; ++i) {
            LogError("vulkan: last checkpoint at stage {:#x}: {}", static_cast<uint64_t>(data[i].stage),
                     data[i].pCheckpointMarker ? static_cast<const char*>(data[i].pCheckpointMarker) : "(none)");
        }
    }
    FatalError(std::string("the graphics device was lost (") + where +
                   "). The GPU driver stopped or reset the device: a driver timeout, an unstable overclock or a fault in a pass.",
               surface != VK_NULL_HANDLE);
}

VkResult Context::AcquireNextImage(VkSemaphore semaphore, uint32_t* index) {
    if (swapchain_owner_) {
        return swapchain_owner_->AcquireNextImage(swapchain.handle, semaphore, index);
    }
    return vkAcquireNextImageKHR(device, swapchain.handle, UINT64_MAX, semaphore, VK_NULL_HANDLE, index);
}

VkResult Context::QueuePresent(const VkPresentInfoKHR& info) {
    if (swapchain_owner_) {
        return swapchain_owner_->QueuePresent(queue, info);
    }
    return vkQueuePresentKHR(queue, &info);
}

bool Context::CreateImage(Image& out, VkFormat format, VkExtent3D extent, VkImageUsageFlags usage, uint32_t mip_levels,
                          uint32_t layers, VkImageAspectFlags aspect, bool cube) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = extent.depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = extent;
    info.mipLevels = mip_levels;
    info.arrayLayers = layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (!Check(vmaCreateImage(allocator, &info, &alloc, &out.image, &out.allocation, nullptr), "vmaCreateImage")) {
        return false;
    }
    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = out.image;
    view_info.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE
                              : (extent.depth > 1 ? VK_IMAGE_VIEW_TYPE_3D : (layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D));
    view_info.format = format;
    view_info.subresourceRange = {aspect, 0, mip_levels, 0, layers};
    out.format = format;
    out.extent = extent;
    out.mip_levels = mip_levels;
    out.layers = layers;
    out.usage = usage;
    return Check(vkCreateImageView(device, &view_info, nullptr, &out.view), "vkCreateImageView");
}

void Context::DestroyImage(Image& image) {
    if (image.view) {
        vkDestroyImageView(device, image.view, nullptr);
    }
    if (image.image) {
        vmaDestroyImage(allocator, image.image, image.allocation);
    }
    image = Image{};
}

bool Context::CreateBuffer(Buffer& out, VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    if (host_visible) {
        alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    VmaAllocationInfo result{};
    if (!Check(vmaCreateBuffer(allocator, &info, &alloc, &out.buffer, &out.allocation, &result), "vmaCreateBuffer")) {
        return false;
    }
    out.mapped = result.pMappedData;
    out.size = size;
    return true;
}

void Context::DestroyBuffer(Buffer& buffer) {
    if (buffer.buffer) {
        vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
    }
    buffer = Buffer{};
}

void Context::Submit(const std::function<void(VkCommandBuffer)>& record) {
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = upload_pool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &alloc, &cmd);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    record(cmd);
    vkEndCommandBuffer(cmd);
    VkCommandBufferSubmitInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmd_info.commandBuffer = cmd;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    vkCreateFence(device, &fence_info, nullptr, &fence);
    CheckDeviceLost(vkQueueSubmit2(queue, 1, &submit, fence), "one-time submit");
    CheckDeviceLost(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "one-time submit wait");
    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, upload_pool_, 1, &cmd);
}

bool Context::Upload(Buffer& dst, const void* data, VkDeviceSize size) {
    Buffer staging;
    if (!CreateBuffer(staging, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) {
        return false;
    }
    std::memcpy(staging.mapped, data, static_cast<size_t>(size));
    vmaFlushAllocation(allocator, staging.allocation, 0, size);
    Submit([&](VkCommandBuffer cmd) {
        VkBufferCopy region{0, 0, size};
        vkCmdCopyBuffer(cmd, staging.buffer, dst.buffer, 1, &region);
    });
    DestroyBuffer(staging);
    return true;
}

}
