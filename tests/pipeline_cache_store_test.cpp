#include "engine/render/pipeline_cache_store.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <vector>

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };

    pt::vk::PipelineCacheIdentity identity;
    identity.vendor_id = 0x10de;
    identity.device_id = 0x2684;
    identity.driver_version = 0x12345678;
    for (uint8_t i = 0; i < identity.pipeline_cache_uuid.size(); ++i) identity.pipeline_cache_uuid[i] = i;
    const std::array<uint8_t, 7> payload = {0, 1, 2, 3, 127, 254, 255};
    std::vector<uint8_t> file;
    check(pt::vk::EncodePipelineCacheFile(identity, payload, file), "valid cache payload is encoded with device identity");
    std::vector<uint8_t> decoded;
    check(pt::vk::DecodePipelineCacheFile(identity, file, decoded) &&
              std::equal(payload.begin(), payload.end(), decoded.begin(), decoded.end()),
          "matching device identity recovers the complete Vulkan payload");
    auto other_gpu = identity;
    ++other_gpu.device_id;
    check(!pt::vk::DecodePipelineCacheFile(other_gpu, file, decoded), "cache payload is rejected for a different GPU");
    other_gpu = identity;
    ++other_gpu.driver_version;
    check(!pt::vk::DecodePipelineCacheFile(other_gpu, file, decoded), "cache payload is rejected for a different driver");
    other_gpu = identity;
    other_gpu.pipeline_cache_uuid[0] ^= 0xff;
    check(!pt::vk::DecodePipelineCacheFile(other_gpu, file, decoded), "cache payload is rejected for a different cache UUID");
    auto corrupt = file;
    corrupt[0] ^= 0xff;
    check(!pt::vk::DecodePipelineCacheFile(identity, corrupt, decoded), "corrupt cache magic is rejected");
    file.pop_back();
    check(!pt::vk::DecodePipelineCacheFile(identity, file, decoded), "truncated cache payload is rejected");
    std::vector<uint8_t> oversized(pt::vk::kMaxPipelineCacheBytes + 1);
    check(!pt::vk::EncodePipelineCacheFile(identity, oversized, file), "cache payload above 64 MiB is rejected");

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto base = std::filesystem::temp_directory_path() / ("pt-pipeline-cache-test-" + std::to_string(stamp));
    const auto path_a = pt::vk::PipelineCacheFilePath(base, identity);
    auto other_device = identity;
    ++other_device.device_id;
    check(path_a != pt::vk::PipelineCacheFilePath(base, other_device), "cache file path is isolated by GPU identity");
    std::filesystem::remove_all(base);
    std::string status;
    check(pt::vk::WritePipelineCacheFileAtomic(path_a, identity, payload, status), "cache file saves through atomic replacement");
    std::vector<uint8_t> loaded;
    check(pt::vk::ReadPipelineCacheFile(path_a, identity, loaded, status) && loaded.size() == payload.size() &&
              std::equal(payload.begin(), payload.end(), loaded.begin(), loaded.end()),
          "cache file loads and revalidates its identity");
    const std::array<uint8_t, 3> replacement = {9, 8, 7};
    check(pt::vk::WritePipelineCacheFileAtomic(path_a, identity, replacement, status) &&
              pt::vk::ReadPipelineCacheFile(path_a, identity, loaded, status) &&
              std::equal(replacement.begin(), replacement.end(), loaded.begin(), loaded.end()),
          "atomic save replaces the previous cache without truncating it first");
    check(!pt::vk::ReadPipelineCacheFile(path_a, other_device, loaded, status), "cache file read rejects a GPU mismatch");
    std::filesystem::remove_all(base);
    return failures ? 1 : 0;
}
