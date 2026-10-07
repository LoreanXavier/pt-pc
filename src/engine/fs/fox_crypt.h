#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace pt::fox {

constexpr uint32_t kCryptMagicShort = 0xA0F8EFE6;
constexpr uint32_t kCryptMagicLong = 0xE3F8EFE6;

bool IsWrapped(std::span<const uint8_t> data);
std::vector<uint8_t> Unwrap(std::span<const uint8_t> data);

bool IsPackageEntryEncrypted(std::span<const uint8_t> data);
bool DecryptPackageEntry(std::span<const uint8_t> data, std::string_view file_name, std::vector<uint8_t>& out);

}
