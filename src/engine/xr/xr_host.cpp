#include "engine/xr/xr_host.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "engine/core/log.h"

#ifdef PT_WITH_OPENXR
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#define XR_NO_PROTOTYPES
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "engine/core/resource_path.h"
#endif

namespace pt::xr {

#ifndef PT_WITH_OPENXR

bool Available() { return false; }
struct Host::Impl {};
Host::Host() = default;
Host::~Host() = default;
bool Host::Init(const std::string&) {
    error_ = "this build has no OpenXR support";
    return false;
}
bool Host::Ready() const { return false; }
VkResult Host::CreateInstance(const VkInstanceCreateInfo& info, VkInstance& instance) { return vkCreateInstance(&info, nullptr, &instance); }
VkPhysicalDevice Host::PhysicalDevice(VkInstance) { return VK_NULL_HANDLE; }
VkResult Host::CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo& info, VkDevice& device) {
    return vkCreateDevice(physical, &info, nullptr, &device);
}
bool Host::StartSession(vk::Context&, float) { return false; }
void Host::Shutdown() {}
void Host::PollEvents() {}
bool Host::SessionRunning() const { return false; }
bool Host::ExitRequested() const { return false; }
bool Host::Focused() const { return false; }
bool Host::FocusLost() const { return false; }
bool Host::WaitFrame() { return false; }
bool Host::ShouldRender() const { return false; }
bool Host::BeginFrame() { return false; }
void Host::LocateViews() {}
void Host::SyncActions() {}
void Host::EndFrame(const FrameLayers&) {}
void Host::Haptic(int, float, float) {}
bool Host::Acquire(Swapchain&) { return false; }
void Host::Release(Swapchain&) {}

#else

bool Available() { return true; }

namespace {

#define PT_XR_FUNCTIONS(X)                    \
    X(xrDestroyInstance)                      \
    X(xrGetInstanceProperties)                \
    X(xrGetSystem)                            \
    X(xrGetSystemProperties)                  \
    X(xrPollEvent)                            \
    X(xrResultToString)                       \
    X(xrEnumerateViewConfigurationViews)      \
    X(xrEnumerateEnvironmentBlendModes)       \
    X(xrCreateSession)                        \
    X(xrDestroySession)                       \
    X(xrBeginSession)                         \
    X(xrEndSession)                           \
    X(xrRequestExitSession)                   \
    X(xrCreateReferenceSpace)                 \
    X(xrCreateActionSpace)                    \
    X(xrLocateSpace)                          \
    X(xrDestroySpace)                         \
    X(xrEnumerateSwapchainFormats)            \
    X(xrCreateSwapchain)                      \
    X(xrDestroySwapchain)                     \
    X(xrEnumerateSwapchainImages)             \
    X(xrAcquireSwapchainImage)                \
    X(xrWaitSwapchainImage)                   \
    X(xrReleaseSwapchainImage)                \
    X(xrWaitFrame)                            \
    X(xrBeginFrame)                           \
    X(xrEndFrame)                             \
    X(xrLocateViews)                          \
    X(xrStringToPath)                         \
    X(xrPathToString)                         \
    X(xrCreateActionSet)                      \
    X(xrDestroyActionSet)                     \
    X(xrCreateAction)                         \
    X(xrSuggestInteractionProfileBindings)    \
    X(xrAttachSessionActionSets)              \
    X(xrGetCurrentInteractionProfile)         \
    X(xrSyncActions)                          \
    X(xrGetActionStateBoolean)                \
    X(xrGetActionStateVector2f)               \
    X(xrGetActionStatePose)                   \
    X(xrApplyHapticFeedback)                  \
    X(xrGetVulkanGraphicsRequirements2KHR)    \
    X(xrCreateVulkanInstanceKHR)              \
    X(xrCreateVulkanDeviceKHR)                \
    X(xrGetVulkanGraphicsDevice2KHR)

glm::quat ToGlm(const XrQuaternionf& q) { return glm::quat(q.w, q.x, q.y, q.z); }
glm::vec3 ToGlm(const XrVector3f& v) { return glm::vec3(v.x, v.y, v.z); }
XrQuaternionf ToXr(const glm::quat& q) { return {q.x, q.y, q.z, q.w}; }
XrVector3f ToXr(const glm::vec3& v) { return {v.x, v.y, v.z}; }

}

struct Host::Impl {
#ifdef _WIN32
    HMODULE library = nullptr;
#else
    void* library = nullptr;
#endif
    PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr = nullptr;
    PFN_xrCreateInstance xrCreateInstance = nullptr;
#define PT_XR_MEMBER(name) PFN_##name name = nullptr;
    PT_XR_FUNCTIONS(PT_XR_MEMBER)
#undef PT_XR_MEMBER
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;
    bool exit_requested = false;
    bool ever_focused = false;
    XrSpace local = XR_NULL_HANDLE;
    XrSpace view = XR_NULL_HANDLE;
    XrViewConfigurationView views[2]{{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    XrActionSet action_set = XR_NULL_HANDLE;
    XrAction move = XR_NULL_HANDLE, turn = XR_NULL_HANDLE, interact = XR_NULL_HANDLE, back = XR_NULL_HANDLE, menu = XR_NULL_HANDLE,
             zoom = XR_NULL_HANDLE, gouge = XR_NULL_HANDLE, triangle = XR_NULL_HANDLE, settings = XR_NULL_HANDLE, aim = XR_NULL_HANDLE,
             haptic = XR_NULL_HANDLE;
    XrPath hands[2] = {XR_NULL_PATH, XR_NULL_PATH};
    XrSpace aim_spaces[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    vk::Context* ctx = nullptr;
    VkInstance vk_instance = VK_NULL_HANDLE;

    bool Ok(XrResult result, const char* what, std::string* error = nullptr) {
        if (XR_SUCCEEDED(result)) {
            return true;
        }
        char name[128] = {};
        if (instance && xrResultToString) {
            xrResultToString(instance, result, name);
        } else {
            const char* known = nullptr;
            switch (result) {
            case XR_ERROR_RUNTIME_UNAVAILABLE: known = "XR_ERROR_RUNTIME_UNAVAILABLE (no OpenXR runtime is installed or active)"; break;
            case XR_ERROR_RUNTIME_FAILURE: known = "XR_ERROR_RUNTIME_FAILURE (the OpenXR runtime failed)"; break;
            case XR_ERROR_API_VERSION_UNSUPPORTED: known = "XR_ERROR_API_VERSION_UNSUPPORTED"; break;
            case XR_ERROR_EXTENSION_NOT_PRESENT: known = "XR_ERROR_EXTENSION_NOT_PRESENT (the runtime has no XR_KHR_vulkan_enable2)"; break;
            case XR_ERROR_FORM_FACTOR_UNAVAILABLE: known = "XR_ERROR_FORM_FACTOR_UNAVAILABLE (no headset is connected)"; break;
            case XR_ERROR_FORM_FACTOR_UNSUPPORTED: known = "XR_ERROR_FORM_FACTOR_UNSUPPORTED (the runtime has no headset)"; break;
            case XR_ERROR_INITIALIZATION_FAILED: known = "XR_ERROR_INITIALIZATION_FAILED"; break;
            default: break;
            }
            if (known) {
                std::snprintf(name, sizeof(name), "%s", known);
            } else {
                std::snprintf(name, sizeof(name), "%d", static_cast<int>(result));
            }
        }
        LogError("vr: {} failed: {}", what, name);
        if (error) {
            *error = std::string(what) + ": " + name;
        }
        return false;
    }

    XrPath Path(const char* text) {
        XrPath path = XR_NULL_PATH;
        Ok(xrStringToPath(instance, text, &path), text);
        return path;
    }
};

Host::Host() : impl_(std::make_unique<Impl>()) {}

Host::~Host() { Shutdown(); }

bool Host::Ready() const { return impl_->instance != XR_NULL_HANDLE && impl_->system != XR_NULL_SYSTEM_ID; }

bool Host::Init(const std::string& application) {
    Impl& x = *impl_;
#ifdef _WIN32
    const std::filesystem::path path = ExecutableDir() / "openxr_loader.dll";
    x.library = LoadLibraryW(path.wstring().c_str());
    if (!x.library) {
        error_ = "openxr_loader.dll was not found next to pt.exe";
        LogError("vr: {}", error_);
        return false;
    }
    x.xrGetInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(x.library, "xrGetInstanceProcAddr"));
#else
    x.library = dlopen("libopenxr_loader.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!x.library) {
        error_ = "libopenxr_loader.so.1 was not found";
        LogError("vr: {}", error_);
        return false;
    }
    x.xrGetInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(dlsym(x.library, "xrGetInstanceProcAddr"));
#endif
    if (!x.xrGetInstanceProcAddr) {
        error_ = "the OpenXR loader has no xrGetInstanceProcAddr";
        LogError("vr: {}", error_);
        return false;
    }
    x.xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction*>(&x.xrCreateInstance));
    if (!x.xrCreateInstance) {
        error_ = "the OpenXR loader has no xrCreateInstance";
        LogError("vr: {}", error_);
        return false;
    }
    const char* extensions[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    std::snprintf(info.applicationInfo.applicationName, sizeof(info.applicationInfo.applicationName), "%s", application.c_str());
    info.applicationInfo.applicationVersion = 1;
    std::snprintf(info.applicationInfo.engineName, sizeof(info.applicationInfo.engineName), "pt-port");
    info.applicationInfo.engineVersion = 1;
    info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    info.enabledExtensionCount = 1;
    info.enabledExtensionNames = extensions;
    if (!x.Ok(x.xrCreateInstance(&info, &x.instance), "xrCreateInstance (is an OpenXR runtime installed and active?)", &error_)) {
        x.instance = XR_NULL_HANDLE;
        return false;
    }
#define PT_XR_LOAD(name) x.xrGetInstanceProcAddr(x.instance, #name, reinterpret_cast<PFN_xrVoidFunction*>(&x.name));
    PT_XR_FUNCTIONS(PT_XR_LOAD)
#undef PT_XR_LOAD
#define PT_XR_CHECK(name) \
    if (!x.name) {        \
        error_ = "the runtime has no " #name; \
        LogError("vr: {}", error_);           \
        return false;     \
    }
    PT_XR_FUNCTIONS(PT_XR_CHECK)
#undef PT_XR_CHECK
    XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
    if (x.Ok(x.xrGetInstanceProperties(x.instance, &properties), "xrGetInstanceProperties")) {
        runtime_name_ = std::format("{} {}.{}.{}", properties.runtimeName, XR_VERSION_MAJOR(properties.runtimeVersion),
                                    XR_VERSION_MINOR(properties.runtimeVersion), XR_VERSION_PATCH(properties.runtimeVersion));
    }
    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!x.Ok(x.xrGetSystem(x.instance, &system_info, &x.system), "xrGetSystem (is the headset connected?)", &error_)) {
        x.system = XR_NULL_SYSTEM_ID;
        return false;
    }
    XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES};
    x.Ok(x.xrGetSystemProperties(x.instance, x.system, &system_properties), "xrGetSystemProperties");
    uint32_t count = 0;
    if (!x.Ok(x.xrEnumerateViewConfigurationViews(x.instance, x.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, x.views),
              "xrEnumerateViewConfigurationViews", &error_) ||
        count != 2) {
        error_ = "the headset has no stereo view configuration";
        return false;
    }
    XrEnvironmentBlendMode modes[8];
    uint32_t mode_count = 0;
    x.xrEnumerateEnvironmentBlendModes(x.instance, x.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 8, &mode_count, modes);
    if (std::find(modes, modes + mode_count, XR_ENVIRONMENT_BLEND_MODE_OPAQUE) == modes + mode_count) {
        error_ = "the headset has no opaque blend mode";
        LogError("vr: {}", error_);
        return false;
    }
    XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    if (!x.Ok(x.xrGetVulkanGraphicsRequirements2KHR(x.instance, x.system, &requirements), "xrGetVulkanGraphicsRequirements2KHR", &error_)) {
        return false;
    }
    const XrVersion wanted = XR_MAKE_VERSION(1, 3, 0);
    if (XR_MAKE_VERSION(XR_VERSION_MAJOR(requirements.maxApiVersionSupported), XR_VERSION_MINOR(requirements.maxApiVersionSupported), 0) < wanted) {
        LogWarn("vr: runtime max Vulkan {}.{}, port needs 1.3", XR_VERSION_MAJOR(requirements.maxApiVersionSupported),
                XR_VERSION_MINOR(requirements.maxApiVersionSupported));
    }
    LogInfo("vr: runtime {}, system '{}' (vendor {:#x}), views {}x{} (max {}x{}), tracking orientation {} position {}", runtime_name_,
            system_properties.systemName, system_properties.vendorId, x.views[0].recommendedImageRectWidth, x.views[0].recommendedImageRectHeight,
            x.views[0].maxImageRectWidth, x.views[0].maxImageRectHeight, system_properties.trackingProperties.orientationTracking != 0,
            system_properties.trackingProperties.positionTracking != 0);
    return true;
}

VkResult Host::CreateInstance(const VkInstanceCreateInfo& info, VkInstance& instance) {
    Impl& x = *impl_;
    XrVulkanInstanceCreateInfoKHR create{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    create.systemId = x.system;
    create.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    create.vulkanCreateInfo = &info;
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    if (!x.Ok(x.xrCreateVulkanInstanceKHR(x.instance, &create, &instance, &result), "xrCreateVulkanInstanceKHR")) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    x.vk_instance = instance;
    return result;
}

VkPhysicalDevice Host::PhysicalDevice(VkInstance instance) {
    Impl& x = *impl_;
    XrVulkanGraphicsDeviceGetInfoKHR info{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    info.systemId = x.system;
    info.vulkanInstance = instance;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    x.Ok(x.xrGetVulkanGraphicsDevice2KHR(x.instance, &info, &physical), "xrGetVulkanGraphicsDevice2KHR");
    return physical;
}

VkResult Host::CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo& info, VkDevice& device) {
    Impl& x = *impl_;
    XrVulkanDeviceCreateInfoKHR create{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    create.systemId = x.system;
    create.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    create.vulkanPhysicalDevice = physical;
    create.vulkanCreateInfo = &info;
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    if (!x.Ok(x.xrCreateVulkanDeviceKHR(x.instance, &create, &device, &result), "xrCreateVulkanDeviceKHR")) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return result;
}

namespace {

bool CreateSwapchain(Host::Impl& x, vk::Context& ctx, const std::vector<int64_t>& formats, uint32_t width, uint32_t height, Swapchain& out,
                     const char* name) {
    /* sRGB 8-bit first: the eye images are sRGB encoded, and a UNORM swapchain would re-encode them and lose bits. */
    static constexpr VkFormat kPreferred[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R16G16B16A16_SFLOAT,
                                              VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
    VkFormat format = VK_FORMAT_UNDEFINED;
    for (VkFormat f : kPreferred) {
        if (std::find(formats.begin(), formats.end(), static_cast<int64_t>(f)) != formats.end()) {
            format = f;
            break;
        }
    }
    if (format == VK_FORMAT_UNDEFINED) {
        LogError("vr: no supported swapchain colour format");
        return false;
    }
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = format;
    info.sampleCount = 1;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    XrSwapchain handle = XR_NULL_HANDLE;
    if (!x.Ok(x.xrCreateSwapchain(x.session, &info, &handle), "xrCreateSwapchain")) {
        return false;
    }
    uint32_t count = 0;
    x.xrEnumerateSwapchainImages(handle, 0, &count, nullptr);
    std::vector<XrSwapchainImageVulkan2KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    if (!x.Ok(x.xrEnumerateSwapchainImages(handle, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),
              "xrEnumerateSwapchainImages")) {
        x.xrDestroySwapchain(handle);
        return false;
    }
    out.handle = reinterpret_cast<uint64_t>(handle);
    out.format = format;
    out.extent = {width, height};
    for (const XrSwapchainImageVulkan2KHR& image : images) {
        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = image.image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = format;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        vkCreateImageView(ctx.device, &view_info, nullptr, &view);
        out.images.push_back(image.image);
        out.views.push_back(view);
    }
    LogInfo("vr: {} swapchain {}x{}, format {}, {} images", name, width, height, static_cast<int>(format), count);
    return true;
}

}

bool Host::StartSession(vk::Context& ctx, float scale) {
    Impl& x = *impl_;
    x.ctx = &ctx;
    XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    binding.instance = ctx.instance;
    binding.physicalDevice = ctx.physical;
    binding.device = ctx.device;
    binding.queueFamilyIndex = ctx.queue_family;
    binding.queueIndex = 0;
    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
    info.next = &binding;
    info.systemId = x.system;
    if (!x.Ok(x.xrCreateSession(x.instance, &info, &x.session), "xrCreateSession", &error_)) {
        x.session = XR_NULL_HANDLE;
        return false;
    }
    XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space.poseInReferenceSpace.orientation.w = 1.0f;
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!x.Ok(x.xrCreateReferenceSpace(x.session, &space, &x.local), "xrCreateReferenceSpace LOCAL", &error_)) {
        return false;
    }
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!x.Ok(x.xrCreateReferenceSpace(x.session, &space, &x.view), "xrCreateReferenceSpace VIEW", &error_)) {
        return false;
    }

    XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::snprintf(set_info.actionSetName, sizeof(set_info.actionSetName), "gameplay");
    std::snprintf(set_info.localizedActionSetName, sizeof(set_info.localizedActionSetName), "Gameplay");
    if (!x.Ok(x.xrCreateActionSet(x.instance, &set_info, &x.action_set), "xrCreateActionSet", &error_)) {
        return false;
    }
    x.hands[0] = x.Path("/user/hand/left");
    x.hands[1] = x.Path("/user/hand/right");
    auto action = [&](XrAction& out, const char* name, const char* localized, XrActionType type, bool per_hand) {
        XrActionCreateInfo action_info{XR_TYPE_ACTION_CREATE_INFO};
        std::snprintf(action_info.actionName, sizeof(action_info.actionName), "%s", name);
        std::snprintf(action_info.localizedActionName, sizeof(action_info.localizedActionName), "%s", localized);
        action_info.actionType = type;
        if (per_hand) {
            action_info.countSubactionPaths = 2;
            action_info.subactionPaths = x.hands;
        }
        return x.Ok(x.xrCreateAction(x.action_set, &action_info, &out), name, &error_);
    };
    if (!action(x.move, "move", "Walk", XR_ACTION_TYPE_VECTOR2F_INPUT, false) ||
        !action(x.turn, "turn", "Turn", XR_ACTION_TYPE_VECTOR2F_INPUT, false) ||
        !action(x.interact, "interact", "Interact and confirm", XR_ACTION_TYPE_BOOLEAN_INPUT, false) ||
        !action(x.back, "back", "Back", XR_ACTION_TYPE_BOOLEAN_INPUT, false) ||
        !action(x.menu, "menu", "Pause menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false) ||
        !action(x.zoom, "zoom", "Zoom", XR_ACTION_TYPE_BOOLEAN_INPUT, false) ||
        !action(x.gouge, "gouge", "Scratch", XR_ACTION_TYPE_BOOLEAN_INPUT, false) ||
        !action(x.triangle, "triangle", "Triangle", XR_ACTION_TYPE_BOOLEAN_INPUT, false) ||
        !action(x.settings, "settings", "PC settings", XR_ACTION_TYPE_BOOLEAN_INPUT, false) ||
        !action(x.aim, "aim", "Hand", XR_ACTION_TYPE_POSE_INPUT, true) ||
        !action(x.haptic, "haptic", "Rumble", XR_ACTION_TYPE_VIBRATION_OUTPUT, true)) {
        return false;
    }
    struct Binding {
        XrAction* action;
        const char* path;
    };
    auto suggest = [&](const char* profile, std::initializer_list<Binding> list) {
        std::vector<XrActionSuggestedBinding> bindings;
        for (const Binding& b : list) {
            bindings.push_back({*b.action, x.Path(b.path)});
        }
        XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggested.interactionProfile = x.Path(profile);
        suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
        suggested.suggestedBindings = bindings.data();
        x.Ok(x.xrSuggestInteractionProfileBindings(x.instance, &suggested), profile);
    };
    suggest("/interaction_profiles/khr/simple_controller",
            {{&x.interact, "/user/hand/right/input/select/click"}, {&x.menu, "/user/hand/left/input/menu/click"},
             {&x.back, "/user/hand/left/input/select/click"}, {&x.aim, "/user/hand/left/input/aim/pose"},
             {&x.aim, "/user/hand/right/input/aim/pose"}, {&x.haptic, "/user/hand/left/output/haptic"},
             {&x.haptic, "/user/hand/right/output/haptic"}});
    suggest("/interaction_profiles/oculus/touch_controller",
            {{&x.move, "/user/hand/left/input/thumbstick"}, {&x.turn, "/user/hand/right/input/thumbstick"},
             {&x.interact, "/user/hand/right/input/a/click"}, {&x.back, "/user/hand/right/input/b/click"},
             {&x.gouge, "/user/hand/left/input/x/click"}, {&x.triangle, "/user/hand/left/input/y/click"},
             {&x.menu, "/user/hand/left/input/menu/click"}, {&x.zoom, "/user/hand/right/input/trigger/value"},
             {&x.settings, "/user/hand/left/input/squeeze/value"},
             {&x.aim, "/user/hand/left/input/aim/pose"}, {&x.aim, "/user/hand/right/input/aim/pose"},
             {&x.haptic, "/user/hand/left/output/haptic"}, {&x.haptic, "/user/hand/right/output/haptic"}});
    suggest("/interaction_profiles/valve/index_controller",
            {{&x.move, "/user/hand/left/input/thumbstick"}, {&x.turn, "/user/hand/right/input/thumbstick"},
             {&x.interact, "/user/hand/right/input/a/click"}, {&x.back, "/user/hand/right/input/b/click"},
             {&x.gouge, "/user/hand/left/input/a/click"}, {&x.triangle, "/user/hand/left/input/b/click"},
             {&x.menu, "/user/hand/left/input/thumbstick/click"}, {&x.zoom, "/user/hand/right/input/trigger/click"},
             {&x.settings, "/user/hand/left/input/squeeze/value"},
             {&x.aim, "/user/hand/left/input/aim/pose"}, {&x.aim, "/user/hand/right/input/aim/pose"},
             {&x.haptic, "/user/hand/left/output/haptic"}, {&x.haptic, "/user/hand/right/output/haptic"}});
    suggest("/interaction_profiles/htc/vive_controller",
            {{&x.move, "/user/hand/left/input/trackpad"}, {&x.turn, "/user/hand/right/input/trackpad"},
             {&x.interact, "/user/hand/right/input/trigger/click"}, {&x.back, "/user/hand/right/input/squeeze/click"},
             {&x.gouge, "/user/hand/left/input/trigger/click"}, {&x.triangle, "/user/hand/left/input/squeeze/click"},
             {&x.menu, "/user/hand/left/input/menu/click"}, {&x.zoom, "/user/hand/right/input/trackpad/click"},
             {&x.settings, "/user/hand/left/input/trackpad/click"},
             {&x.aim, "/user/hand/left/input/aim/pose"}, {&x.aim, "/user/hand/right/input/aim/pose"},
             {&x.haptic, "/user/hand/left/output/haptic"}, {&x.haptic, "/user/hand/right/output/haptic"}});
    suggest("/interaction_profiles/microsoft/motion_controller",
            {{&x.move, "/user/hand/left/input/thumbstick"}, {&x.turn, "/user/hand/right/input/thumbstick"},
             {&x.interact, "/user/hand/right/input/trigger/value"}, {&x.back, "/user/hand/right/input/squeeze/click"},
             {&x.gouge, "/user/hand/left/input/trigger/value"}, {&x.triangle, "/user/hand/left/input/squeeze/click"},
             {&x.menu, "/user/hand/left/input/menu/click"}, {&x.zoom, "/user/hand/right/input/thumbstick/click"},
             {&x.settings, "/user/hand/left/input/thumbstick/click"},
             {&x.aim, "/user/hand/left/input/aim/pose"}, {&x.aim, "/user/hand/right/input/aim/pose"},
             {&x.haptic, "/user/hand/left/output/haptic"}, {&x.haptic, "/user/hand/right/output/haptic"}});
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &x.action_set;
    if (!x.Ok(x.xrAttachSessionActionSets(x.session, &attach), "xrAttachSessionActionSets", &error_)) {
        return false;
    }
    for (int hand = 0; hand < 2; ++hand) {
        XrActionSpaceCreateInfo aim_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        aim_info.action = x.aim;
        aim_info.subactionPath = x.hands[hand];
        aim_info.poseInActionSpace.orientation.w = 1.0f;
        x.Ok(x.xrCreateActionSpace(x.session, &aim_info, &x.aim_spaces[hand]), "xrCreateActionSpace");
    }

    uint32_t format_count = 0;
    x.xrEnumerateSwapchainFormats(x.session, 0, &format_count, nullptr);
    std::vector<int64_t> formats(format_count);
    x.xrEnumerateSwapchainFormats(x.session, format_count, &format_count, formats.data());
    scale = std::clamp(scale, 0.5f, 2.0f);
    for (int eye = 0; eye < 2; ++eye) {
        const uint32_t width = std::clamp(static_cast<uint32_t>(std::lround(x.views[eye].recommendedImageRectWidth * scale)), 16u,
                                          x.views[eye].maxImageRectWidth);
        const uint32_t height = std::clamp(static_cast<uint32_t>(std::lround(x.views[eye].recommendedImageRectHeight * scale)), 16u,
                                           x.views[eye].maxImageRectHeight);
        if (!CreateSwapchain(x, ctx, formats, width, height, eye_swapchains_[eye], eye == 0 ? "left eye" : "right eye")) {
            error_ = "cannot create the eye swapchains";
            return false;
        }
    }
    if (!CreateSwapchain(x, ctx, formats, 1920, 1080, hud_swapchain_, "HUD") ||
        !CreateSwapchain(x, ctx, formats, 1920, 1080, screen_swapchain_, "virtual screen")) {
        error_ = "cannot create the screen swapchains";
        return false;
    }
    LogInfo("vr: session created");
    return true;
}

void Host::Shutdown() {
    Impl& x = *impl_;
    if (x.session && x.running) {
        if (frame_open_) {
            EndFrame({});
        }
        x.xrRequestExitSession(x.session);
        for (int i = 0; i < 90 && x.running; ++i) {
            PollEvents();
            if (x.running && WaitFrame() && BeginFrame()) {
                EndFrame({});
            }
        }
        if (x.running) {
            LogWarn("vr: session destroyed while still running");
        }
    }
    if (x.ctx && x.ctx->device) {
        vkDeviceWaitIdle(x.ctx->device);
    }
    for (Swapchain* sc : {&eye_swapchains_[0], &eye_swapchains_[1], &hud_swapchain_, &screen_swapchain_}) {
        for (VkImageView view : sc->views) {
            if (view && x.ctx) vkDestroyImageView(x.ctx->device, view, nullptr);
        }
        sc->views.clear();
        sc->images.clear();
        if (sc->handle && x.xrDestroySwapchain) x.xrDestroySwapchain(reinterpret_cast<XrSwapchain>(sc->handle));
        sc->handle = 0;
    }
    for (XrSpace& space : x.aim_spaces) {
        if (space) x.xrDestroySpace(space);
        space = XR_NULL_HANDLE;
    }
    if (x.view) x.xrDestroySpace(x.view);
    if (x.local) x.xrDestroySpace(x.local);
    x.view = x.local = XR_NULL_HANDLE;
    if (x.session) {
        x.xrDestroySession(x.session);
        x.session = XR_NULL_HANDLE;
        x.running = false;
    }
    if (x.action_set) x.xrDestroyActionSet(x.action_set);
    x.action_set = XR_NULL_HANDLE;
    if (x.instance) {
        LogInfo("vr: {} frames submitted", frames_submitted_);
        x.xrDestroyInstance(x.instance);
        x.instance = XR_NULL_HANDLE;
    }
    /* The OpenXR loader stays loaded on purpose: the Vulkan instance it created may still be destroyed after this. */
    x.ctx = nullptr;
}

void Host::PollEvents() {
    Impl& x = *impl_;
    if (!x.instance) {
        return;
    }
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (x.xrPollEvent(x.instance, &event) == XR_SUCCESS) {
        switch (event.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const auto& changed = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            x.state = changed.state;
            x.ever_focused = x.ever_focused || changed.state == XR_SESSION_STATE_FOCUSED;
            LogInfo("vr: session state {}", static_cast<int>(changed.state));
            if (changed.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                x.running = x.Ok(x.xrBeginSession(x.session, &begin), "xrBeginSession");
            } else if (changed.state == XR_SESSION_STATE_STOPPING) {
                x.Ok(x.xrEndSession(x.session), "xrEndSession");
                x.running = false;
                frame_open_ = false;
            } else if (changed.state == XR_SESSION_STATE_EXITING || changed.state == XR_SESSION_STATE_LOSS_PENDING) {
                x.exit_requested = true;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            LogWarn("vr: instance loss pending");
            x.exit_requested = true;
            break;
        case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED: {
            XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
            for (int hand = 0; hand < 2; ++hand) {
                if (XR_SUCCEEDED(x.xrGetCurrentInteractionProfile(x.session, x.hands[hand], &state))) {
                    char name[XR_MAX_PATH_LENGTH] = "none";
                    uint32_t length = 0;
                    if (state.interactionProfile != XR_NULL_PATH) {
                        x.xrPathToString(x.instance, state.interactionProfile, sizeof(name), &length, name);
                    }
                    LogInfo("vr: {} hand: {}", hand == 0 ? "left" : "right", name);
                }
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
            LogInfo("vr: reference space recentred");
            break;
        default:
            break;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

bool Host::SessionRunning() const { return impl_->running; }
bool Host::ExitRequested() const { return impl_->exit_requested; }
bool Host::Focused() const { return impl_->state == XR_SESSION_STATE_FOCUSED; }
bool Host::FocusLost() const {
    return impl_->ever_focused && (impl_->state == XR_SESSION_STATE_VISIBLE || impl_->state == XR_SESSION_STATE_SYNCHRONIZED);
}
bool Host::ShouldRender() const { return should_render_; }

bool Host::WaitFrame() {
    Impl& x = *impl_;
    if (!x.running) {
        return false;
    }
    XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState state{XR_TYPE_FRAME_STATE};
    if (!x.Ok(x.xrWaitFrame(x.session, &wait, &state), "xrWaitFrame")) {
        return false;
    }
    display_time_ = state.predictedDisplayTime;
    display_period_ = std::clamp(static_cast<double>(state.predictedDisplayPeriod) * 1.0e-9, 1.0 / 240.0, 1.0 / 30.0);
    should_render_ = state.shouldRender == XR_TRUE;
    return true;
}

bool Host::BeginFrame() {
    Impl& x = *impl_;
    XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
    const XrResult result = x.xrBeginFrame(x.session, &begin);
    frame_open_ = XR_SUCCEEDED(result);
    if (!frame_open_) {
        x.Ok(result, "xrBeginFrame");
    }
    return frame_open_;
}

void Host::LocateViews() {
    Impl& x = *impl_;
    XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
    info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    info.displayTime = display_time_;
    info.space = x.local;
    XrViewState state{XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t count = 0;
    views_valid_ = false;
    if (!x.Ok(x.xrLocateViews(x.session, &info, &state, 2, &count, views), "xrLocateViews") || count != 2) {
        return;
    }
    views_valid_ = (state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
    if (!views_valid_) {
        return;
    }
    for (int i = 0; i < 2; ++i) {
        eyes_[i].orientation = glm::normalize(ToGlm(views[i].pose.orientation));
        eyes_[i].position = ToGlm(views[i].pose.position);
        const XrFovf& f = views[i].fov;
        eyes_[i].angles = glm::vec4(f.angleLeft, f.angleRight, f.angleUp, f.angleDown);
        eyes_[i].tangents = glm::vec4(std::tan(f.angleLeft), std::tan(f.angleRight), std::tan(f.angleUp), std::tan(f.angleDown));
    }
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    if (XR_SUCCEEDED(x.xrLocateSpace(x.view, x.local, display_time_, &head)) && (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        head_.orientation = glm::normalize(ToGlm(head.pose.orientation));
        head_.position = ToGlm(head.pose.position);
    } else {
        head_.orientation = glm::normalize(glm::slerp(eyes_[0].orientation, eyes_[1].orientation, 0.5f));
        head_.position = (eyes_[0].position + eyes_[1].position) * 0.5f;
    }
}

void Host::SyncActions() {
    Impl& x = *impl_;
    controllers_ = ControllerState{};
    if (!x.running || x.state != XR_SESSION_STATE_FOCUSED) {
        return;
    }
    XrActiveActionSet active{x.action_set, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (!x.Ok(x.xrSyncActions(x.session, &sync), "xrSyncActions")) {
        return;
    }
    auto vector2 = [&](XrAction action) {
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.action = action;
        XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(x.xrGetActionStateVector2f(x.session, &get, &state)) && state.isActive) {
            controllers_.active = true;
            return glm::vec2(state.currentState.x, state.currentState.y);
        }
        return glm::vec2(0.0f);
    };
    auto boolean = [&](XrAction action) {
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.action = action;
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_SUCCEEDED(x.xrGetActionStateBoolean(x.session, &get, &state)) && state.isActive) {
            controllers_.active = true;
            return state.currentState == XR_TRUE;
        }
        return false;
    };
    controllers_.move = vector2(x.move);
    controllers_.turn = vector2(x.turn);
    controllers_.interact = boolean(x.interact);
    controllers_.back = boolean(x.back);
    controllers_.menu = boolean(x.menu);
    controllers_.zoom = boolean(x.zoom);
    controllers_.gouge = boolean(x.gouge);
    controllers_.triangle = boolean(x.triangle);
    controllers_.settings = boolean(x.settings);
    for (int hand = 0; hand < 2; ++hand) {
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.action = x.aim;
        get.subactionPath = x.hands[hand];
        XrActionStatePose pose{XR_TYPE_ACTION_STATE_POSE};
        if (!XR_SUCCEEDED(x.xrGetActionStatePose(x.session, &get, &pose)) || !pose.isActive || !x.aim_spaces[hand]) {
            continue;
        }
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(x.xrLocateSpace(x.aim_spaces[hand], x.local, display_time_, &location)) &&
            (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) && (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
            controllers_.aim[hand].valid = true;
            controllers_.aim[hand].orientation = glm::normalize(ToGlm(location.pose.orientation));
            controllers_.aim[hand].position = ToGlm(location.pose.position);
        }
    }
}

bool Host::Acquire(Swapchain& swapchain) {
    Impl& x = *impl_;
    const auto handle = reinterpret_cast<XrSwapchain>(swapchain.handle);
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!x.Ok(x.xrAcquireSwapchainImage(handle, &acquire, &swapchain.index), "xrAcquireSwapchainImage")) {
        return false;
    }
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    if (!x.Ok(x.xrWaitSwapchainImage(handle, &wait), "xrWaitSwapchainImage")) {
        return false;
    }
    swapchain.acquired = true;
    return true;
}

void Host::Release(Swapchain& swapchain) {
    Impl& x = *impl_;
    if (!swapchain.acquired) {
        return;
    }
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    x.Ok(x.xrReleaseSwapchainImage(reinterpret_cast<XrSwapchain>(swapchain.handle), &release), "xrReleaseSwapchainImage");
    swapchain.acquired = false;
}

void Host::EndFrame(const FrameLayers& layers) {
    Impl& x = *impl_;
    if (!frame_open_) {
        return;
    }
    XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerQuad screen{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad hud{XR_TYPE_COMPOSITION_LAYER_QUAD};
    std::vector<const XrCompositionLayerBaseHeader*> list;
    if (should_render_ && layers.projection) {
        for (int i = 0; i < 2; ++i) {
            views[i].pose.orientation = ToXr(layers.eyes[i].orientation);
            views[i].pose.position = ToXr(layers.eyes[i].position);
            views[i].fov = {layers.eyes[i].angles.x, layers.eyes[i].angles.y, layers.eyes[i].angles.z, layers.eyes[i].angles.w};
            views[i].subImage.swapchain = reinterpret_cast<XrSwapchain>(eye_swapchains_[i].handle);
            views[i].subImage.imageRect = {{0, 0}, {static_cast<int32_t>(eye_swapchains_[i].extent.width), static_cast<int32_t>(eye_swapchains_[i].extent.height)}};
        }
        projection.space = x.local;
        projection.viewCount = 2;
        projection.views = views;
        list.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection));
    }
    auto quad = [&](XrCompositionLayerQuad& q, const Swapchain& sc, const glm::quat& orientation, const glm::vec3& position, const glm::vec2& size,
                    bool alpha) {
        q.layerFlags = alpha ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
        q.space = x.local;
        q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        q.subImage.swapchain = reinterpret_cast<XrSwapchain>(sc.handle);
        q.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(sc.extent.width), static_cast<int32_t>(sc.extent.height)}};
        q.pose.orientation = ToXr(glm::normalize(orientation));
        q.pose.position = ToXr(position);
        q.size = {size.x, size.y};
        list.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q));
    };
    if (should_render_ && layers.screen) {
        quad(screen, screen_swapchain_, layers.screen_orientation, layers.screen_position, layers.screen_size, false);
    }
    if (should_render_ && layers.hud) {
        quad(hud, hud_swapchain_, layers.hud_orientation, layers.hud_position, layers.hud_size, true);
    }
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = display_time_;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = static_cast<uint32_t>(list.size());
    end.layers = list.data();
    if (x.Ok(x.xrEndFrame(x.session, &end), "xrEndFrame") && !list.empty()) {
        ++frames_submitted_;
    }
    frame_open_ = false;
}

void Host::Haptic(int hand, float amplitude, float seconds) {
    Impl& x = *impl_;
    if (!x.running || x.state != XR_SESSION_STATE_FOCUSED || hand < 0 || hand > 1) {
        return;
    }
    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = std::clamp(amplitude, 0.0f, 1.0f);
    vibration.duration = static_cast<XrDuration>(seconds * 1.0e9);
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
    info.action = x.haptic;
    info.subactionPath = x.hands[hand];
    x.xrApplyHapticFeedback(x.session, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
}

#endif

}
