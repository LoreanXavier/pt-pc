#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/type_precision.hpp>

#include <bitset>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "engine/render/vk.h"

namespace pt {

struct Vertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
    glm::vec2 uv0{0.0f};
    glm::vec2 uv1{0.0f};
    glm::vec4 color{1.0f};
    glm::u8vec4 joints{0};
    glm::u8vec4 weights{0};
    glm::vec2 uv2{0.0f};
};
static_assert(sizeof(Vertex) == 88);

enum class RenderPass : uint8_t { Opaque = 0, Decal = 1, Unlit = 2, Transparent = 3, Count = 4 };

struct SubMesh {
    uint32_t first_index = 0;
    uint32_t index_count = 0;
    int32_t vertex_offset = 0;
    uint32_t material = 0;
    RenderPass pass = RenderPass::Opaque;
    uint8_t kind = 0;
    bool double_sided = false;
    bool skinned = false;
    bool shadow = true;
    bool shadow_double_sided = false;
    uint16_t group = 0;
};

struct MeshGroup {
    uint64_t code = 0;
    int16_t parent = -1;
};

constexpr size_t kMaxHiddenMeshes = 256;
using MeshMask = std::bitset<kMaxHiddenMeshes>;

struct MeshData {
    std::string name;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<SubMesh> submeshes;
    std::vector<MeshGroup> groups;
    glm::vec3 bounds_min{0.0f};
    glm::vec3 bounds_max{0.0f};
};

struct GpuMesh {
    std::string name;
    vk::Buffer vertices;
    vk::Buffer indices;
    std::vector<SubMesh> submeshes;
    std::vector<MeshGroup> groups;
    glm::vec3 bounds_min{0.0f};
    glm::vec3 bounds_max{0.0f};
    bool skinned = false;
};

MeshData MakeBoxMesh(glm::vec3 half_extent);
MeshMask HiddenMeshes(const GpuMesh& mesh, const std::set<uint64_t>& hidden_group_codes);

}
