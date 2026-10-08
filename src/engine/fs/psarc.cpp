#include "engine/fs/psarc.h"

#include <zlib.h>

#include <cstdio>
#include <cstring>

#include "engine/core/log.h"
#include "engine/platform/os.h"

namespace pt {
namespace {

uint32_t ReadBe32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint64_t ReadBe(const uint8_t* p, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) {
        v = (v << 8) | p[i];
    }
    return v;
}

}

Psarc::~Psarc() {
    if (file_) {
        std::fclose(file_);
    }
}

bool Psarc::Open(const std::filesystem::path& path) {
    file_ = os::OpenFile(path, "rb");
    if (!file_) {
        LogError("psarc: cannot open {}", pt::os::PathToUtf8(path));
        return false;
    }
    uint8_t header[32];
    if (std::fread(header, 1, sizeof(header), file_) != sizeof(header) || std::memcmp(header, "PSAR", 4) != 0) {
        LogError("psarc: bad header in {}", pt::os::PathToUtf8(path));
        return false;
    }
    if (std::memcmp(header + 8, "zlib", 4) != 0) {
        LogError("psarc: unsupported compression in {}", pt::os::PathToUtf8(path));
        return false;
    }
    const uint32_t toc_length = ReadBe32(header + 12);
    const uint32_t entry_size = ReadBe32(header + 16);
    const uint32_t entry_count = ReadBe32(header + 20);
    block_size_ = ReadBe32(header + 24);

    std::vector<uint8_t> toc(toc_length - sizeof(header));
    if (std::fread(toc.data(), 1, toc.size(), file_) != toc.size()) {
        return false;
    }
    entries_.resize(entry_count);
    for (uint32_t i = 0; i < entry_count; ++i) {
        const uint8_t* e = toc.data() + size_t(i) * entry_size;
        entries_[i] = {ReadBe32(e + 16), ReadBe(e + 20, 5), ReadBe(e + 25, 5)};
    }
    int width = 2;
    while ((uint64_t(1) << (8 * width)) < block_size_) {
        ++width;
    }
    const size_t table_start = size_t(entry_count) * entry_size;
    for (size_t pos = table_start; pos + width <= toc.size(); pos += width) {
        block_sizes_.push_back(static_cast<uint32_t>(ReadBe(toc.data() + pos, width)));
    }

    auto manifest = ReadEntry(0);
    if (!manifest) {
        return false;
    }
    names_.clear();
    names_.push_back("");
    std::string line;
    for (uint8_t c : *manifest) {
        if (c == '\n') {
            names_.push_back(line);
            line.clear();
        } else if (c != '\r') {
            line.push_back(static_cast<char>(c));
        }
    }
    if (!line.empty()) {
        names_.push_back(line);
    }
    for (size_t i = 1; i < names_.size() && i < entries_.size(); ++i) {
        index_[names_[i]] = i;
    }
    LogInfo("psarc: {} entries in {}", entries_.size() - 1, pt::os::PathToUtf8(path.filename()));
    return true;
}

bool Psarc::Contains(std::string_view name) const {
    return index_.contains(std::string(name));
}

std::optional<uint64_t> Psarc::Size(std::string_view name) const {
    auto it = index_.find(std::string(name));
    if (it == index_.end()) {
        return std::nullopt;
    }
    return entries_[it->second].size;
}

std::optional<std::vector<uint8_t>> Psarc::Read(std::string_view name) const {
    auto it = index_.find(std::string(name));
    if (it == index_.end()) {
        return std::nullopt;
    }
    return ReadEntry(it->second);
}

std::optional<std::vector<uint8_t>> Psarc::ReadEntry(size_t index) const {
    const Entry& entry = entries_[index];
    /* TOC sizes are 40-bit and nothing in P.T. comes near 1 GB, so anything larger is a corrupt table. */
    if (entry.size > (uint64_t(1) << 30)) {
        LogError("psarc: entry {} size {} out of range, bad TOC", index, entry.size);
        return std::nullopt;
    }
    std::vector<uint8_t> out;
    out.reserve(entry.size);
    std::vector<uint8_t> block;
    std::lock_guard lock(mutex_);
    if (os::SeekFile(file_, static_cast<long long>(entry.offset), SEEK_SET) != 0) {
        return std::nullopt;
    }
    uint32_t block_index = entry.first_block;
    while (out.size() < entry.size) {
        const uint32_t stored = block_sizes_[block_index] ? block_sizes_[block_index] : block_size_;
        block.resize(stored);
        if (std::fread(block.data(), 1, stored, file_) != stored) {
            return std::nullopt;
        }
        const uint64_t remaining = entry.size - out.size();
        if (stored == block_size_ && remaining >= block_size_) {
            out.insert(out.end(), block.begin(), block.end());
        } else {
            std::vector<uint8_t> inflated(block_size_);
            uLongf length = block_size_;
            if (uncompress(inflated.data(), &length, block.data(), stored) == Z_OK) {
                out.insert(out.end(), inflated.begin(), inflated.begin() + length);
            } else {
                out.insert(out.end(), block.begin(), block.end());
            }
        }
        ++block_index;
    }
    out.resize(entry.size);
    return out;
}

}
