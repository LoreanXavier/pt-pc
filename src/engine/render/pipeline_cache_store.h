#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <mutex>
#include <vector>

#include "engine/render/vk.h"

namespace pt::vk {

constexpr size_t kMaxPipelineCacheBytes = 64u * 1024u * 1024u;

struct PipelineCacheIdentity {
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint32_t driver_version = 0;
    std::array<uint8_t, VK_UUID_SIZE> pipeline_cache_uuid{};
};

struct PipelineCreationStats {
    uint64_t graphics_count = 0;
    uint64_t compute_count = 0;
    uint64_t duration_ns = 0;
};

PipelineCacheIdentity PipelineCacheIdentityFor(const VkPhysicalDeviceProperties& properties);
std::filesystem::path PipelineCacheFilePath(const std::filesystem::path& directory, const PipelineCacheIdentity& identity);
bool EncodePipelineCacheFile(const PipelineCacheIdentity& identity, std::span<const uint8_t> payload, std::vector<uint8_t>& file);
bool DecodePipelineCacheFile(const PipelineCacheIdentity& identity, std::span<const uint8_t> file, std::vector<uint8_t>& payload);
bool ReadPipelineCacheFile(const std::filesystem::path& path, const PipelineCacheIdentity& identity, std::vector<uint8_t>& payload,
                           std::string& status);
bool WritePipelineCacheFileAtomic(const std::filesystem::path& path, const PipelineCacheIdentity& identity,
                                  std::span<const uint8_t> payload, std::string& status);

void RegisterPipelineCache(VkDevice device, VkPipelineCache cache);
void UnregisterPipelineCache(VkDevice device);
PipelineCreationStats PipelineStats(VkDevice device);
std::mutex& PipelineCacheCreationMutex();
VkResult CreateGraphicsPipelinesCached(VkDevice device, uint32_t count, const VkGraphicsPipelineCreateInfo* infos,
                                      const VkAllocationCallbacks* allocator, VkPipeline* pipelines);
VkResult CreateComputePipelinesCached(VkDevice device, uint32_t count, const VkComputePipelineCreateInfo* infos,
                                     const VkAllocationCallbacks* allocator, VkPipeline* pipelines);

}
