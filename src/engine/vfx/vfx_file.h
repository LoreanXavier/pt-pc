#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/vfx/vfx_schema.h"

namespace pt::vfx {

uint32_t NameHash(std::string_view name);

struct Prop {
    uint32_t hash = 0;
    PropType type = PropType::Bool;
    std::vector<glm::vec4> vectors;
    std::vector<uint64_t> ints;
    std::vector<std::string> strings;

    size_t Count() const;
};

struct Node {
    const ClassDef* def = nullptr;
    uint32_t index = 0;
    std::vector<Prop> props;

    std::string_view ClassName() const { return def ? std::string_view(def->name) : std::string_view(); }
    const Prop* Find(uint32_t hash) const;
    const Prop* Find(std::string_view name) const { return Find(NameHash(name)); }
    float Float(uint32_t hash, float fallback = 0.0f, size_t i = 0) const;
    float Float(std::string_view name, float fallback = 0.0f, size_t i = 0) const { return Float(NameHash(name), fallback, i); }
    uint32_t UInt(uint32_t hash, uint32_t fallback = 0, size_t i = 0) const;
    uint32_t UInt(std::string_view name, uint32_t fallback = 0, size_t i = 0) const { return UInt(NameHash(name), fallback, i); }
    bool Bool(uint32_t hash, bool fallback = false) const { return UInt(hash, fallback ? 1u : 0u) != 0; }
    bool Bool(std::string_view name, bool fallback = false) const { return Bool(NameHash(name), fallback); }
    glm::vec4 Vec4(uint32_t hash, const glm::vec4& fallback = glm::vec4(0.0f)) const;
    glm::vec4 Vec4(std::string_view name, const glm::vec4& fallback = glm::vec4(0.0f)) const { return Vec4(NameHash(name), fallback); }
    std::string String(uint32_t hash) const;
    std::string String(std::string_view name) const { return String(NameHash(name)); }
    uint64_t Code(uint32_t hash) const;
    uint64_t Code(std::string_view name) const { return Code(NameHash(name)); }
    std::vector<float> Floats(std::string_view name) const;
};

struct Edge {
    uint16_t from = 0;
    uint16_t to = 0;
    uint8_t from_type = 0;
    uint8_t from_port = 0;
    uint8_t to_type = 0;
    uint8_t to_port = 0;
};

struct File {
    uint16_t version = 0;
    std::vector<Node> nodes;
    std::vector<Edge> edges;

    const Node* Input(uint32_t node, uint8_t port) const;
};

bool ParseFile(std::span<const uint8_t> data, File& out, std::string* error = nullptr);

struct SoundNode {
    std::string play;
    std::string stop;
    bool stop_playing = false;
    float fade = 0.0f;
    uint32_t curve = 4;
};

bool ReadSoundNode(std::span<const uint8_t> data, SoundNode& out);

}
