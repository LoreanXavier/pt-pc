#include "engine/ui/lang_file.h"

#include <cstring>
#include <format>

namespace pt::ui {
namespace {

uint32_t U32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

}

bool LangFile::Parse(std::span<const uint8_t> data, std::string* error) {
    entries_.clear();
    if (data.size() < 0x20 || std::memcmp(data.data(), "LANG", 4) != 0) {
        if (error) {
            *error = "not a LANG file";
        }
        return false;
    }
    if (std::memcmp(data.data() + 8, "LE", 2) != 0) {
        if (error) {
            *error = "only little endian LANG files are supported";
        }
        return false;
    }
    const uint32_t count = U32(data.data() + 12);
    const uint32_t table = U32(data.data() + 16);
    const uint32_t keys = U32(data.data() + 20);
    const uint32_t values = U32(data.data() + 24);
    auto c_string = [&](size_t at) -> std::string {
        if (at >= data.size()) {
            return {};
        }
        const auto* begin = reinterpret_cast<const char*>(data.data() + at);
        return std::string(begin, strnlen(begin, data.size() - at));
    };
    for (uint32_t i = 0; i < count; ++i) {
        const size_t at = table + static_cast<size_t>(i) * 8;
        if (at + 8 > data.size()) {
            if (error) {
                *error = std::format("LANG entry {} out of range", i);
            }
            return false;
        }
        const size_t key_at = keys + static_cast<size_t>(U32(data.data() + at));
        const size_t value_at = values + static_cast<size_t>(U32(data.data() + at + 4));
        if (value_at + 2 > data.size()) {
            continue;
        }
        LangEntry entry;
        std::memcpy(&entry.color, data.data() + value_at, 2);
        entry.text = c_string(value_at + 2);
        entries_[c_string(key_at)] = std::move(entry);
    }
    return true;
}

const LangEntry* LangFile::Find(std::string_view key) const {
    auto it = entries_.find(std::string(key));
    return it == entries_.end() ? nullptr : &it->second;
}

std::string LangFile::Text(std::string_view key) const {
    const LangEntry* entry = Find(key);
    return entry ? entry->text : std::string(key);
}

}
