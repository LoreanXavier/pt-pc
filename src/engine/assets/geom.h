#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace pt {

struct GeomTriangle {
    glm::vec3 a;
    glm::vec3 b;
    glm::vec3 c;
    uint64_t tags = 0;
    uint8_t node = 0;
    uint32_t material = 0;
    // the shape's flags (info >> 4: 0x200 double sided, 0x800 over the fmdl's vertices) and the polygon's 16-bit info word
    // (bits 2-8 the material index)
    uint32_t shape_flags = 0;
    uint16_t prim_info = 0;
};

bool LoadGeom(std::span<const uint8_t> data, std::span<const glm::vec3> fmdl_positions, std::vector<GeomTriangle>& out,
              bool* skipped_fmdl_materials = nullptr);

}
