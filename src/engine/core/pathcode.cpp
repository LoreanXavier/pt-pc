#include "engine/core/pathcode.h"

#include <array>
#include <string>

#include "engine/core/strcode.h"

namespace pt {
namespace {

constexpr uint64_t kSeed0 = 0x9AE16A3B2F90404Full;
constexpr uint64_t kKMul = 0x9DDFEA08EB382D69ull;
constexpr uint64_t kPathMask = 0x3FFFFFFFFFFFFull;
constexpr uint64_t kMetaFlag = 1ull << 50;
constexpr int kTypeShift = 51;
constexpr std::string_view kAssetsPrefix = "/Assets/";
constexpr std::array<std::string_view, 4> kKnownRoots = {"fox", "tpp", "sh", "mgo"};

uint64_t HashLen16(uint64_t u, uint64_t v) {
    uint64_t a = (u ^ v) * kKMul;
    a ^= a >> 47;
    uint64_t b = (v ^ a) * kKMul;
    b ^= b >> 47;
    return b * kKMul;
}

}

uint64_t SeededHash(std::string_view text) {
    uint64_t seed1 = 0;
    for (size_t i = 0; i < 8 && i < text.size(); ++i) {
        seed1 |= static_cast<uint64_t>(static_cast<uint8_t>(text[text.size() - 1 - i])) << (8 * i);
    }
    return HashLen16(CityHash64(text.data(), text.size()) - kSeed0, seed1);
}

uint32_t ExtensionType(std::string_view extension) {
    if (extension.starts_with('.')) {
        extension.remove_prefix(1);
    }
    return extension.empty() ? 0 : static_cast<uint32_t>(SeededHash(extension) & 0x1FFF);
}

uint64_t PathCode64(std::string_view path) {
    std::string normalized(path);
    for (char& c : normalized) {
        if (c == '\\') {
            c = '/';
        }
    }
    bool meta = true;
    if (normalized.starts_with(kAssetsPrefix)) {
        const std::string_view rest = std::string_view(normalized).substr(kAssetsPrefix.size());
        const size_t slash = rest.find('/');
        if (slash != std::string_view::npos) {
            const std::string_view root = rest.substr(0, slash);
            for (std::string_view known : kKnownRoots) {
                if (root == known) {
                    meta = false;
                }
            }
        }
    } else if (normalized.starts_with("./")) {
        normalized.erase(0, 2);
    }
    const size_t name_start = normalized.find_last_of('/') == std::string::npos ? 0 : normalized.find_last_of('/') + 1;
    const size_t dot = normalized.find('.', name_start);
    std::string_view stem = dot == std::string::npos ? std::string_view(normalized) : std::string_view(normalized).substr(0, dot);
    const std::string_view extension = dot == std::string::npos ? std::string_view() : std::string_view(normalized).substr(dot);
    if (stem.starts_with(kAssetsPrefix)) {
        stem.remove_prefix(kAssetsPrefix.size());
    }
    const uint64_t value = stem.empty() ? 0 : (SeededHash(stem) & kPathMask);
    return (static_cast<uint64_t>(ExtensionType(extension)) << kTypeShift) | (meta ? kMetaFlag : 0) | value;
}

uint64_t PathCodeWithExtension(uint64_t code, std::string_view extension) {
    return (static_cast<uint64_t>(ExtensionType(extension)) << kTypeShift) | (code & ((1ull << kTypeShift) - 1));
}

}
