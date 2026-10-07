#include "engine/ui/uif.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <unordered_map>

#include "engine/core/strcode.h"

namespace pt::ui {
namespace {

uint16_t U16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

uint32_t U32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

float F32(const uint8_t* p) {
    float v;
    std::memcpy(&v, p, 4);
    return v;
}

glm::vec4 Vec4(const uint8_t* p) {
    return {F32(p), F32(p + 4), F32(p + 8), F32(p + 12)};
}

const std::unordered_map<uint64_t, int>& TextureSlots() {
    static const std::unordered_map<uint64_t, int> slots = {
        {StrCode64("Base_Texture"), kUifBase},
        {StrCode64("Layer_Texture"), kUifLayer},
        {StrCode64("Mask_Texture"), kUifMask},
        {StrCode64("Screen_Texture"), kUifScreen},
    };
    return slots;
}

const std::unordered_map<uint64_t, int>& ParamSlots() {
    static const std::unordered_map<uint64_t, int> slots = [] {
        std::unordered_map<uint64_t, int> map;
        const char* groups[4] = {"BaseTex", "LayerTex", "MaskTex", "ScreenTex"};
        const char* fields[6] = {"UCenter", "VCenter", "UShift", "VShift", "URepeat", "VRepeat"};
        for (int g = 0; g < 4; ++g) {
            for (int k = 0; k < 6; ++k) {
                map[StrCode64(std::format("{}_{}", fields[k], groups[g]))] = g * 6 + k;
            }
            map[StrCode64(std::format("Blend_{}", groups[g]))] = 24 + g;
        }
        return map;
    }();
    return slots;
}

void DefaultParams(std::array<float, 28>& p) {
    for (int g = 0; g < 4; ++g) {
        p[g * 6 + 0] = 0.5f;
        p[g * 6 + 1] = 0.5f;
        p[g * 6 + 2] = 0.0f;
        p[g * 6 + 3] = 0.0f;
        p[g * 6 + 4] = 1.0f;
        p[g * 6 + 5] = 1.0f;
        p[24 + g] = 0.0f;
    }
    p[24] = 1.0f;
}

}

int UifParamSlot(uint32_t code32) {
    for (const auto& [hash, slot] : ParamSlots()) {
        if (static_cast<uint32_t>(hash) == code32) {
            return slot;
        }
    }
    return -1;
}

bool UifModel::Parse(std::span<const uint8_t> data, std::string* error) {
    nodes_.clear();
    names_.clear();
    textures_.clear();
    index_of_id_.clear();
    auto fail = [&](std::string text) {
        if (error) {
            *error = std::move(text);
        }
        return false;
    };
    if (data.size() < 0x20 || std::memcmp(data.data(), "UIF ", 4) != 0) {
        return fail("not a UIF file");
    }
    const uint8_t* b = data.data();
    const size_t size = data.size();
    const uint16_t node_count = U16(b + 0x0A);
    const uint16_t name_count = U16(b + 0x0C);
    const uint16_t texture_count = U16(b + 0x0E);
    const uint32_t node_table = U32(b + 0x10);
    const uint32_t name_table = U32(b + 0x14);
    const uint32_t texture_table = U32(b + 0x18);
    const uint32_t blob = U32(b + 0x1C);
    auto in_range = [&](size_t at, size_t bytes) { return at + bytes <= size; };
    if (!in_range(blob + static_cast<size_t>(name_table) + name_count * 8ull, 0) || !in_range(node_table, node_count * 8ull) ||
        !in_range(texture_table, texture_count * 8ull)) {
        return fail("UIF tables out of range");
    }
    for (uint16_t i = 0; i < name_count; ++i) {
        uint64_t hash;
        std::memcpy(&hash, b + blob + name_table + i * 8ull, 8);
        names_.push_back(hash & kStrCode64Mask);
    }
    for (uint16_t i = 0; i < texture_count; ++i) {
        const uint32_t length = U32(b + texture_table + i * 8ull);
        const uint32_t offset = U32(b + texture_table + i * 8ull + 4);
        if (!in_range(static_cast<size_t>(blob) + offset, length)) {
            return fail("UIF texture path out of range");
        }
        textures_.emplace_back(reinterpret_cast<const char*>(b + blob + offset), strnlen(reinterpret_cast<const char*>(b + blob + offset), length));
    }
    index_of_id_.assign(name_count + 1, -1);
    std::vector<uint16_t> parents;
    for (uint16_t i = 0; i < node_count; ++i) {
        const uint8_t* entry = b + node_table + i * 8ull;
        UifNode node;
        node.id = U16(entry);
        node.type = static_cast<UifNodeType>(U16(entry + 2));
        const uint32_t at = U32(entry + 4);
        const size_t node_size = node.type == UifNodeType::Root ? 8 : node.type == UifNodeType::Mesh ? 0x88 : node.type == UifNodeType::Text ? 0x8C : 0x50;
        if (!in_range(at, node_size)) {
            return fail(std::format("UIF node {} out of range", node.id));
        }
        const uint8_t* n = b + at;
        parents.push_back(U16(n));
        if (node.type != UifNodeType::Root) {
            node.flags = U16(n + 4);
            node.text_flags = U16(n + 6);
            node.size = {F32(n + 8), F32(n + 12)};
            node.scale = {F32(n + 16), F32(n + 20)};
            if ((node.flags & 0x100) != 0) {
                node.rotation = Vec4(n + 0x18);
            } else {
                const float half = glm::radians(F32(n + 0x20)) * 0.5f;
                node.rotation = {0.0f, 0.0f, std::sin(half), std::cos(half)};
            }
            node.translate = Vec4(n + 0x28);
            node.priority = F32(n + 0x38);
            node.color = Vec4(n + 0x3C);
        }
        if (node.type == UifNodeType::Mesh) {
            const uint16_t vertex_count = U16(n + 0x50);
            const uint16_t primitive_count = U16(n + 0x52);
            const uint32_t positions = U32(n + 0x54);
            const uint32_t uvs = U32(n + 0x58);
            const uint32_t triangles = U32(n + 0x64);
            if (!in_range(static_cast<size_t>(blob) + positions, vertex_count * 16ull) || !in_range(static_cast<size_t>(blob) + uvs, vertex_count * 16ull) ||
                !in_range(static_cast<size_t>(blob) + triangles, primitive_count * 6ull)) {
                return fail(std::format("UIF mesh {} data out of range", node.id));
            }
            for (uint16_t v = 0; v < vertex_count; ++v) {
                node.positions.push_back({F32(b + blob + positions + v * 16ull), F32(b + blob + positions + v * 16ull + 4)});
                node.uvs.push_back({F32(b + blob + uvs + v * 16ull), F32(b + blob + uvs + v * 16ull + 4)});
            }
            for (uint32_t k = 0; k < primitive_count * 3u; ++k) {
                node.indices.push_back(U16(b + blob + triangles + k * 2ull));
            }
            const uint32_t strip = U32(n + 0x60);
            const uint16_t point_count = U16(n + 0x6A);
            const uint32_t point_table = U32(n + 0x70);
            if (point_count && in_range(point_table, point_count * 0x18ull) && in_range(static_cast<size_t>(blob) + strip, vertex_count * 2ull)) {
                for (uint16_t k = 0; k < point_count; ++k) {
                    const uint8_t* e = b + point_table + k * 0x18ull;
                    UifPoint point;
                    point.name = U16(e) < names_.size() ? names_[U16(e)] : 0;
                    point.position = {F32(e + 8), F32(e + 12)};
                    const uint32_t list = U32(e + 4);
                    if (e[2] == 1 && list != 0xFFFFFFFFu && in_range(static_cast<size_t>(blob) + list, e[3] * 2ull)) {
                        for (uint8_t j = 0; j < e[3]; ++j) {
                            const uint16_t slot = U16(b + blob + list + j * 2ull);
                            if (slot < vertex_count) {
                                point.vertices.push_back(U16(b + blob + strip + slot * 2ull));
                            }
                        }
                    }
                    node.points.push_back(std::move(point));
                }
            }
            DefaultParams(node.material.params);
            if (const uint16_t material = U16(n + 0x74); material < names_.size()) {
                node.material_name = names_[material];
            }
            const uint16_t binding_count = U16(n + 0x78);
            const uint16_t param_count = U16(n + 0x7A);
            const uint32_t bindings = U32(n + 0x7C);
            const uint32_t params = U32(n + 0x80);
            if (in_range(bindings, binding_count * 4ull)) {
                for (uint16_t k = 0; k < binding_count; ++k) {
                    const uint16_t name = U16(b + bindings + k * 4ull);
                    const uint16_t texture = U16(b + bindings + k * 4ull + 2);
                    if (name < names_.size()) {
                        auto it = TextureSlots().find(names_[name]);
                        if (it != TextureSlots().end()) {
                            node.material.textures[it->second] = texture;
                        }
                    }
                }
            }
            if (in_range(params, param_count * 8ull)) {
                for (uint16_t k = 0; k < param_count; ++k) {
                    const uint16_t name = U16(b + params + k * 8ull + 2);
                    if (name < names_.size()) {
                        auto it = ParamSlots().find(names_[name]);
                        if (it != ParamSlots().end()) {
                            node.material.params[it->second] = F32(b + params + k * 8ull + 4);
                        }
                    }
                }
            }
        } else if (node.type == UifNodeType::Text) {
            node.box_min = Vec4(n + 0x50);
            node.box_max = Vec4(n + 0x60);
            const uint16_t text = U16(n + 0x80);
            const uint16_t font = U16(n + 0x82);
            node.text_name = text < names_.size() ? names_[text] : 0;
            node.font_name = font < names_.size() ? names_[font] : 0;
        }
        if (node.id < index_of_id_.size()) {
            index_of_id_[node.id] = static_cast<int>(nodes_.size());
        }
        nodes_.push_back(std::move(node));
    }
    for (size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i].type != UifNodeType::Root) {
            nodes_[i].parent = IndexOfId(parents[i]);
        }
    }
    return true;
}

int UifModel::IndexOfId(uint16_t id) const {
    return id < index_of_id_.size() ? index_of_id_[id] : -1;
}

const UifNode* UifModel::FindById(uint16_t id) const {
    const int index = IndexOfId(id);
    return index >= 0 ? &nodes_[index] : nullptr;
}

UifNode* UifModel::FindById(uint16_t id) {
    const int index = IndexOfId(id);
    return index >= 0 ? &nodes_[index] : nullptr;
}

uint16_t UifModel::AddNode(UifNode node, uint64_t name) {
    const uint16_t id = static_cast<uint16_t>(names_.size());
    names_.push_back(name & kStrCode64Mask);
    node.id = id;
    index_of_id_.resize(std::max<size_t>(index_of_id_.size(), id + 1u), -1);
    index_of_id_[id] = static_cast<int>(nodes_.size());
    nodes_.push_back(std::move(node));
    return id;
}

int UifModel::AddTexture(std::string path) {
    for (size_t i = 0; i < textures_.size(); ++i) {
        if (textures_[i] == path) {
            return static_cast<int>(i);
        }
    }
    textures_.push_back(std::move(path));
    return static_cast<int>(textures_.size()) - 1;
}

}
