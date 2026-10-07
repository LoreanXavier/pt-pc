#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pt::anim {

/* The demo clock is 59.94, not 60 (0xB13B80: frames = seconds x 59.94); using the exact ratio keeps long demos in sync with their audio. */
constexpr double kDemoFramesPerSecond = 60000.0 / 1001.0;
constexpr double kMotionFramesPerSecond = 60.0;
constexpr uint32_t kTicksPerFrame = 5;

enum TrackKind : uint8_t {
    kTrackRotation = 0,
    kTrackFloat1 = 1,
    kTrackFloat2 = 2,
    kTrackFloat3 = 3,
    kTrackFloat4 = 4,
    kTrackRotationSlerp = 5,
    kTrackRootTranslation = 6,
};

struct Key {
    uint32_t frame = 0;
    glm::vec4 value{0.0f};
};

int KindComponents(uint8_t kind);
bool IsRotationKind(uint8_t kind);
uint32_t KeyBits(uint8_t kind, uint32_t bits);

uint64_t ReadBits(std::span<const uint8_t> data, size_t bit_pos, uint32_t count);
float HalfToFloat(uint32_t raw);
glm::vec4 DecodeQuatKey(std::span<const uint8_t> data, size_t bit_pos, uint32_t bits);
glm::vec4 DecodeVectorKey(std::span<const uint8_t> data, size_t bit_pos, uint32_t bits, int components);

bool ReadKeys(std::span<const uint8_t> data, size_t byte_offset, uint8_t kind, uint32_t bits, bool is_static, uint32_t frames, uint32_t frame_base,
              std::vector<Key>& out);

glm::vec4 SlerpKey(const glm::vec4& a, const glm::vec4& b, float t);
glm::vec4 InterpolateKey(uint8_t kind, const glm::vec4& a, const glm::vec4& b, float t);
glm::vec4 SampleKeys(std::span<const Key> keys, uint8_t kind, double frame);

inline glm::quat ToQuat(const glm::vec4& v) { return glm::quat(v.w, v.x, v.y, v.z); }
inline glm::vec4 FromQuat(const glm::quat& q) { return glm::vec4(q.x, q.y, q.z, q.w); }
glm::quat SlerpShortest(const glm::quat& a, const glm::quat& b, float t);

}
