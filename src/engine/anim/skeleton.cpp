#include "engine/anim/skeleton.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstring>
#include <map>

#include "engine/anim/anim_codec.h"
#include "engine/anim/motion_data.h"

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

}

int Skeleton::Find(uint32_t hash) const {
    for (size_t i = 0; i < hashes.size(); ++i) {
        if (hashes[i] == hash) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int Skeleton::FindName(std::string_view name) const {
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool ReadFmdlSkeleton(std::span<const uint8_t> data, Skeleton& out) {
    out = Skeleton{};
    if (data.size() < 0x40 || std::memcmp(data.data(), "FMDL", 4) != 0) {
        return false;
    }
    const uint64_t table = At<uint64_t>(data, 0x08);
    const uint32_t feature_count = At<uint32_t>(data, 0x20);
    const uint32_t buffer_count = At<uint32_t>(data, 0x24);
    const uint32_t features_offset = At<uint32_t>(data, 0x28);
    const uint32_t buffers_offset = At<uint32_t>(data, 0x30);
    std::map<uint8_t, std::pair<size_t, uint32_t>> features;
    for (uint32_t i = 0; i < feature_count; ++i) {
        const size_t h = table + 8 * static_cast<size_t>(i);
        const uint32_t count = At<uint8_t>(data, h + 1) * 65536u + At<uint16_t>(data, h + 2);
        features[At<uint8_t>(data, h)] = {features_offset + static_cast<size_t>(At<uint32_t>(data, h + 4)), count};
    }
    size_t string_buffer = 0;
    for (uint32_t i = 0; i < buffer_count; ++i) {
        const size_t h = table + 8 * static_cast<size_t>(feature_count) + 12 * static_cast<size_t>(i);
        if (At<uint32_t>(data, h) == 3) {
            string_buffer = buffers_offset + static_cast<size_t>(At<uint32_t>(data, h + 4));
        }
    }
    if (!features.contains(0)) {
        return true;
    }
    const auto [strings_at, string_count] = features.contains(12) ? features[12] : std::pair<size_t, uint32_t>{0, 0};
    auto string = [&](uint16_t index) {
        if (index >= string_count) {
            return std::string();
        }
        const size_t rec = strings_at + 8 * static_cast<size_t>(index);
        const uint16_t length = At<uint16_t>(data, rec + 2);
        const size_t start = string_buffer + At<uint32_t>(data, rec + 4);
        if (start + length > data.size()) {
            return std::string();
        }
        return std::string(reinterpret_cast<const char*>(data.data() + start), length);
    };
    const auto [bones_at, bone_count] = features[0];
    for (uint32_t i = 0; i < bone_count; ++i) {
        const size_t rec = bones_at + 0x30 * static_cast<size_t>(i);
        out.names.push_back(string(At<uint16_t>(data, rec)));
        out.hashes.push_back(StrCode32(out.names.back()));
        out.parents.push_back(At<int16_t>(data, rec + 2));
        out.bind_world.emplace_back(At<float>(data, rec + 0x20), At<float>(data, rec + 0x24), At<float>(data, rec + 0x28));
    }
    out.bind_local.resize(out.names.size());
    for (size_t i = 0; i < out.names.size(); ++i) {
        const int16_t parent = out.parents[i];
        out.bind_local[i] = out.bind_world[i];
        if (parent >= 0 && static_cast<size_t>(parent) < out.names.size()) {
            out.bind_local[i] -= out.bind_world[static_cast<size_t>(parent)];
        }
    }
    return true;
}

void Pose::Reset(size_t bones) {
    rotation.assign(bones, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    offset.assign(bones, glm::vec3(0.0f));
}

void ComputeBoneWorld(const Skeleton& skeleton, const Pose& pose, std::vector<glm::mat4>& world) {
    const size_t n = skeleton.Size();
    world.assign(n, glm::mat4(1.0f));
    std::vector<uint8_t> state(n, 0);
    auto solve = [&](auto&& self, size_t i) -> void {
        if (state[i] == 2) {
            return;
        }
        state[i] = 1;
        const glm::quat rotation = i < pose.rotation.size() ? pose.rotation[i] : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        const glm::vec3 offset = i < pose.offset.size() ? pose.offset[i] : glm::vec3(0.0f);
        const glm::mat4 local = glm::translate(glm::mat4(1.0f), skeleton.bind_local[i] + offset) * glm::mat4_cast(rotation);
        const int16_t parent = skeleton.parents[i];
        if (parent >= 0 && static_cast<size_t>(parent) < n && state[static_cast<size_t>(parent)] != 1) {
            self(self, static_cast<size_t>(parent));
            world[i] = world[static_cast<size_t>(parent)] * local;
        } else {
            world[i] = local;
        }
        state[i] = 2;
    };
    for (size_t i = 0; i < n; ++i) {
        solve(solve, i);
    }
}

void ComputeSkin(const Skeleton& skeleton, const std::vector<glm::mat4>& world, std::vector<glm::mat4>& skin) {
    skin.resize(world.size());
    for (size_t i = 0; i < world.size(); ++i) {
        skin[i] = world[i] * glm::translate(glm::mat4(1.0f), -skeleton.bind_world[i]);
    }
}

void BlendPose(const Pose& from, const Pose& to, float weight, Pose& out) {
    const size_t n = std::min(from.Size(), to.Size());
    Pose result;
    result.Reset(n);
    for (size_t i = 0; i < n; ++i) {
        result.rotation[i] = SlerpShortest(from.rotation[i], to.rotation[i], weight);
        result.offset[i] = from.offset[i] + (to.offset[i] - from.offset[i]) * weight;
    }
    out = std::move(result);
}

}
