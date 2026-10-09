#include "engine/anim/anim_codec.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pt::anim {

int KindComponents(uint8_t kind) {
    switch (kind) {
    case kTrackFloat1: return 1;
    case kTrackFloat2: return 2;
    case kTrackFloat3:
    case kTrackRootTranslation: return 3;
    default: return 4;
    }
}

bool IsRotationKind(uint8_t kind) {
    return kind == kTrackRotation || kind == kTrackRotationSlerp;
}

uint32_t KeyBits(uint8_t kind, uint32_t bits) {
    return IsRotationKind(kind) ? 3 * bits + 3 : static_cast<uint32_t>(KindComponents(kind)) * bits;
}

uint64_t ReadBits(std::span<const uint8_t> data, size_t bit_pos, uint32_t count) {
    const size_t start = bit_pos >> 3;
    const uint32_t shift = static_cast<uint32_t>(bit_pos & 7);
    const size_t bytes = (shift + count + 7) >> 3;
    uint64_t raw = 0;
    for (size_t i = 0; i < bytes && i < 8; ++i) {
        if (start + i < data.size()) {
            raw |= static_cast<uint64_t>(data[start + i]) << (8 * i);
        }
    }
    const uint64_t mask = count >= 64 ? ~0ull : ((1ull << count) - 1);
    return (raw >> shift) & mask;
}

// 0xAD4610: exponent bias 8 (exponent << 13 + 0x3B800000)
/* Not IEEE half: the motion codec's 16-bit floats use an exponent bias of 8 (0xAD4610), so a standard conversion comes out 128 times too small. */
float HalfToFloat(uint32_t raw) {
    const uint32_t exponent = (raw >> 10) & 0x1F;
    const uint32_t bits = ((raw & 0x8000u) << 16) | ((raw & 0x3FFu) << 13) | (exponent ? (exponent + 119u) << 23 : 0u);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}

glm::vec4 DecodeQuatKey(std::span<const uint8_t> data, size_t bit_pos, uint32_t bits) {
    const double scale = 1.0 / static_cast<double>((1ull << bits) - 1);
    const double angle = static_cast<double>(ReadBits(data, bit_pos, bits));
    const double ax = static_cast<double>(ReadBits(data, bit_pos + bits, bits)) * scale;
    const double ay = static_cast<double>(ReadBits(data, bit_pos + 2 * bits, bits)) * scale;
    const double az = 1.0 - ax - ay;
    const uint64_t signs = ReadBits(data, bit_pos + 3 * bits, 3);
    const double length2 = ax * ax + ay * ay + az * az;
    const double length = length2 > 1.1754944e-38 ? std::sqrt(length2) : 1.0;
    const double half = angle * scale * 3.14159265358979323846 * 0.5;
    const double k = std::sin(half) / length;
    return glm::vec4(static_cast<float>(ax * ((signs & 1) ? -k : k)), static_cast<float>(ay * ((signs & 2) ? -k : k)),
                     static_cast<float>(az * ((signs & 4) ? -k : k)), static_cast<float>(std::cos(half)));
}

glm::vec4 DecodeVectorKey(std::span<const uint8_t> data, size_t bit_pos, uint32_t bits, int components) {
    glm::vec4 out(0.0f);
    for (int i = 0; i < components && i < 4; ++i) {
        const uint64_t raw = ReadBits(data, bit_pos + static_cast<size_t>(i) * bits, bits);
        if (bits == 32) {
            const uint32_t word = static_cast<uint32_t>(raw);
            std::memcpy(&out[i], &word, 4);
        } else {
            out[i] = HalfToFloat(static_cast<uint32_t>(raw));
        }
    }
    return out;
}

bool ReadKeys(std::span<const uint8_t> data, size_t byte_offset, uint8_t kind, uint32_t bits, bool is_static, uint32_t frames, uint32_t frame_base,
              std::vector<Key>& out) {
    if (bits == 0 || bits > 32 || byte_offset >= data.size()) {
        return false;
    }
    const uint32_t size = KeyBits(kind, bits);
    const bool rotation = IsRotationKind(kind);
    const int components = KindComponents(kind);
    auto decode = [&](size_t pos) { return rotation ? DecodeQuatKey(data, pos, bits) : DecodeVectorKey(data, pos, bits, components); };
    size_t pos = byte_offset * 8;
    out.push_back({frame_base, decode(pos)});
    if (is_static) {
        return true;
    }
    uint32_t frame = 0;
    pos += size;
    const size_t limit = data.size() * 8;
    while (frame < frames && pos + 8 <= limit) {
        const uint32_t delta = static_cast<uint32_t>(ReadBits(data, pos, 8));
        if (delta == 0) {
            break;
        }
        frame += delta;
        out.push_back({frame_base + frame, decode(pos + 8)});
        pos += 8 + size;
    }
    return true;
}

glm::vec4 SlerpKey(const glm::vec4& a, const glm::vec4& b_in, float t) {
    glm::vec4 b = b_in;
    double dot = static_cast<double>(a.x) * b.x + static_cast<double>(a.y) * b.y + static_cast<double>(a.z) * b.z + static_cast<double>(a.w) * b.w;
    if (dot < 0.0) {
        b = -b;
        dot = -dot;
    }
    glm::dvec4 r;
    if (dot > 0.9995) {
        r = glm::dvec4(a) + (glm::dvec4(b) - glm::dvec4(a)) * static_cast<double>(t);
    } else {
        const double theta = std::acos(std::min(1.0, dot));
        const double s = std::sin(theta);
        const double wa = std::sin((1.0 - t) * theta) / s;
        const double wb = std::sin(static_cast<double>(t) * theta) / s;
        r = glm::dvec4(a) * wa + glm::dvec4(b) * wb;
    }
    double n = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
    if (n == 0.0) {
        n = 1.0;
    }
    return glm::vec4(r / n);
}

glm::vec4 InterpolateKey(uint8_t kind, const glm::vec4& a, const glm::vec4& b, float t) {
    if (IsRotationKind(kind)) {
        return SlerpKey(a, b, t);
    }
    return a + (b - a) * t;
}

glm::vec4 SampleKeys(std::span<const Key> keys, uint8_t kind, double frame) {
    if (keys.empty()) {
        return IsRotationKind(kind) ? glm::vec4(0.0f, 0.0f, 0.0f, 1.0f) : glm::vec4(0.0f);
    }
    if (keys.size() == 1) {
        return keys[0].value;
    }
    auto it = std::upper_bound(keys.begin(), keys.end(), frame, [](double f, const Key& k) { return f < static_cast<double>(k.frame); });
    size_t k = it == keys.begin() ? 0 : static_cast<size_t>(it - keys.begin()) - 1;
    k = std::min(k, keys.size() - 2);
    const Key& k0 = keys[k];
    const Key& k1 = keys[k + 1];
    double t = 0.0;
    if (k1.frame != k0.frame) {
        t = std::clamp((frame - static_cast<double>(k0.frame)) / (static_cast<double>(k1.frame) - static_cast<double>(k0.frame)), 0.0, 1.0);
    }
    return InterpolateKey(kind, k0.value, k1.value, static_cast<float>(t));
}

glm::quat SlerpShortest(const glm::quat& a, const glm::quat& b, float t) {
    return ToQuat(SlerpKey(FromQuat(a), FromQuat(b), t));
}

}
