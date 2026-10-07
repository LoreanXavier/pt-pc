#include "engine/ui/uia.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace pt::ui {
namespace {

constexpr uint32_t kMagic = 0x0BFCA2D2;
constexpr size_t kTreeNodeSize = 0x30;
constexpr float kHalfScale = 128.0f;

uint32_t U32(std::span<const uint8_t> d, size_t at) {
    uint32_t v;
    std::memcpy(&v, d.data() + at, 4);
    return v;
}

int32_t I32(std::span<const uint8_t> d, size_t at) {
    int32_t v;
    std::memcpy(&v, d.data() + at, 4);
    return v;
}

uint32_t ReadBits(std::span<const uint8_t> d, size_t pos, int count) {
    const size_t start = pos >> 3;
    const size_t bytes = ((pos & 7) + count + 7) >> 3;
    uint64_t raw = 0;
    for (size_t i = 0; i < bytes && start + i < d.size(); ++i) {
        raw |= static_cast<uint64_t>(d[start + i]) << (8 * i);
    }
    return static_cast<uint32_t>((raw >> (pos & 7)) & ((1ull << count) - 1));
}

float HalfToFloat(uint32_t raw) {
    const uint32_t exponent = (raw >> 10) & 0x1F;
    const uint32_t bits = ((raw & 0x8000u) << 16) | ((raw & 0x3FFu) << 13) | (exponent ? (exponent + 112) << 23 : 0u);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

glm::vec4 ReadValue(std::span<const uint8_t> d, size_t pos, int bits, int components) {
    glm::vec4 v(0.0f);
    for (int i = 0; i < components; ++i) {
        const uint32_t raw = ReadBits(d, pos + static_cast<size_t>(i) * bits, bits);
        if (bits == 32) {
            std::memcpy(&v[i], &raw, 4);
        } else {
            v[i] = HalfToFloat(raw) * kHalfScale;
        }
    }
    return v;
}

}

bool UiaAnimation::ParseUnits(std::span<const uint8_t> d, size_t at, uint32_t node_hash) {
    if (at + 20 > d.size()) {
        return false;
    }
    const uint32_t unit_count = U32(d, at);
    const uint32_t frames = U32(d, at + 12);
    if (unit_count == 0 || unit_count > 4096 || at + 20 + unit_count * 4ull > d.size()) {
        return false;
    }
    frames_ = std::max(frames_, static_cast<int>(frames));
    UiaNode node;
    node.node = node_hash;
    for (uint32_t i = 0; i < unit_count; ++i) {
        const size_t u = at + U32(d, at + 20 + i * 4);
        if (u + 8 > d.size()) {
            return false;
        }
        const uint32_t unit_hash = U32(d, u);
        const uint8_t count = d[u + 4];
        const bool is_static = (d[u + 5] & 4) != 0;
        for (uint8_t t = 0; t < count; ++t) {
            const size_t e = u + 8 + t * 8ull;
            if (e + 8 > d.size()) {
                return false;
            }
            const int32_t data_off = I32(d, e);
            const int kind = d[e + 6] & 0xF;
            const int bits = d[e + 7];
            const int components = kind >= 1 && kind <= 4 ? kind : kind == 6 ? 3 : 0;
            if (data_off == 0 || components == 0 || (bits != 16 && bits != 32)) {
                continue;
            }
            const size_t size = static_cast<size_t>(components) * bits;
            size_t pos = static_cast<size_t>(static_cast<int64_t>(e) + data_off) * 8;
            if ((pos + size) / 8 > d.size()) {
                return false;
            }
            UiaTrack track;
            track.unit = unit_hash;
            track.components = components;
            track.frames.push_back(0);
            track.values.push_back(ReadValue(d, pos, bits, components));
            if (!is_static) {
                pos += size;
                int frame = 0;
                while (frame < static_cast<int>(frames) && (pos + 8 + size) / 8 <= d.size()) {
                    const uint32_t delta = ReadBits(d, pos, 8);
                    if (delta == 0) {
                        break;
                    }
                    frame += static_cast<int>(delta);
                    track.frames.push_back(frame);
                    track.values.push_back(ReadValue(d, pos + 8, bits, components));
                    pos += 8 + size;
                }
            }
            node.tracks.push_back(std::move(track));
        }
    }
    nodes_.push_back(std::move(node));
    return true;
}

bool UiaAnimation::Parse(std::span<const uint8_t> d, std::string* error) {
    auto fail = [&](std::string message) {
        if (error) {
            *error = std::move(message);
        }
        return false;
    };
    frames_ = 0;
    nodes_.clear();
    if (d.size() < 16 || U32(d, 0) != kMagic) {
        return fail("not a UIA file");
    }
    const size_t root = U32(d, 4);
    std::vector<std::pair<size_t, int>> stack{{root, 0}};
    int visited = 0;
    while (!stack.empty()) {
        auto [at, depth] = stack.back();
        stack.pop_back();
        while (true) {
            if (at + kTreeNodeSize > d.size() || ++visited > 4096 || depth > 32) {
                return fail(std::format("UIA tree out of range at {:#x}", at));
            }
            const uint32_t hash = U32(d, at);
            const uint32_t has_units = U32(d, at + 8);
            const int32_t data_off = I32(d, at + 12);
            const int32_t child = I32(d, at + 0x18);
            const int32_t next = I32(d, at + 0x20);
            if (has_units && data_off > 0 && !ParseUnits(d, at + data_off, hash)) {
                return fail(std::format("UIA units out of range at {:#x}", at));
            }
            if (child > 0) {
                stack.push_back({at + child, depth + 1});
            }
            if (next <= 0) {
                break;
            }
            at += next;
        }
    }
    return true;
}

glm::vec4 UiaAnimation::Sample(const UiaTrack& track, float frame) {
    if (track.values.empty()) {
        return glm::vec4(0.0f);
    }
    if (track.values.size() == 1 || frame <= static_cast<float>(track.frames.front())) {
        return track.values.front();
    }
    if (frame >= static_cast<float>(track.frames.back())) {
        return track.values.back();
    }
    const auto it = std::upper_bound(track.frames.begin(), track.frames.end(), frame, [](float f, int k) { return f < static_cast<float>(k); });
    const size_t hi = static_cast<size_t>(it - track.frames.begin());
    const size_t lo = hi - 1;
    const float span = static_cast<float>(track.frames[hi] - track.frames[lo]);
    const float t = span > 0.0f ? (frame - static_cast<float>(track.frames[lo])) / span : 1.0f;
    return track.values[lo] + (track.values[hi] - track.values[lo]) * t;
}

}
