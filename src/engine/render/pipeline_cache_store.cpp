#include "engine/render/pipeline_cache_store.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace pt::vk {
namespace {

constexpr size_t kHeaderSize = 48;
constexpr uint32_t kFileVersion = 1;
constexpr char kMagic[8] = {'P', 'T', 'V', 'K', 'P', 'C', '0', '1'};

std::atomic<uint64_t> g_temp_sequence{};

uint64_t ProcessId() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<uint64_t>(getpid());
#endif
}

void Put32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8));
}

uint32_t Get32(std::span<const uint8_t> bytes, size_t offset) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= uint32_t(bytes[offset + i]) << (i * 8);
    return value;
}

void Put64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8));
}

uint64_t Get64(std::span<const uint8_t> bytes, size_t offset) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= uint64_t(bytes[offset + i]) << (i * 8);
    return value;
}

bool SameIdentity(std::span<const uint8_t> file, const PipelineCacheIdentity& id) {
    if (Get32(file, 8) != kFileVersion || Get32(file, 12) != id.vendor_id || Get32(file, 16) != id.device_id ||
        Get32(file, 20) != id.driver_version) return false;
    return std::equal(id.pipeline_cache_uuid.begin(), id.pipeline_cache_uuid.end(), file.begin() + 24);
}

}

PipelineCacheIdentity PipelineCacheIdentityFor(const VkPhysicalDeviceProperties& properties) {
    PipelineCacheIdentity id;
    id.vendor_id = properties.vendorID;
    id.device_id = properties.deviceID;
    id.driver_version = properties.driverVersion;
    std::copy(std::begin(properties.pipelineCacheUUID), std::end(properties.pipelineCacheUUID), id.pipeline_cache_uuid.begin());
    return id;
}

std::filesystem::path PipelineCacheFilePath(const std::filesystem::path& directory, const PipelineCacheIdentity& id) {
    std::ostringstream name;
    name << "vulkan-pipelines-v1-" << std::hex << id.vendor_id << '-' << id.device_id << '-' << id.driver_version << '-';
    for (uint8_t byte : id.pipeline_cache_uuid) name << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
    return directory / (name.str() + ".bin");
}

bool EncodePipelineCacheFile(const PipelineCacheIdentity& id, std::span<const uint8_t> payload, std::vector<uint8_t>& file) {
    if (payload.size() > kMaxPipelineCacheBytes) return false;
    file.assign(kHeaderSize + payload.size(), 0);
    std::copy(std::begin(kMagic), std::end(kMagic), file.begin());
    Put32(file, 8, kFileVersion);
    Put32(file, 12, id.vendor_id);
    Put32(file, 16, id.device_id);
    Put32(file, 20, id.driver_version);
    std::copy(id.pipeline_cache_uuid.begin(), id.pipeline_cache_uuid.end(), file.begin() + 24);
    Put64(file, 40, payload.size());
    std::copy(payload.begin(), payload.end(), file.begin() + kHeaderSize);
    return true;
}

bool DecodePipelineCacheFile(const PipelineCacheIdentity& id, std::span<const uint8_t> file, std::vector<uint8_t>& payload) {
    if (file.size() < kHeaderSize || !std::equal(std::begin(kMagic), std::end(kMagic), file.begin()) || !SameIdentity(file, id)) return false;
    const uint64_t payload_size = Get64(file, 40);
    if (payload_size > kMaxPipelineCacheBytes || payload_size != file.size() - kHeaderSize) return false;
    payload.assign(file.begin() + kHeaderSize, file.end());
    return true;
}

bool ReadPipelineCacheFile(const std::filesystem::path& path, const PipelineCacheIdentity& id, std::vector<uint8_t>& payload,
                           std::string& status) {
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        status = ec == std::errc::no_such_file_or_directory ? "cache file is absent" : "cannot inspect cache file: " + ec.message();
        return false;
    }
    if (size < kHeaderSize || size > kHeaderSize + kMaxPipelineCacheBytes) {
        status = "cache file size is invalid";
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if (!input || !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        status = "cannot read cache file";
        return false;
    }
    if (!DecodePipelineCacheFile(id, bytes, payload)) {
        status = "cache header or device identity does not match";
        return false;
    }
    status = "loaded";
    return true;
}

bool WritePipelineCacheFileAtomic(const std::filesystem::path& path, const PipelineCacheIdentity& id,
                                  std::span<const uint8_t> payload, std::string& status) {
    std::vector<uint8_t> bytes;
    if (!EncodePipelineCacheFile(id, payload, bytes)) {
        status = "cache exceeds 64 MiB limit";
        return false;
    }
    std::error_code ec;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        status = "cannot create cache directory: " + ec.message();
        return false;
    }
    auto temporary = path;
    temporary += ".tmp-" + std::to_string(ProcessId()) + "-" +
                 std::to_string(g_temp_sequence.fetch_add(1, std::memory_order_relaxed));
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output || !output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
            status = "cannot write temporary cache file";
            output.close();
            std::filesystem::remove(temporary, ec);
            return false;
        }
        output.flush();
        if (!output) {
            status = "cannot flush temporary cache file";
            output.close();
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        status = "cannot atomically replace cache file (Windows error " + std::to_string(GetLastError()) + ")";
        std::filesystem::remove(temporary, ec);
        return false;
    }
#else
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        status = "cannot atomically replace cache file: " + ec.message();
        std::filesystem::remove(temporary, ec);
        return false;
    }
#endif
    status = "saved";
    return true;
}

}
