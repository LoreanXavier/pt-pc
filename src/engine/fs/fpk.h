#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pt {

class FoxPackage {
public:
    struct Entry {
        std::string path;
        uint32_t offset = 0;
        uint32_t size = 0;
    };

    bool Load(std::string name, std::vector<uint8_t> data);

    const std::string& Name() const { return name_; }
    bool IsDataPackage() const { return is_data_package_; }
    const std::string& Platform() const { return platform_; }
    const std::vector<Entry>& Entries() const { return entries_; }
    const std::vector<std::string>& References() const { return references_; }

    const Entry* Find(std::string_view path) const;
    std::vector<uint8_t> Read(const Entry& entry) const;
    std::optional<std::vector<uint8_t>> Read(std::string_view path) const;

private:
    std::string name_;
    std::string platform_;
    bool is_data_package_ = false;
    std::vector<uint8_t> data_;
    std::vector<Entry> entries_;
    std::vector<std::string> references_;
    std::unordered_map<std::string, size_t> index_;
};

}
