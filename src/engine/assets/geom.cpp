#include "engine/assets/geom.h"

#include <algorithm>
#include <cstring>

#include "engine/core/log.h"

namespace pt {
namespace {

constexpr uint32_t kGeomVersion = 201209110;
constexpr uint32_t kShapeFmdlVertices = 0x800;
constexpr uint32_t kPrimPoly = 2;
constexpr uint32_t kPrimAabb = 4;

struct Reader {
    std::span<const uint8_t> data;

    template <typename T>
    T At(size_t offset) const {
        T value{};
        if (offset + sizeof(T) <= data.size()) {
            std::memcpy(&value, data.data() + offset, sizeof(T));
        }
        return value;
    }
};

int32_t Signed28(uint32_t v) {
    v &= 0x0FFFFFFFu;
    return (v & 0x08000000u) ? static_cast<int32_t>(v | 0xF0000000u) : static_cast<int32_t>(v);
}

struct VertexSource {
    const Reader* r = nullptr;
    size_t header = 0;
    bool fmdl_backed = false;
    std::span<const glm::vec3> fmdl;

    glm::vec3 Get(uint32_t index) const {
        if (fmdl_backed) {
            static_assert(sizeof(glm::vec3) == 12);
            const float* words = reinterpret_cast<const float*>(fmdl.data());
            const size_t word = size_t(r->At<uint32_t>(header + 0x18) / 4) + size_t(index) * 3;
            return word + 2 < fmdl.size() * 3 ? glm::vec3(words[word], words[word + 1], words[word + 2]) : glm::vec3(0.0f);
        }
        const uint32_t index_offset = r->At<uint32_t>(header + 0x04);
        const uint32_t relative = r->At<uint32_t>(header + 0x08);
        const uint32_t origin_index = r->At<uint32_t>(header + 0x0C);
        const size_t table = header + static_cast<size_t>(r->At<int64_t>(header + 0x10));
        auto entry = [&](size_t i) { return glm::vec3(r->At<float>(table + i * 16), r->At<float>(table + i * 16 + 4), r->At<float>(table + i * 16 + 8)); };
        glm::vec3 v = entry(size_t(index_offset) + index);
        if (relative) {
            v += entry(origin_index);
        }
        return v;
    }
};

struct GroupContext {
    uint8_t node = 0;
    std::span<const glm::vec3> fmdl;
    std::vector<uint32_t> materials;
    bool skipped_materials = false;
};

uint32_t PolyMaterial(const GroupContext& group, uint16_t info) {
    const uint32_t index = (info >> 2) & 0x7F;
    if ((info & 1) || index == 0x7F || index >= group.materials.size()) {
        return 0;
    }
    return group.materials[index];
}

void ReadShapes(const Reader& r, size_t shape, int depth, GroupContext& group, std::vector<GeomTriangle>& out) {
    while (depth < 8 && shape + 0x20 <= r.data.size()) {
        const uint32_t word = r.At<uint32_t>(shape);
        const uint32_t type = word & 0xF;
        const uint32_t flags = (word >> 4) & 0xFFFFF;
        const uint32_t count = word >> 24;
        const uint64_t tags = r.At<uint64_t>(shape + 0x10);
        if (type == kPrimPoly) {
            VertexSource source;
            source.r = &r;
            source.header = shape + static_cast<ptrdiff_t>(Signed28(r.At<uint32_t>(shape + 0x1C))) * 16;
            source.fmdl_backed = (flags & kShapeFmdlVertices) != 0;
            source.fmdl = group.fmdl;
            if (!source.fmdl_backed || !group.fmdl.empty()) {
                for (uint32_t p = 0; p < count; ++p) {
                    const size_t prim = shape + 0x20 + size_t(p) * 10;
                    const uint16_t a = r.At<uint16_t>(prim);
                    const uint16_t b = r.At<uint16_t>(prim + 2);
                    const uint16_t c = r.At<uint16_t>(prim + 4);
                    const uint16_t d = r.At<uint16_t>(prim + 6);
                    const uint16_t info = r.At<uint16_t>(prim + 8);
                    const uint32_t material = PolyMaterial(group, info);
                    const glm::vec3 va = source.Get(a);
                    const glm::vec3 vc = source.Get(c);
                    out.push_back({va, source.Get(b), vc, tags, group.node, material, flags, info});
                    if (d != a && d != c) {
                        out.push_back({va, vc, source.Get(d), tags, group.node, material, flags, info});
                    }
                }
            } else if (!group.materials.empty()) {
                for (uint32_t p = 0; p < count && !group.skipped_materials; ++p) {
                    group.skipped_materials = PolyMaterial(group, r.At<uint16_t>(shape + 0x20 + size_t(p) * 10 + 8)) != 0;
                }
            }
        } else if (type == kPrimAabb) {
            const int32_t child = Signed28(r.At<uint32_t>(shape + 0x0C));
            if (child != 0) {
                ReadShapes(r, shape + static_cast<ptrdiff_t>(child) * 16, depth + 1, group, out);
            }
        }
        const int32_t next = Signed28(r.At<uint32_t>(shape + 0x04));
        if (next == 0 || depth == 0) {
            break;
        }
        shape += static_cast<ptrdiff_t>(next) * 16;
    }
}

}

bool LoadGeom(std::span<const uint8_t> data, std::span<const glm::vec3> fmdl_positions, std::vector<GeomTriangle>& out,
              bool* skipped_fmdl_materials) {
    Reader r{data};
    if (r.At<uint32_t>(0) != kGeomVersion) {
        return false;
    }
    size_t node = r.At<uint32_t>(4);
    for (uint8_t node_index = 0; node_index < 2 && node && node + 0x28 <= data.size(); ++node_index) {
        const int32_t child = r.At<int32_t>(node + 0x18);
        if (child != 0) {
            const size_t group = node + static_cast<ptrdiff_t>(child);
            const size_t payload = group + static_cast<ptrdiff_t>(r.At<int32_t>(group + 0x0C));
            const uint32_t payload_size = r.At<uint32_t>(group + 0x10);
            const size_t end = std::min<size_t>(payload + payload_size, data.size());
            GroupContext context;
            context.node = node_index;
            context.fmdl = fmdl_positions;
            size_t terminator = payload;
            while (terminator + 0x20 <= end && r.At<uint8_t>(terminator) == 0) {
                terminator += 0x20;
            }
            const size_t materials = terminator + 0x20;
            if (materials + 4 <= end) {
                for (size_t entry = materials + 4 + size_t(r.At<uint8_t>(materials)) * 12; entry + 12 <= end; entry += 12) {
                    const uint32_t hash = r.At<uint32_t>(entry);
                    if (hash == 0) {
                        break;
                    }
                    context.materials.push_back(hash);
                }
            }
            for (size_t block = payload; block < terminator; block += 0x20) {
                ReadShapes(r, block + r.At<uint32_t>(block + 0x10), 0, context, out);
            }
            if (skipped_fmdl_materials && context.skipped_materials) {
                *skipped_fmdl_materials = true;
            }
        }
        const int32_t next = r.At<int32_t>(node + 0x20);
        if (next == 0) {
            break;
        }
        node += static_cast<ptrdiff_t>(next);
    }
    return true;
}

}
