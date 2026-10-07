#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace pt {

class QarArchive {
public:
    QarArchive() = default;
    QarArchive(const QarArchive&) = delete;
    QarArchive& operator=(const QarArchive&) = delete;
    ~QarArchive();

    bool Open(const std::filesystem::path& path);
    bool Contains(uint64_t code) const { return entries_.contains(code); }
    std::optional<std::vector<uint8_t>> Read(uint64_t code) const;
    size_t Count() const { return entries_.size(); }

private:
    struct Entry {
        uint64_t offset;
        uint32_t size;
    };
    mutable std::mutex mutex_;
    mutable FILE* file_ = nullptr;
    std::unordered_map<uint64_t, Entry> entries_;
};

}
