#include "engine/render/mesh.h"

namespace pt {

MeshData MakeBoxMesh(glm::vec3 h) {
    MeshData mesh;
    mesh.name = "box";
    const glm::vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const glm::vec3& n : normals) {
        const glm::vec3 up = std::abs(n.y) > 0.5f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        const glm::vec3 right = glm::cross(up, n);
        const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
        const glm::vec2 corners[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (const glm::vec2& c : corners) {
            Vertex v;
            v.position = (n + right * c.x + up * c.y) * h;
            v.normal = n;
            v.tangent = glm::vec4(right, 1.0f);
            v.uv0 = c * 0.5f + 0.5f;
            mesh.vertices.push_back(v);
        }
        const uint32_t quad[6] = {0, 1, 2, 0, 2, 3};
        for (uint32_t i : quad) {
            mesh.indices.push_back(base + i);
        }
    }
    mesh.submeshes.push_back({0, static_cast<uint32_t>(mesh.indices.size()), 0, 0});
    mesh.bounds_min = -h;
    mesh.bounds_max = h;
    return mesh;
}

MeshMask HiddenMeshes(const GpuMesh& mesh, const std::set<uint64_t>& hidden_group_codes) {
    MeshMask mask;
    if (hidden_group_codes.empty()) {
        return mask;
    }
    const int32_t count = static_cast<int32_t>(mesh.groups.size());
    std::vector<uint8_t> hidden(mesh.groups.size(), 0);
    for (int32_t g = 0; g < count; ++g) {
        int32_t at = g;
        for (int depth = 0; at >= 0 && at < count && depth < 64; ++depth) {
            if (hidden_group_codes.contains(mesh.groups[at].code)) {
                hidden[g] = 1;
                break;
            }
            at = mesh.groups[at].parent;
        }
    }
    for (size_t i = 0; i < mesh.submeshes.size() && i < kMaxHiddenMeshes; ++i) {
        const uint16_t g = mesh.submeshes[i].group;
        mask[i] = g < hidden.size() && hidden[g] != 0;
    }
    return mask;
}

}
