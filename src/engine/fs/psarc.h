#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pt {

class Psarc {
public:
    Psarc() = default;
    Psarc(const Psarc&) = delete;
    Psarc& operator=(const Psarc&) = delete;
    ~Psarc();

    bool Open(const std::filesystem::path& path);
    const std::vector<std::string>& Names() const { return names_; }
    bool Contains(std::string_view name) const;
    std::optional<std::vector<uint8_t>> Read(std::string_view name) const;
    std::optional<uint64_t> Size(std::string_view name) const;

private:
    struct Entry {
        uint32_t first_block;
        uint64_t size;
        uint64_t offset;
    };

    std::optional<std::vector<uint8_t>> ReadEntry(size_t index) const;

    mutable std::mutex mutex_;
    mutable FILE* file_ = nullptr;
    uint32_t block_size_ = 0;
    std::vector<Entry> entries_;
    std::vector<uint32_t> block_sizes_;
    std::vector<std::string> names_;
    std::unordered_map<std::string, size_t> index_;
};

}
