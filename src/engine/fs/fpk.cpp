#include "engine/fs/fpk.h"

#include <cstring>

#include "engine/core/log.h"
#include "engine/core/memory_status.h"
#include "engine/fs/fox_crypt.h"
#include "engine/fs/mods.h"

namespace pt {
namespace {

template <typename T>
T ReadLe(const std::vector<uint8_t>& data, size_t offset) {
    T value{};
    if (offset + sizeof(T) <= data.size()) {
        std::memcpy(&value, data.data() + offset, sizeof(T));
    }
    return value;
}

std::string ReadString(const std::vector<uint8_t>& data, size_t offset) {
    const uint32_t string_offset = ReadLe<uint32_t>(data, offset);
    const uint32_t string_length = ReadLe<uint32_t>(data, offset + 8);
    if (string_offset + size_t(string_length) > data.size()) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(data.data() + string_offset), string_length);
}

}

bool FoxPackage::Load(std::string name, std::vector<uint8_t> data) {
    name_ = std::move(name);
    data_ = fox::IsWrapped(data) ? fox::Unwrap(data) : std::move(data);
    if (data_.size() < 48 || std::memcmp(data_.data(), "foxfpk", 6) != 0) {
        LogError("fpk: {} is not a Fox package", name_);
        return false;
    }
    is_data_package_ = data_[6] == 'd';
    platform_.assign(reinterpret_cast<const char*>(data_.data() + 7), 3);
    const uint32_t file_count = ReadLe<uint32_t>(data_, 0x24);
    const uint32_t reference_count = ReadLe<uint32_t>(data_, 0x28);
    size_t pos = 48;
    entries_.clear();
    entries_.reserve(file_count);
    for (uint32_t i = 0; i < file_count; ++i, pos += 48) {
        Entry entry;
        entry.offset = ReadLe<uint32_t>(data_, pos);
        entry.size = ReadLe<uint32_t>(data_, pos + 8);
        entry.path = ReadString(data_, pos + 16);
        if (size_t(entry.offset) + entry.size > data_.size()) {
            LogError("fpk: {} entry {} at {} + {} bytes exceeds package size {}", name_, entry.path, entry.offset, entry.size,
                     data_.size());
            /* A truncated entry reads as empty rather than failing the package: a bad mod file then costs one asset, not the stage. */
            entry.offset = 0;
            entry.size = 0;
        }
        index_[entry.path] = entries_.size();
        entries_.push_back(std::move(entry));
    }
    references_.clear();
    for (uint32_t i = 0; i < reference_count; ++i, pos += 16) {
        references_.push_back(ReadString(data_, pos));
    }
    return true;
}

const FoxPackage::Entry* FoxPackage::Find(std::string_view path) const {
    auto it = index_.find(std::string(path));
    return it == index_.end() ? nullptr : &entries_[it->second];
}

std::vector<uint8_t> FoxPackage::Read(const Entry& entry) const {
    if (auto data = mods::ReadOverride(entry.path)) {
        return std::move(*data);
    }
    std::span<const uint8_t> raw(data_.data() + entry.offset, entry.size);
    try {
        std::vector<uint8_t> decrypted;
        if (fox::DecryptPackageEntry(raw, entry.path, decrypted)) {
            return decrypted;
        }
        return std::vector<uint8_t>(raw.begin(), raw.end());
    } catch (const std::bad_alloc&) {
        LogMemoryStatus(("fpk: no memory for " + entry.path).c_str(), entry.size);
        throw;
    }
}

std::optional<std::vector<uint8_t>> FoxPackage::Read(std::string_view path) const {
    const Entry* entry = Find(path);
    if (!entry) {
        return std::nullopt;
    }
    return Read(*entry);
}

}
