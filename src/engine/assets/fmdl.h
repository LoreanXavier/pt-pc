#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/render/mesh.h"

namespace pt {

struct FmdlMaterial {
    std::string name;
    std::string shader;
    std::string technique;
    std::string base_color;
    std::string normal_map;
    std::string specular_map;
    std::string mask;

    glm::vec4 Vector(std::string_view name, glm::vec4 fallback = glm::vec4(0.0f)) const {
        for (const auto& [n, v] : vectors) {
            if (n == name) {
                return v;
            }
        }
        return fallback;
    }
    std::vector<std::pair<std::string, std::string>> textures;
    std::vector<std::pair<std::string, glm::vec4>> vectors;
};

struct FmdlMeshInfo {
    uint32_t render_flags = 0;
    uint32_t material = 0;
    uint32_t vertex_count = 0;
    uint32_t index_count = 0;
    bool skinned = false;
    int32_t bone_group = -1;
    std::vector<uint16_t> bones;
};

struct FmdlBoneGroup {
    uint16_t max_weights = 0;
    std::vector<uint16_t> bones;
};

struct FmdlModel {
    MeshData mesh;
    std::vector<FmdlMaterial> materials;
    std::vector<FmdlMeshInfo> meshes;
    std::vector<glm::vec3> bone_world;
    std::vector<int16_t> bone_parent;
    std::vector<std::string> bone_names;
    std::vector<FmdlBoneGroup> bone_groups;
    std::vector<std::string> mesh_group_names;
    std::vector<glm::vec3> raw_positions;
};

bool LoadFmdl(std::span<const uint8_t> data, const std::string& name, FmdlModel& out);

}
