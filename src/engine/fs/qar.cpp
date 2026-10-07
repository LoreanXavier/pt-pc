#include "engine/fs/qar.h"

#include <cstdio>
#include <cstring>

#include "engine/core/log.h"
#include "engine/platform/os.h"

namespace pt {

QarArchive::~QarArchive() {
    if (file_) {
        std::fclose(file_);
    }
}

bool QarArchive::Open(const std::filesystem::path& path) {
    file_ = os::OpenFile(path, "rb");
    if (!file_) {
        LogError("qar: cannot open {}", path.string());
        return false;
    }
    os::SeekFile(file_, -0x24, SEEK_END);
    uint8_t footer[0x24];
    if (std::fread(footer, 1, sizeof(footer), file_) != sizeof(footer)) {
        return false;
    }
    uint32_t count;
    uint16_t magic;
    uint32_t table_offset16;
    std::memcpy(&count, footer + 0x10, 4);
    std::memcpy(&magic, footer + 0x16, 2);
    std::memcpy(&table_offset16, footer + 0x18, 4);
    if (magic != 0x7161) {
        LogError("qar: {} has no QAR footer", path.string());
        return false;
    }
    std::vector<uint8_t> table(size_t(count) * 16);
    os::SeekFile(file_, static_cast<long long>(table_offset16) << 4, SEEK_SET);
    if (std::fread(table.data(), 1, table.size(), file_) != table.size()) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t code;
        uint32_t offset16;
        uint32_t size;
        std::memcpy(&code, table.data() + i * 16, 8);
        std::memcpy(&offset16, table.data() + i * 16 + 8, 4);
        std::memcpy(&size, table.data() + i * 16 + 12, 4);
        entries_[code] = {static_cast<uint64_t>(offset16) << 4, size};
    }
    LogInfo("qar: {} entries in {}", entries_.size(), path.filename().string());
    return true;
}

std::optional<std::vector<uint8_t>> QarArchive::Read(uint64_t code) const {
    auto it = entries_.find(code);
    if (it == entries_.end()) {
        return std::nullopt;
    }
    std::vector<uint8_t> data(it->second.size);
    std::lock_guard lock(mutex_);
    os::SeekFile(file_, static_cast<long long>(it->second.offset), SEEK_SET);
    if (std::fread(data.data(), 1, data.size(), file_) != data.size()) {
        return std::nullopt;
    }
    return data;
}

}
