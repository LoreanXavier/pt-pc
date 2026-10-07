#include "engine/fs/fox_crypt.h"

#include <cctype>
#include <cstring>
#include <string>

#include "engine/core/strcode.h"

namespace pt::fox {

bool IsWrapped(std::span<const uint8_t> data) {
    if (data.size() < 8) {
        return false;
    }
    uint32_t magic;
    std::memcpy(&magic, data.data(), 4);
    return magic == kCryptMagicShort || magic == kCryptMagicLong;
}

std::vector<uint8_t> Unwrap(std::span<const uint8_t> data) {
    uint32_t magic;
    uint32_t key;
    std::memcpy(&magic, data.data(), 4);
    std::memcpy(&key, data.data() + 4, 4);
    const size_t header = magic == kCryptMagicShort ? 8 : 16;
    std::vector<uint8_t> out(data.begin() + header, data.end());
    const uint32_t step = 278u * key;
    uint32_t block_key = key | ((key ^ 25974u) << 16);
    const size_t words = out.size() / 4;
    for (size_t i = 0; i < words; ++i) {
        uint32_t word;
        std::memcpy(&word, out.data() + i * 4, 4);
        word ^= block_key;
        std::memcpy(out.data() + i * 4, &word, 4);
        block_key = step + 48828125u * block_key;
    }
    return out;
}

bool IsPackageEntryEncrypted(std::span<const uint8_t> data) {
    return !data.empty() && (data[0] == 0x1B || data[0] == 0x1C);
}

bool DecryptPackageEntry(std::span<const uint8_t> data, std::string_view file_name, std::vector<uint8_t>& out) {
    if (!IsPackageEntryEncrypted(data)) {
        return false;
    }
    std::string lower(file_name.substr(file_name.find_last_of('/') == std::string_view::npos ? 0 : file_name.find_last_of('/') + 1));
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    const uint64_t key64 = ~StrCode64(lower);
    uint8_t key[8];
    std::memcpy(key, &key64, 8);
    out.resize(data.size() - 1);
    for (size_t i = 0; i + 1 < data.size(); ++i) {
        key[i % 8] ^= data[i + 1];
        out[i] = key[i % 8];
    }
    if (out.empty() || out.back() != 0) {
        return false;
    }
    out.pop_back();
    return true;
}

}
