#include "engine/anim/help_bones.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>

namespace pt::anim {
namespace {

template <typename T>
T At(std::span<const uint8_t> data, size_t offset) {
    T value{};
    if (offset + sizeof(T) <= data.size()) {
        std::memcpy(&value, data.data() + offset, sizeof(T));
    }
    return value;
}

const glm::quat kIdentity(1.0f, 0.0f, 0.0f, 0.0f);
constexpr float kPi = 3.14159265f;
constexpr float kDegree = 0.017453294f;
constexpr float kDoubleDegrees = 114.59155f;

float Limit(float v, float lo, float hi) {
    if (v - lo < 0.0f) {
        v = lo;
    }
    if (hi - v < 0.0f) {
        v = hi;
    }
    return v;
}

glm::quat SlerpFromIdentity(const glm::quat& q, float t) {
    glm::quat from = kIdentity;
    float d = q.w;
    if (d < 0.0f) {
        from = -from;
        d = -d;
    }
    d = std::min(d, 1.0f);
    float w0 = 1.0f - t;
    float w1 = t;
    if (d < 0.999f) {
        const float theta = std::acos(d);
        const float s = std::sin(theta);
        w0 = std::sin((1.0f - t) * theta) / s;
        w1 = std::sin(t * theta) / s;
    }
    return glm::normalize(from * w0 + q * w1);
}

glm::quat Arc(const glm::vec3& a, const glm::vec3& b) {
    const float d = glm::dot(a, b);
    if (d + 1.0f <= 1e-5f) {
        glm::vec3 e(0.0f, 1.0f, 0.0f);
        float smallest = std::abs(a.y);
        if (std::abs(a.x) < smallest) {
            e = glm::vec3(1.0f, 0.0f, 0.0f);
            smallest = std::abs(a.x);
        }
        if (std::abs(a.z) <= smallest) {
            e = glm::vec3(0.0f, 0.0f, 1.0f);
        }
        const glm::vec3 c = glm::normalize(glm::cross(a, e));
        return glm::quat(0.0f, c.x, c.y, c.z);
    }
    const glm::vec3 s = a + b;
    if (glm::dot(s, s) < 1e-6f) {
        return kIdentity;
    }
    const float k = std::sqrt(2.0f + 2.0f * d);
    const glm::vec3 c = glm::cross(a, b) / k;
    return glm::quat(k * 0.5f, c.x, c.y, c.z);
}

glm::quat Twist(const glm::quat& q, const glm::vec3& axis) {
    return glm::conjugate(Arc(axis, q * axis)) * q;
}

glm::quat ScaledTwist(const glm::quat& twist, float weight) {
    return weight >= 0.0f ? SlerpFromIdentity(twist, weight) : glm::conjugate(SlerpFromIdentity(twist, -weight));
}

struct Swing {
    float theta = 0.0f;
    float x = 0.0f;
    float y = 0.0f;
};

Swing SwingOf(const glm::quat& q, const glm::vec3& a, const glm::vec3& b) {
    const glm::vec3 v = q * a;
    const float half = std::clamp(std::sqrt(std::max(0.0f, 2.0f * (glm::dot(a, v) + 1.0f))) * 0.5f, -1.0f, 1.0f);
    return {2.0f * std::acos(half), glm::dot(b, v), -glm::dot(glm::cross(a, b), v)};
}

float Toward(const Swing& s) {
    return 2.0f / kPi * std::atan2(std::abs(s.x), std::abs(s.y) + 1e-10f);
}

glm::quat AxisRotation(uint32_t axis, float angle) {
    const float s = std::sin(angle * 0.5f);
    const float c = std::cos(angle * 0.5f);
    return glm::quat(c, axis == 0 ? s : 0.0f, axis == 1 ? s : 0.0f, axis == 2 ? s : 0.0f);
}

}

bool HelpBonesEnabled() {
    static const bool enabled = [] {
        const char* off = std::getenv("PT_HELP_BONES_OFF");
        return !off || std::strcmp(off, "0") == 0;
    }();
    return enabled;
}

bool HelpBones::Parse(std::span<const uint8_t> data, std::string* error) {
    entries_.clear();
    auto fail = [&](std::string text) {
        if (error) {
            *error = std::move(text);
        }
        entries_.clear();
        return false;
    };
    if (data.size() < 0x10 || std::memcmp(data.data(), "FRDV", 4) != 0) {
        return fail("not an FRDV file");
    }
    const uint32_t count = At<uint32_t>(data, 8);
    if (0x10 + static_cast<size_t>(count) * 4 > data.size()) {
        return fail(std::format("{} entries do not fit in {} bytes", count, data.size()));
    }
    for (uint32_t i = 0; i < count; ++i) {
        const size_t o = At<uint32_t>(data, 0x10 + 4 * static_cast<size_t>(i));
        if (o + kEntrySize > data.size()) {
            return fail(std::format("entry {} at {:#x} is out of the file", i, o));
        }
        HelpBoneEntry e;
        e.type = At<uint16_t>(data, o);
        e.driven = At<int16_t>(data, o + 0x02);
        e.source = At<int16_t>(data, o + 0x04);
        e.parent = At<int16_t>(data, o + 0x08);
        e.reference = At<int16_t>(data, o + 0x0A);
        e.weight = At<float>(data, o + 0x10);
        e.min = At<float>(data, o + 0x18);
        e.max = At<float>(data, o + 0x1C);
        e.axis = At<uint32_t>(data, o + 0x20);
        e.slide = At<float>(data, o + 0x24);
        e.slide_min = At<float>(data, o + 0x2C);
        e.slide_max = At<float>(data, o + 0x30);
        e.slide_axis = At<uint32_t>(data, o + 0x34);
        e.a = glm::vec3(At<float>(data, o + 0x40), At<float>(data, o + 0x44), At<float>(data, o + 0x48));
        e.b = glm::vec3(At<float>(data, o + 0x50), At<float>(data, o + 0x54), At<float>(data, o + 0x58));
        entries_.push_back(e);
    }
    return true;
}

size_t HelpBones::UnknownEntries() const {
    return static_cast<size_t>(std::count_if(entries_.begin(), entries_.end(), [](const HelpBoneEntry& e) {
        return !(e.Is(HelpBoneType::SlideByAngle) || e.Is(HelpBoneType::Follow) || e.Is(HelpBoneType::Twist) ||
                 e.Is(HelpBoneType::SwingTurn) || e.Is(HelpBoneType::TwistSlide) || e.Is(HelpBoneType::SwingTurnSlide));
    }));
}

void HelpBones::Evaluate(std::span<const glm::vec3> bind_local, std::span<glm::quat> rotation, std::span<glm::vec3> position) const {
    const size_t n = std::min({bind_local.size(), rotation.size(), position.size()});
    auto valid = [n](int16_t bone) { return bone >= 0 && static_cast<size_t>(bone) < n; };
    for (const HelpBoneEntry& e : entries_) {
        if (!valid(e.driven) || !valid(e.source) || !valid(e.parent) || (e.reference != -1 && !valid(e.reference))) {
            continue;
        }
        const size_t driven = static_cast<size_t>(e.driven);
        const glm::quat parent = rotation[static_cast<size_t>(e.parent)];
        glm::quat q = rotation[static_cast<size_t>(e.source)];
        if (e.reference != -1) {
            q = glm::conjugate(rotation[static_cast<size_t>(e.reference)]) * q;
        }
        glm::vec3 offset = bind_local[driven];
        glm::quat result = parent;
        auto slide = [&]() {
            const float t = Limit(e.slide * glm::dot(e.b, q * e.a), e.slide_min, e.slide_max);
            if ((e.slide_axis & 3) < 3) {
                offset[e.slide_axis & 3] = t * 0.1f;
            }
        };
        switch (static_cast<HelpBoneType>(e.type)) {
        case HelpBoneType::SlideByAngle: {
            const float degrees = Limit(e.weight * kDoubleDegrees * std::acos(std::min(std::abs(q.w), 1.0f)), e.min, e.max);
            if ((e.axis & 3) < 3) {
                offset[e.axis & 3] = degrees * 0.1f;
            }
            break;
        }
        case HelpBoneType::Follow:
            result = glm::normalize(parent * SlerpFromIdentity(q, e.weight));
            break;
        case HelpBoneType::Twist:
            result = glm::normalize(parent * ScaledTwist(Twist(q, e.a), e.weight));
            break;
        case HelpBoneType::TwistSlide:
            slide();
            result = glm::normalize(parent * ScaledTwist(Twist(q, e.a), e.weight));
            break;
        case HelpBoneType::SwingTurn:
        case HelpBoneType::SwingTurnSlide: {
            const Swing s = SwingOf(q, e.a, e.b);
            float g = Toward(s);
            if (e.Is(HelpBoneType::SwingTurn)) {
                g = s.x >= 0.0f ? g : -g;
            } else {
                slide();
                g = s.y >= 0.0f ? 1.0f - g : g - 1.0f;
            }
            const float angle = Limit(e.weight * g * s.theta, e.min * kDegree, e.max * kDegree);
            if (e.axis > 2) {
                continue;
            }
            result = glm::normalize(parent * AxisRotation(e.axis, angle));
            break;
        }
        default:
            continue;
        }
        rotation[driven] = result;
        position[driven] = position[static_cast<size_t>(e.parent)] + parent * offset;
    }
}

void HelpBones::Apply(const Skeleton& skeleton, std::vector<glm::mat4>& world) const {
    if (entries_.empty() || !HelpBonesEnabled()) {
        return;
    }
    const size_t n = std::min(world.size(), skeleton.Size());
    std::vector<glm::quat> rotation(n);
    std::vector<glm::vec3> position(n);
    for (size_t i = 0; i < n; ++i) {
        rotation[i] = glm::normalize(glm::quat_cast(glm::mat3(world[i])));
        position[i] = glm::vec3(world[i][3]);
    }
    Evaluate(std::span<const glm::vec3>(skeleton.bind_local.data(), n), rotation, position);
    for (const HelpBoneEntry& e : entries_) {
        if (e.driven >= 0 && static_cast<size_t>(e.driven) < n) {
            const size_t d = static_cast<size_t>(e.driven);
            world[d] = glm::translate(glm::mat4(1.0f), position[d]) * glm::mat4_cast(rotation[d]);
        }
    }
}

}
