#include "engine/render/pipeline_cache_store.h"

#include <chrono>
#include <unordered_map>

namespace pt::vk {
namespace {

std::mutex g_pipeline_mutex;
struct PipelineDeviceState {
    VkPipelineCache cache = VK_NULL_HANDLE;
    PipelineCreationStats stats;
};
std::unordered_map<VkDevice, PipelineDeviceState> g_pipeline_devices;

VkPipelineCache CacheFor(VkDevice device) {
    const auto it = g_pipeline_devices.find(device);
    return it == g_pipeline_devices.end() ? VK_NULL_HANDLE : it->second.cache;
}

}

void RegisterPipelineCache(VkDevice device, VkPipelineCache cache) {
    std::lock_guard lock(g_pipeline_mutex);
    g_pipeline_devices[device].cache = cache;
}

void UnregisterPipelineCache(VkDevice device) {
    std::lock_guard lock(g_pipeline_mutex);
    g_pipeline_devices.erase(device);
}

PipelineCreationStats PipelineStats(VkDevice device) {
    std::lock_guard lock(g_pipeline_mutex);
    const auto it = g_pipeline_devices.find(device);
    return it == g_pipeline_devices.end() ? PipelineCreationStats{} : it->second.stats;
}

std::mutex& PipelineCacheCreationMutex() { return g_pipeline_mutex; }

VkResult CreateGraphicsPipelinesCached(VkDevice device, uint32_t count, const VkGraphicsPipelineCreateInfo* infos,
                                      const VkAllocationCallbacks* allocator, VkPipeline* pipelines) {
    std::lock_guard lock(g_pipeline_mutex);
    const auto start = std::chrono::steady_clock::now();
    const VkResult result = vkCreateGraphicsPipelines(device, CacheFor(device), count, infos, allocator, pipelines);
    if (auto it = g_pipeline_devices.find(device); it != g_pipeline_devices.end()) {
        it->second.stats.graphics_count += count;
        it->second.stats.duration_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    }
    return result;
}

VkResult CreateComputePipelinesCached(VkDevice device, uint32_t count, const VkComputePipelineCreateInfo* infos,
                                     const VkAllocationCallbacks* allocator, VkPipeline* pipelines) {
    std::lock_guard lock(g_pipeline_mutex);
    const auto start = std::chrono::steady_clock::now();
    const VkResult result = vkCreateComputePipelines(device, CacheFor(device), count, infos, allocator, pipelines);
    if (auto it = g_pipeline_devices.find(device); it != g_pipeline_devices.end()) {
        it->second.stats.compute_count += count;
        it->second.stats.duration_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    }
    return result;
}

}
