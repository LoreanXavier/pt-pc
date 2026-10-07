#include "engine/render/upscale/upscale.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <type_traits>

#include "engine/core/log.h"
#include "engine/render/upscale/frame_generation.h"
#include "engine/render/upscale/streamline.h"
#include "engine/render/upscale/upscale_platform.h"

namespace pt {
namespace {

struct KindInfo {
    const char* name;
    const char* key;
};

constexpr KindInfo kKinds[] = {{"Off", "off"},           {"AMD FSR 3", "fsr3"},          {"NVIDIA DLSS", "dlss"},
                               {"Intel XeSS", "xess"},    {"Spatial (test)", "spatial"}, {"AMD FSR 4", "fsr4"}};

constexpr const char* kDlssModels[] = {"auto", "k", "l", "m"};
constexpr const char* kFrameGens[] = {"off", "fsr3", "dlss"};

struct QualityInfo {
    const char* name;
    const char* key;
    float ratio;
};

constexpr QualityInfo kQualities[] = {{"Native AA", "native", 1.0f},       {"Quality", "quality", 1.5f},
                                      {"Balanced", "balanced", 1.7f},      {"Performance", "performance", 2.0f},
                                      {"Ultra performance", "ultra_performance", 3.0f}, {"Custom", "custom", 1.0f}};

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

float Halton(int32_t index, int32_t base) {
    float f = 1.0f;
    float result = 0.0f;
    for (int32_t i = index; i > 0;) {
        f /= static_cast<float>(base);
        result += f * static_cast<float>(i % base);
        i = static_cast<int32_t>(std::floor(static_cast<float>(i) / static_cast<float>(base)));
    }
    return result;
}

}

const char* UpscalerName(UpscalerKind kind) {
    const int i = static_cast<int>(kind);
    return i >= 0 && i < static_cast<int>(UpscalerKind::Count) ? kKinds[i].name : "?";
}

const char* UpscalerKey(UpscalerKind kind) {
    const int i = static_cast<int>(kind);
    return i >= 0 && i < static_cast<int>(UpscalerKind::Count) ? kKinds[i].key : "off";
}

const char* UpscaleQualityName(UpscaleQuality quality) {
    const int i = static_cast<int>(quality);
    return i >= 0 && i < static_cast<int>(UpscaleQuality::Count) ? kQualities[i].name : "?";
}

const char* UpscaleQualityKey(UpscaleQuality quality) {
    const int i = static_cast<int>(quality);
    return i >= 0 && i < static_cast<int>(UpscaleQuality::Count) ? kQualities[i].key : "quality";
}

bool ParseUpscaler(const std::string& text, UpscalerKind& out) {
    const std::string key = Lower(text);
    for (int i = 0; i < static_cast<int>(UpscalerKind::Count); ++i) {
        if (key == kKinds[i].key) {
            out = static_cast<UpscalerKind>(i);
            return true;
        }
    }
    if (key == "dlaa") {
        out = UpscalerKind::Dlss;
        return true;
    }
    if (key == "fsr" || key == "fsr31") {
        out = UpscalerKind::Fsr;
        return true;
    }
    return false;
}

const char* DlssModelKey(DlssModel model) {
    const int i = static_cast<int>(model);
    return i >= 0 && i < static_cast<int>(DlssModel::Count) ? kDlssModels[i] : "auto";
}

bool ParseDlssModel(const std::string& text, DlssModel& out) {
    std::string key = Lower(text);
    if (key.starts_with("preset_")) {
        key = key.substr(7);
    }
    for (int i = 0; i < static_cast<int>(DlssModel::Count); ++i) {
        if (key == kDlssModels[i]) {
            out = static_cast<DlssModel>(i);
            return true;
        }
    }
    if (key == "default") {
        out = DlssModel::Auto;
        return true;
    }
    return false;
}

const char* FrameGenKey(FrameGenKind kind) {
    const int i = static_cast<int>(kind);
    return i >= 0 && i < static_cast<int>(FrameGenKind::Count) ? kFrameGens[i] : "off";
}

bool ParseFrameGen(const std::string& text, FrameGenKind& out) {
    const std::string key = Lower(text);
    for (int i = 0; i < static_cast<int>(FrameGenKind::Count); ++i) {
        if (key == kFrameGens[i]) {
            out = static_cast<FrameGenKind>(i);
            return true;
        }
    }
    if (key == "1" || key == "true" || key == "yes" || key == "on" || key == "fsr") {
        out = FrameGenKind::Fsr;
        return true;
    }
    if (key == "0" || key == "false" || key == "no" || key.empty()) {
        out = FrameGenKind::Off;
        return true;
    }
    if (key == "dlssg" || key == "dlss_g") {
        out = FrameGenKind::Dlss;
        return true;
    }
    return false;
}

bool ParseUpscaleQuality(const std::string& text, UpscaleQuality& out) {
    const std::string key = Lower(text);
    for (int i = 0; i < static_cast<int>(UpscaleQuality::Count); ++i) {
        if (key == kQualities[i].key) {
            out = static_cast<UpscaleQuality>(i);
            return true;
        }
    }
    if (key == "native_aa" || key == "nativeaa" || key == "dlaa") {
        out = UpscaleQuality::NativeAA;
        return true;
    }
    return false;
}

float UpscaleRatio(UpscaleQuality quality, float custom_scale) {
    if (quality == UpscaleQuality::Custom) {
        return 1.0f / std::clamp(custom_scale, 0.25f, 1.0f);
    }
    const int i = static_cast<int>(quality);
    return i >= 0 && i < static_cast<int>(UpscaleQuality::Count) ? kQualities[i].ratio : 1.0f;
}

VkExtent2D UpscaleRenderExtent(VkExtent2D output, UpscaleQuality quality, float custom_scale) {
    const float ratio = UpscaleRatio(quality, custom_scale);
    const uint32_t width = static_cast<uint32_t>(std::lround(static_cast<double>(output.width) / ratio));
    const uint32_t height = static_cast<uint32_t>(std::lround(static_cast<double>(output.height) / ratio));
    return {std::clamp(width, 1u, output.width), std::clamp(height, 1u, output.height)};
}

int32_t JitterPhaseCount(uint32_t render_width, uint32_t output_width) {
    const float ratio = static_cast<float>(output_width) / static_cast<float>(std::max(render_width, 1u));
    return static_cast<int32_t>(8.0f * ratio * ratio);
}

glm::vec2 JitterOffset(int32_t index, int32_t phase_count) {
    const int32_t base = (index % std::max(phase_count, 1)) + 1;
    return glm::vec2(Halton(base, 2) - 0.5f, Halton(base, 3) - 0.5f);
}

bool UpscaleBackend::Supported(std::string& reason) {
    reason.clear();
    return true;
}

float UpscaleBackend::MipBias(float render_over_display) const {
    return std::log2(std::max(render_over_display, 1.0e-3f));
}

UpscaleHost& UpscaleHost::Get() {
    static UpscaleHost host;
    return host;
}

bool UpscaleHost::InstanceHas(const char* name) const {
    return std::any_of(instance_available_.begin(), instance_available_.end(),
                       [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
}

bool UpscaleHost::DeviceHas(const char* name) const {
    return std::any_of(device_available_.begin(), device_available_.end(),
                       [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
}

void UpscaleHost::InstanceExtensions(VkInstanceCreateInfo&, std::vector<const char*>& extensions) {
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    instance_available_.resize(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, instance_available_.data());
    std::vector<std::string> wanted;
    DlssInstanceExtensions(startup_ == UpscalerKind::Dlss, wanted);
    for (const std::string& name : wanted) {
        if (!AddExtension(extensions, name.c_str(), false)) {
            LogInfo("upscale: instance extension {} unavailable", name);
        }
    }
    instance_enabled_.assign(extensions.begin(), extensions.end());
}

bool UpscaleHost::Enabled(const char* name, bool device) const {
    const std::vector<std::string>& list = device ? device_enabled_ : instance_enabled_;
    return std::find(list.begin(), list.end(), name) != list.end();
}

bool UpscaleHost::AddExtension(std::vector<const char*>& extensions, const char* name, bool device) {
    if (device ? !DeviceHas(name) : !InstanceHas(name)) {
        return false;
    }
    for (const char* existing : extensions) {
        if (std::strcmp(existing, name) == 0) {
            return true;
        }
    }
    names_.emplace_back(name);
    extensions.push_back(names_.back().c_str());
    return true;
}

namespace {

template <typename T>
VkBool32* Bools(T& s, size_t& count) {
    if constexpr (std::is_same_v<T, VkPhysicalDeviceFeatures>) {
        count = sizeof(T) / sizeof(VkBool32);
        return reinterpret_cast<VkBool32*>(&s);
    } else {
        constexpr size_t offset = offsetof(T, pNext) + sizeof(void*);
        count = (sizeof(T) - offset) / sizeof(VkBool32);
        return reinterpret_cast<VkBool32*>(reinterpret_cast<char*>(&s) + offset);
    }
}

template <typename T>
bool Merge(T& dst, T wanted, T supported) {
    size_t count = 0;
    VkBool32* d = Bools(dst, count);
    const VkBool32* w = Bools(wanted, count);
    const VkBool32* s = Bools(supported, count);
    bool any = false;
    for (size_t i = 0; i < count; ++i) {
        if (w[i] && s[i]) {
            d[i] = VK_TRUE;
            any = true;
        }
    }
    return any;
}

template <typename T>
bool Covers(T enabled, T wanted) {
    size_t count = 0;
    const VkBool32* e = Bools(enabled, count);
    const VkBool32* w = Bools(wanted, count);
    for (size_t i = 0; i < count; ++i) {
        if (w[i] && !e[i]) {
            return false;
        }
    }
    return true;
}

}

std::string UpscaleHost::MissingFeature(const DeviceFeatureSet& wanted) const {
    for (const std::string& name : wanted.extensions) {
        if (!Enabled(name.c_str(), true)) {
            return name;
        }
    }
    if (wanted.unknown) {
        return "a Vulkan feature this build does not enable";
    }
    if (!Covers(enabled_features_.core, wanted.core) || !Covers(enabled_features_.v11, wanted.v11) || !Covers(enabled_features_.v12, wanted.v12) ||
        !Covers(enabled_features_.v13, wanted.v13) || !Covers(enabled_features_.mutable_descriptor, wanted.mutable_descriptor)) {
        return "a Vulkan device feature";
    }
    return {};
}

void UpscaleHost::DeviceSetup(VkInstance instance, VkPhysicalDevice physical, std::vector<const char*>& extensions,
                              VkPhysicalDeviceFeatures2& features) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
    device_available_.resize(count);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, device_available_.data());
    AddExtension(extensions, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, true);
    AddExtension(extensions, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME, true);
    std::vector<std::string> wanted;
    DlssDeviceExtensions(startup_ == UpscalerKind::Dlss, instance, physical, wanted);
    for (const std::string& name : wanted) {
        if (!AddExtension(extensions, name.c_str(), true)) {
            LogInfo("upscale: device extension {} (DLSS) unavailable", name);
        }
    }
    DeviceFeatureSet xess;
    XessDeviceRequirements(startup_ == UpscalerKind::Xess, instance, physical, xess);
    for (const std::string& name : xess.extensions) {
        if (!AddExtension(extensions, name.c_str(), true)) {
            LogInfo("upscale: device extension {} (XeSS) unavailable", name);
        }
    }
    /* Streamline's device proxy enables VK_NV_low_latency2 without the VK_KHR_present_id it depends on; adding it here keeps the validation layer quiet. */
    const bool windowed = std::any_of(extensions.begin(), extensions.end(),
                                      [](const char* e) { return std::strcmp(e, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0; });
    const bool present_id = streamline::Active() && windowed && AddExtension(extensions, VK_KHR_PRESENT_ID_EXTENSION_NAME, true);
    device_enabled_.assign(extensions.begin(), extensions.end());
    const bool mutable_extension = Enabled(VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME, true);
    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT supported_mutable{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features supported13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    supported13.pNext = mutable_extension ? &supported_mutable : nullptr;
    VkPhysicalDeviceVulkan12Features supported12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    supported12.pNext = &supported13;
    VkPhysicalDeviceVulkan11Features supported11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    supported11.pNext = &supported12;
    VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    supported.pNext = &supported11;
    vkGetPhysicalDeviceFeatures2(physical, &supported);
    Merge(features.features, xess.core, supported.features);
    bool has11 = false;
    bool has_mutable = false;
    for (auto* s = reinterpret_cast<VkBaseOutStructure*>(&features); s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES) {
            Merge(*reinterpret_cast<VkPhysicalDeviceVulkan11Features*>(s), xess.v11, supported11);
            has11 = true;
        } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            auto* f12 = reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(s);
            f12->shaderFloat16 |= supported12.shaderFloat16;
            f12->shaderStorageBufferArrayNonUniformIndexing |= supported12.shaderStorageBufferArrayNonUniformIndexing;
            Merge(*f12, xess.v12, supported12);
        } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) {
            auto* f13 = reinterpret_cast<VkPhysicalDeviceVulkan13Features*>(s);
            f13->subgroupSizeControl |= supported13.subgroupSizeControl;
            f13->computeFullSubgroups |= supported13.computeFullSubgroups;
            if (streamline::Active()) {
                f13->privateData |= supported13.privateData;
            }
            Merge(*f13, xess.v13, supported13);
        } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT) {
            has_mutable = true;
        }
    }
    chain_v11_ = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    if (!has11 && Merge(chain_v11_, xess.v11, supported11)) {
        chain_v11_.pNext = features.pNext;
        features.pNext = &chain_v11_;
    }
    chain_present_id_ = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
    if (present_id) {
        chain_present_id_.presentId = VK_TRUE;
        chain_present_id_.pNext = features.pNext;
        features.pNext = &chain_present_id_;
    }
    chain_mutable_ = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT};
    if (!has_mutable && mutable_extension && Merge(chain_mutable_, xess.mutable_descriptor, supported_mutable)) {
        chain_mutable_.pNext = features.pNext;
        features.pNext = &chain_mutable_;
    }
    enabled_features_ = {};
    enabled_features_.core = features.features;
    for (auto* s = reinterpret_cast<VkBaseOutStructure*>(&features); s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES) {
            enabled_features_.v11 = *reinterpret_cast<VkPhysicalDeviceVulkan11Features*>(s);
        } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            enabled_features_.v12 = *reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(s);
        } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) {
            enabled_features_.v13 = *reinterpret_cast<VkPhysicalDeviceVulkan13Features*>(s);
        } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT) {
            enabled_features_.mutable_descriptor = *reinterpret_cast<VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT*>(s);
        }
    }
    enabled_features_.v11.pNext = nullptr;
    enabled_features_.v12.pNext = nullptr;
    enabled_features_.v13.pNext = nullptr;
    enabled_features_.mutable_descriptor.pNext = nullptr;
}

void UpscaleHost::DeviceQueues(VkPhysicalDevice physical, VkSurfaceKHR surface, uint32_t, std::vector<VkDeviceQueueCreateInfo>& queues) {
    for (int i = 0; i < 3; ++i) {
        fg_family_[i] = UINT32_MAX;
        fg_index_[i] = 0;
        fg_queue_[i] = VK_NULL_HANDLE;
    }
    if (!surface || queues.empty()) {
        return;
    }
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    std::vector<uint32_t> used(count, 0);
    for (const VkDeviceQueueCreateInfo& q : queues) {
        used[q.queueFamilyIndex] += q.queueCount;
    }
    auto take = [&](uint32_t f, int slot) {
        if (fg_family_[slot] != UINT32_MAX || used[f] >= families[f].queueCount) {
            return;
        }
        fg_family_[slot] = f;
        fg_index_[slot] = used[f]++;
    };
    std::vector<VkBool32> present(count, VK_FALSE);
    for (uint32_t f = 0; f < count; ++f) {
        vkGetPhysicalDeviceSurfaceSupportKHR(physical, f, surface, &present[f]);
    }
    const VkQueueFlags graphics_compute = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    for (uint32_t f = 0; f < count; ++f) {
        if ((families[f].queueFlags & graphics_compute) == graphics_compute && present[f]) {
            take(f, 0);
        }
    }
    for (uint32_t f = 0; f < count; ++f) {
        if ((families[f].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(families[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present[f]) {
            take(f, 0);
        }
    }
    for (uint32_t f = 0; f < count; ++f) {
        if ((families[f].queueFlags & VK_QUEUE_COMPUTE_BIT) && present[f]) {
            take(f, 0);
        }
    }
    for (uint32_t f = 0; f < count; ++f) {
        if (!(families[f].queueFlags & graphics_compute) && (families[f].queueFlags & VK_QUEUE_TRANSFER_BIT)) {
            take(f, 1);
        }
    }
    for (uint32_t f = 0; f < count; ++f) {
        if (!(families[f].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            take(f, 1);
        }
    }
    for (uint32_t f = 0; f < count; ++f) {
        if ((families[f].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(families[f].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            take(f, 2);
        }
    }
    uint32_t most = 0;
    for (uint32_t f = 0; f < count; ++f) {
        most = std::max(most, used[f]);
    }
    queue_priorities_.assign(most, 1.0f);
    for (uint32_t f = 0; f < count; ++f) {
        if (used[f] == 0) {
            continue;
        }
        auto it = std::find_if(queues.begin(), queues.end(), [&](const VkDeviceQueueCreateInfo& q) { return q.queueFamilyIndex == f; });
        if (it == queues.end()) {
            VkDeviceQueueCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            info.queueFamilyIndex = f;
            queues.push_back(info);
            it = queues.end() - 1;
        }
        it->queueCount = used[f];
        it->pQueuePriorities = queue_priorities_.data();
    }
}

void UpscaleHost::DeviceCreated(vk::Context& ctx) {
    ctx_ = &ctx;
    for (int i = 0; i < 3; ++i) {
        if (fg_family_[i] != UINT32_MAX) {
            vkGetDeviceQueue(ctx.device, fg_family_[i], fg_index_[i], &fg_queue_[i]);
        }
    }
    if (fg_family_[0] != UINT32_MAX) {
        LogInfo("frame generation queues: present {}.{}, image acquisition {}.{}, compute {}.{}", fg_family_[0], fg_index_[0], fg_family_[1],
                fg_index_[1], fg_family_[2], fg_index_[2]);
    }
    if (streamline::Active()) {
        streamline::DeviceCreated(ctx.instance, ctx.physical, ctx.device);
        ctx.before_device_destroy = [] { streamline::Shutdown(); };
    }
    CheckDlssFrameGen();
}

bool AmdGcnGpu(uint32_t vendor_id, uint32_t min_subgroup_size, uint32_t max_subgroup_size) {
    return vendor_id == kAmdVendor && min_subgroup_size == 64 && max_subgroup_size == 64;
}

bool FsrFrameGenHardware(VkPhysicalDevice physical, const VkPhysicalDeviceProperties& properties, std::string& reason) {
    if (properties.vendorID != kAmdVendor) {
        return true;
    }
    VkPhysicalDeviceVulkan13Properties properties13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
    VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties2.pNext = &properties13;
    vkGetPhysicalDeviceProperties2(physical, &properties2);
    if (!AmdGcnGpu(properties.vendorID, properties13.minSubgroupSize, properties13.maxSubgroupSize)) {
        return true;
    }
    reason = std::format("needs an AMD Radeon RX 5000 series or newer GPU ({} is GCN: subgroup size {} to {}, PCI device {:#06x})",
                         properties.deviceName, properties13.minSubgroupSize, properties13.maxSubgroupSize, properties.deviceID);
    if (std::getenv("PT_FSR_FG_ALLOW_UNSUPPORTED")) {
        LogWarn("frame generation: PT_FSR_FG_ALLOW_UNSUPPORTED set, skipping FSR 3 GPU check ({})", reason);
        reason.clear();
        return true;
    }
    return false;
}

void UpscaleHost::CheckDlssFrameGen() {
    dlss_fg_ = {};
    dlss_fg_.checked = true;
    uint32_t unsupported = 0;
    std::string detail;
    const bool queried = ctx_->properties.vendorID == 0x10DE && DlssFrameGenRequirements(ctx_->instance, ctx_->physical, unsupported, detail);
    if (ctx_->properties.vendorID == 0x10DE && !queried) {
        constexpr uint32_t kFirstAda = 0x2600;
        unsupported = ctx_->properties.deviceID < kFirstAda ? 4u : 0u;
        detail = std::format("{}; PCI device {:#06x}, {} Ada (0x2600)", detail, ctx_->properties.deviceID,
                             ctx_->properties.deviceID < kFirstAda ? "before" : "from");
    }
    if (std::getenv("PT_DLSSG_ALLOW_UNSUPPORTED") && ctx_->properties.vendorID == 0x10DE && (unsupported & 6u)) {
        LogWarn("frame generation: PT_DLSSG_ALLOW_UNSUPPORTED set, skipping DLSS-G GPU and driver checks");
        unsupported &= ~6u;
    }
#ifdef _WIN32
    /* RTX30MFG-Unlock (RTX40MFGCore.dll next to pt.exe, loaded by an ASI loader) lifts Streamline's GPU check in memory on RTX 20
       and 30; with it in place the PCI gate steps aside and Streamline's own answer decides. */
    if (ctx_->properties.vendorID == 0x10DE && (unsupported & 4u)) {
        wchar_t module[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, module, MAX_PATH);
        std::error_code ec;
        if (n > 0 && n < MAX_PATH && std::filesystem::exists(std::filesystem::path(module).parent_path() / L"RTX40MFGCore.dll", ec)) {
            LogWarn("frame generation: RTX40MFGCore.dll next to pt.exe, the DLSS-G GPU check steps aside for the unlock");
            unsupported &= ~4u;
        }
    }
#endif
    if (ctx_->properties.vendorID != 0x10DE) {
        dlss_fg_.reason = "needs an NVIDIA GeForce RTX 40 series or newer GPU";
        dlss_fg_.note = "pc_note_dlssg_gpu";
    } else if (unsupported & 4u) {
        dlss_fg_.reason = std::format("needs an NVIDIA GeForce RTX 40 series or newer GPU ({})", detail);
        dlss_fg_.note = "pc_note_dlssg_gpu";
    } else if (unsupported & 2u) {
        dlss_fg_.reason = std::format("needs a newer NVIDIA driver ({})", detail);
        dlss_fg_.note = "pc_note_dlssg_driver";
    } else if (unsupported != 0) {
        dlss_fg_.reason = std::format("not supported on this system ({})", detail);
        dlss_fg_.note = "pc_note_dlssg_driver";
    } else {
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext = &id;
        vkGetPhysicalDeviceProperties2(ctx_->physical, &properties);
#ifdef _WIN32
        const int scheduling = id.deviceLUIDValid ? HardwareGpuScheduling(id.deviceLUID) : -1;
#else
        const int scheduling = -1;
#endif
        std::string streamline_reason;
        if (scheduling == 0) {
            dlss_fg_.reason = "needs hardware-accelerated GPU scheduling (Windows graphics settings)";
            dlss_fg_.note = "pc_note_dlssg_hags";
        } else if (dlss_fg_failed_) {
            dlss_fg_.reason = "could not start and was turned off";
            dlss_fg_.note = "pc_note_dlssg_failed";
        } else if (streamline::Active() && !streamline::FrameGenSupported(&streamline_reason)) {
            dlss_fg_.reason = std::format("Streamline: {}", streamline_reason);
            dlss_fg_.note = streamline_reason.find("scheduling") != std::string::npos ? "pc_note_dlssg_hags"
                            : streamline_reason.find("driver") != std::string::npos   ? "pc_note_dlssg_driver"
                                                                                      : "pc_note_dlssg_gpu";
        } else {
            dlss_fg_.hardware = true;
#if defined(PT_WITH_STREAMLINE)
            std::error_code ec;
            dlss_fg_.built = std::filesystem::exists(ExecutableDir() / L"sl.interposer.dll", ec) &&
                             std::filesystem::exists(ExecutableDir() / L"sl.dlss_g.dll", ec) &&
                             std::filesystem::exists(ExecutableDir() / L"nvngx_dlssg.dll", ec);
#endif
            dlss_fg_.reason = dlss_fg_.built ? std::string() : std::string("not in this build");
            dlss_fg_.note = dlss_fg_.built ? std::string() : std::string("pc_note_dlssg_missing");
        }
    }
    LogInfo("frame generation: NVIDIA DLSS Frame Generation {}{}", dlss_fg_.hardware && dlss_fg_.built ? "available" : "unavailable: ",
            dlss_fg_.hardware && dlss_fg_.built ? "" : dlss_fg_.reason);
}

void UpscaleHost::FrameTick() {
    if (frame_stats_ < 0) {
        frame_stats_ = std::getenv("PT_FRAME_STATS") ? 1 : 0;
    }
    if (!frame_stats_) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (last_tick_.time_since_epoch().count() != 0) {
        frame_intervals_.push_back(std::chrono::duration<float, std::milli>(now - last_tick_).count());
    }
    last_tick_ = now;
}

FrameGeneration* UpscaleHost::DlssFrameGenImpl() {
    if (!dlss_frame_gen_created_ && ctx_) {
        dlss_frame_gen_created_ = true;
        dlss_frame_gen_ = CreateDlssFrameGeneration(*ctx_);
    }
    return dlss_frame_gen_.get();
}

FrameGeneration* UpscaleHost::FrameGen() {
    if (!frame_gen_created_ && ctx_) {
        frame_gen_created_ = true;
        FrameGenQueues queues;
        queues.present = {fg_queue_[0], fg_family_[0], fg_index_[0]};
        queues.acquire = {fg_queue_[1], fg_family_[1], fg_index_[1]};
        queues.compute = {fg_queue_[2], fg_family_[2], fg_index_[2]};
        frame_gen_ = CreateFrameGeneration(*ctx_, queues);
    }
    return frame_gen_.get();
}

void UpscaleHost::Shutdown() {
    if (frame_intervals_.size() > 120) {
        std::vector<float> sorted(frame_intervals_.begin() + 60, frame_intervals_.end());
        std::sort(sorted.begin(), sorted.end());
        double sum = 0.0;
        for (float v : sorted) {
            sum += v;
        }
        const double mean = sum / static_cast<double>(sorted.size());
        LogInfo("frame pacing: {} frames, interval mean {:.2f} ms ({:.1f} fps), median {:.2f} ms, p99 {:.2f} ms, max {:.2f} ms", sorted.size(), mean,
                1000.0 / mean, sorted[sorted.size() / 2], sorted[sorted.size() * 99 / 100], sorted.back());
    }
    frame_intervals_.clear();
    if (frame_gen_) {
        frame_gen_->Shutdown();
        frame_gen_.reset();
    }
    frame_gen_created_ = false;
    if (dlss_frame_gen_) {
        dlss_frame_gen_->Shutdown();
        dlss_frame_gen_.reset();
    }
    dlss_frame_gen_created_ = false;
    for (auto& backend : backends_) {
        if (backend) {
            backend->Release();
            backend.reset();
        }
    }
    for (int i = 0; i < static_cast<int>(UpscalerKind::Count); ++i) {
        probed_[i] = false;
        available_[i] = false;
    }
    ctx_ = nullptr;
}

UpscaleBackend* UpscaleHost::Backend(UpscalerKind kind) {
    const int i = static_cast<int>(kind);
    if (i <= 0 || i >= static_cast<int>(UpscalerKind::Count) || !ctx_) {
        return nullptr;
    }
    std::string reason;
    if (!Available(kind, reason)) {
        return nullptr;
    }
    return backends_[i].get();
}

bool UpscaleHost::Available(UpscalerKind kind, std::string& reason, bool probe) {
    const int i = static_cast<int>(kind);
    if (i <= 0 || i >= static_cast<int>(UpscalerKind::Count)) {
        reason.clear();
        return i == 0;
    }
    if (kind == UpscalerKind::Spatial) {
        reason.clear();
        return true;
    }
    if (!ctx_) {
        reason = "no Vulkan device";
        return false;
    }
    if (!probed_[i] && !backends_[i]) {
        switch (kind) {
        case UpscalerKind::Fsr: backends_[i] = CreateFsrBackend(*ctx_, 3); break;
        case UpscalerKind::Fsr4: backends_[i] = CreateFsrBackend(*ctx_, 4); break;
        case UpscalerKind::Dlss: backends_[i] = streamline::Active() ? CreateStreamlineDlssBackend(*ctx_) : CreateDlssBackend(*ctx_); break;
        case UpscalerKind::Xess: backends_[i] = CreateXessBackend(*ctx_); break;
        default: break;
        }
        if (!backends_[i]) {
            probed_[i] = true;
            reasons_[i] = "not built into this executable";
            LogInfo("upscale: {} unavailable: {}", UpscalerName(kind), reasons_[i]);
        }
    }
    if (!probed_[i] && !probe) {
        return backends_[i]->Supported(reason);
    }
    if (!probed_[i]) {
        probed_[i] = true;
        available_[i] = backends_[i]->Available(reasons_[i]);
        if (!available_[i]) {
            backends_[i].reset();
        }
        LogInfo("upscale: {} {}{}", UpscalerName(kind), available_[i] ? "available" : "unavailable: ", available_[i] ? "" : reasons_[i]);
    }
    reason = reasons_[i];
    return available_[i];
}

}
