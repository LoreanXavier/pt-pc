#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace pt::ui {

struct LangEntry {
    std::string text;
    uint16_t color = 0;
};

class LangFile {
public:
    bool Parse(std::span<const uint8_t> data, std::string* error);
    const LangEntry* Find(std::string_view key) const;
    std::string Text(std::string_view key) const;
    const std::unordered_map<std::string, LangEntry>& Entries() const { return entries_; }

private:
    std::unordered_map<std::string, LangEntry> entries_;
};

}
