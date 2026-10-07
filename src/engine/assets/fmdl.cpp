#include "engine/assets/fmdl.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cstring>
#include <map>

#include "engine/core/log.h"
#include "engine/core/strcode.h"

namespace pt {
namespace {

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

struct FeatureRef {
    size_t offset = 0;
    uint32_t count = 0;
};

glm::vec4 ReadElement(const Reader& r, size_t offset, uint8_t format) {
    switch (format) {
    case 1: return glm::vec4(r.At<float>(offset), r.At<float>(offset + 4), r.At<float>(offset + 8), 1.0f);
    case 6: return glm::vec4(glm::unpackHalf1x16(r.At<uint16_t>(offset)), glm::unpackHalf1x16(r.At<uint16_t>(offset + 2)),
                             glm::unpackHalf1x16(r.At<uint16_t>(offset + 4)), glm::unpackHalf1x16(r.At<uint16_t>(offset + 6)));
    case 7: return glm::vec4(glm::unpackHalf1x16(r.At<uint16_t>(offset)), glm::unpackHalf1x16(r.At<uint16_t>(offset + 2)), 0.0f, 0.0f);
    case 8: return glm::vec4(r.At<uint8_t>(offset), r.At<uint8_t>(offset + 1), r.At<uint8_t>(offset + 2), r.At<uint8_t>(offset + 3)) / 255.0f;
    case 9: return glm::vec4(r.At<uint8_t>(offset), r.At<uint8_t>(offset + 1), r.At<uint8_t>(offset + 2), r.At<uint8_t>(offset + 3));
    default: return glm::vec4(0.0f);
    }
}

std::string StemOf(const std::string& file) {
    const size_t dot = file.find_last_of('.');
    return dot == std::string::npos ? file : file.substr(0, dot);
}

}

bool LoadFmdl(std::span<const uint8_t> bytes, const std::string& name, FmdlModel& out) {
    Reader r{bytes};
    if (bytes.size() < 0x40 || std::memcmp(bytes.data(), "FMDL", 4) != 0) {
        LogError("fmdl: {} is not an FMDL file", name);
        return false;
    }
    const uint64_t table_offset = r.At<uint64_t>(0x08);
    const uint32_t feature_count = r.At<uint32_t>(0x20);
    const uint32_t buffer_count = r.At<uint32_t>(0x24);
    const uint32_t features_offset = r.At<uint32_t>(0x28);
    const uint32_t buffers_offset = r.At<uint32_t>(0x30);

    std::map<uint8_t, FeatureRef> features;
    for (uint32_t i = 0; i < feature_count; ++i) {
        const size_t h = table_offset + i * 8;
        const uint8_t type = r.At<uint8_t>(h);
        const uint32_t count = r.At<uint8_t>(h + 1) * 65536u + r.At<uint16_t>(h + 2);
        features[type] = {features_offset + size_t(r.At<uint32_t>(h + 4)), count};
    }
    std::map<uint32_t, std::pair<size_t, uint32_t>> buffers;
    for (uint32_t i = 0; i < buffer_count; ++i) {
        const size_t h = table_offset + feature_count * 8 + i * 12;
        buffers[r.At<uint32_t>(h)] = {buffers_offset + size_t(r.At<uint32_t>(h + 4)), r.At<uint32_t>(h + 8)};
    }
    auto feature = [&](uint8_t type) { return features.contains(type) ? features[type] : FeatureRef{}; };

    std::vector<std::string> strings;
    const FeatureRef string_feature = feature(12);
    const size_t string_buffer = buffers.contains(3) ? buffers[3].first : 0;
    for (uint32_t i = 0; i < string_feature.count; ++i) {
        const size_t rec = string_feature.offset + i * 8;
        const uint16_t length = r.At<uint16_t>(rec + 2);
        const uint32_t offset = r.At<uint32_t>(rec + 4);
        const size_t start = string_buffer + offset;
        strings.emplace_back(start + length <= bytes.size() ? std::string(reinterpret_cast<const char*>(bytes.data() + start), length)
                                                            : std::string());
    }
    auto str = [&](uint16_t index) { return index < strings.size() ? strings[index] : std::string(); };

    const size_t vertex_buffer = buffers.contains(2) ? buffers[2].first : 0;
    const size_t vector_buffer = buffers.contains(0) ? buffers[0].first : 0;
    std::vector<std::pair<uint16_t, size_t>> file_buffers;
    const FeatureRef fb = feature(14);
    for (uint32_t i = 0; i < fb.count; ++i) {
        const size_t rec = fb.offset + i * 0x10;
        file_buffers.emplace_back(r.At<uint16_t>(rec), vertex_buffer + r.At<uint32_t>(rec + 8));
        if (i == 0) {
            const uint32_t size = r.At<uint32_t>(rec + 4);
            const size_t base = file_buffers.back().second;
            out.raw_positions.reserve(size / 12);
            for (uint32_t v = 0; v + 12 <= size; v += 12) {
                out.raw_positions.emplace_back(r.At<float>(base + v), r.At<float>(base + v + 4), r.At<float>(base + v + 8));
            }
        }
    }
    size_t index_data = 0;
    for (const auto& [type, offset] : file_buffers) {
        if (type == 1) {
            index_data = offset;
        }
    }

    const FeatureRef textures = feature(6);
    const FeatureRef params = feature(7);
    const FeatureRef shaders = feature(8);
    const FeatureRef instances = feature(4);
    for (uint32_t i = 0; i < instances.count; ++i) {
        const size_t rec = instances.offset + i * 0x10;
        FmdlMaterial m;
        m.name = str(r.At<uint16_t>(rec));
        const uint16_t shader_index = r.At<uint16_t>(rec + 4);
        const uint8_t texture_count = r.At<uint8_t>(rec + 6);
        const uint8_t vector_count = r.At<uint8_t>(rec + 7);
        const uint16_t first_texture = r.At<uint16_t>(rec + 8);
        const uint16_t first_vector = r.At<uint16_t>(rec + 10);
        if (shader_index < shaders.count) {
            m.shader = str(r.At<uint16_t>(shaders.offset + shader_index * 4));
            m.technique = str(r.At<uint16_t>(shaders.offset + shader_index * 4 + 2));
        }
        for (uint32_t t = 0; t < texture_count; ++t) {
            const size_t p = params.offset + (first_texture + t) * 4;
            const std::string param_name = str(r.At<uint16_t>(p));
            const uint16_t ref = r.At<uint16_t>(p + 2);
            if (ref >= textures.count) {
                continue;
            }
            const std::string file = str(r.At<uint16_t>(textures.offset + ref * 4));
            const std::string dir = str(r.At<uint16_t>(textures.offset + ref * 4 + 2));
            const std::string path = dir + StemOf(file);
            m.textures.emplace_back(param_name, path);
            if (param_name.starts_with("Base_Tex") && m.base_color.empty()) {
                m.base_color = path;
            } else if (param_name.starts_with("NormalMap_Tex") && m.normal_map.empty()) {
                m.normal_map = path;
            } else if (param_name.starts_with("SpecularMap_Tex") && m.specular_map.empty()) {
                m.specular_map = path;
            } else if (param_name.starts_with("Mask_Tex") && m.mask.empty()) {
                m.mask = path;
            }
        }
        for (uint32_t v = 0; v < vector_count; ++v) {
            const size_t p = params.offset + (first_vector + v) * 4;
            const uint16_t ref = r.At<uint16_t>(p + 2);
            const size_t vo = vector_buffer + size_t(ref) * 16;
            m.vectors.emplace_back(str(r.At<uint16_t>(p)),
                                   glm::vec4(r.At<float>(vo), r.At<float>(vo + 4), r.At<float>(vo + 8), r.At<float>(vo + 12)));
        }
        out.materials.push_back(std::move(m));
    }

    const FeatureRef bones = feature(0);
    for (uint32_t i = 0; i < bones.count; ++i) {
        const size_t rec = bones.offset + i * 0x30;
        out.bone_names.push_back(str(r.At<uint16_t>(rec)));
        out.bone_parent.push_back(r.At<int16_t>(rec + 2));
        out.bone_world.emplace_back(r.At<float>(rec + 0x20), r.At<float>(rec + 0x24), r.At<float>(rec + 0x28));
    }
    const FeatureRef bone_groups = feature(5);
    for (uint32_t i = 0; i < bone_groups.count; ++i) {
        const size_t rec = bone_groups.offset + i * 0x44;
        FmdlBoneGroup group;
        group.max_weights = r.At<uint16_t>(rec);
        const uint16_t count = std::min<uint16_t>(r.At<uint16_t>(rec + 2), 32);
        for (uint16_t b = 0; b < count; ++b) {
            group.bones.push_back(r.At<uint16_t>(rec + 4 + b * 2));
        }
        out.bone_groups.push_back(std::move(group));
    }

    const FeatureRef mesh_groups = feature(1);
    for (uint32_t i = 0; i < mesh_groups.count; ++i) {
        const size_t rec = mesh_groups.offset + i * 8;
        out.mesh_group_names.push_back(str(r.At<uint16_t>(rec)));
        out.mesh.groups.push_back({StrCode64(out.mesh_group_names.back()) & kStrCode64Mask, r.At<int16_t>(rec + 4)});
    }
    std::vector<uint16_t> group_of_mesh;
    const FeatureRef group_defs = feature(2);
    for (uint32_t i = 0; i < group_defs.count; ++i) {
        const size_t rec = group_defs.offset + i * 0x20;
        const uint16_t group = r.At<uint16_t>(rec + 4);
        const uint16_t count = r.At<uint16_t>(rec + 6);
        const uint16_t first = r.At<uint16_t>(rec + 8);
        if (group_of_mesh.size() < size_t(first) + count) {
            group_of_mesh.resize(size_t(first) + count, 0);
        }
        for (uint16_t k = 0; k < count; ++k) {
            group_of_mesh[first + k] = group;
        }
    }

    const FeatureRef meshes = feature(3);
    const FeatureRef layouts = feature(9);
    const FeatureRef streams = feature(10);
    const FeatureRef elements = feature(11);
    MeshData& mesh = out.mesh;
    mesh.name = name;
    for (uint32_t m = 0; m < meshes.count; ++m) {
        const size_t rec = meshes.offset + m * 0x30;
        FmdlMeshInfo info;
        info.render_flags = r.At<uint32_t>(rec);
        info.material = r.At<uint16_t>(rec + 4);
        const uint16_t group_index = r.At<uint16_t>(rec + 6);
        if (group_index < out.bone_groups.size() && bones.count > 0) {
            info.bone_group = group_index;
            info.bones = out.bone_groups[group_index].bones;
        }
        const uint16_t layout_index = r.At<uint16_t>(rec + 8);
        info.vertex_count = r.At<uint16_t>(rec + 0x0A);
        const uint32_t first_index = r.At<uint32_t>(rec + 0x10);
        info.index_count = r.At<uint32_t>(rec + 0x14);

        const size_t lay = layouts.offset + layout_index * 8;
        const uint8_t stream_count = r.At<uint8_t>(lay);
        const uint16_t first_stream = r.At<uint16_t>(lay + 4);
        uint16_t element_cursor = r.At<uint16_t>(lay + 6);
        const uint32_t base_vertex = static_cast<uint32_t>(mesh.vertices.size());
        mesh.vertices.resize(base_vertex + info.vertex_count);
        for (uint8_t s = 0; s < stream_count; ++s) {
            const size_t st = streams.offset + (first_stream + s) * 8;
            const uint8_t file_buffer = r.At<uint8_t>(st);
            const uint8_t element_count = r.At<uint8_t>(st + 1);
            const uint8_t stride = r.At<uint8_t>(st + 2);
            const uint32_t byte_offset = r.At<uint32_t>(st + 4);
            const size_t stream_base = (file_buffer < file_buffers.size() ? file_buffers[file_buffer].second : 0) + byte_offset;
            for (uint8_t e = 0; e < element_count; ++e, ++element_cursor) {
                const size_t el = elements.offset + element_cursor * 4;
                const uint8_t usage = r.At<uint8_t>(el);
                const uint8_t format = r.At<uint8_t>(el + 1);
                const uint16_t element_offset = r.At<uint16_t>(el + 2);
                for (uint32_t v = 0; v < info.vertex_count; ++v) {
                    const glm::vec4 value = ReadElement(r, stream_base + size_t(v) * stride + element_offset, format);
                    Vertex& vertex = mesh.vertices[base_vertex + v];
                    switch (usage) {
                    case 0: vertex.position = glm::vec3(value); break;
                    case 2: vertex.normal = glm::vec3(value); break;
                    case 3: vertex.color = value; break;
                    case 8: vertex.uv0 = glm::vec2(value); break;
                    case 9: vertex.uv1 = glm::vec2(value); break;
                    case 10: vertex.uv2 = glm::vec2(value); break;
                    case 14: vertex.tangent = value; break;
                    case 1:
                        info.skinned = true;
                        for (int c = 0; c < 4; ++c) {
                            vertex.weights[c] = static_cast<uint8_t>(std::clamp(value[c] * 255.0f + 0.5f, 0.0f, 255.0f));
                        }
                        break;
                    case 7:
                        info.skinned = true;
                        for (int c = 0; c < 4; ++c) {
                            const uint32_t local = static_cast<uint32_t>(value[c]);
                            vertex.joints[c] = static_cast<uint8_t>(local < info.bones.size() ? std::min<uint16_t>(info.bones[local], 255) : 0);
                        }
                        break;
                    default: break;
                    }
                }
            }
        }
        SubMesh sub;
        sub.first_index = static_cast<uint32_t>(mesh.indices.size());
        sub.index_count = info.index_count - info.index_count % 3;
        sub.vertex_offset = static_cast<int32_t>(base_vertex);
        sub.material = info.material;
        sub.skinned = info.skinned && !info.bones.empty();
        sub.group = m < group_of_mesh.size() ? group_of_mesh[m] : 0;
        for (uint32_t i = 0; i + 2 < info.index_count; i += 3) {
            const size_t at = index_data + (size_t(first_index) + i) * 2;
            const uint16_t a = r.At<uint16_t>(at);
            const uint16_t b = r.At<uint16_t>(at + 2);
            const uint16_t c = r.At<uint16_t>(at + 4);
            mesh.indices.push_back(a);
            mesh.indices.push_back(c);
            mesh.indices.push_back(b);
        }
        mesh.submeshes.push_back(sub);
        out.meshes.push_back(info);
    }
    const FeatureRef boxes = feature(13);
    if (boxes.count > 0) {
        mesh.bounds_max = glm::vec3(r.At<float>(boxes.offset), r.At<float>(boxes.offset + 4), r.At<float>(boxes.offset + 8));
        mesh.bounds_min = glm::vec3(r.At<float>(boxes.offset + 16), r.At<float>(boxes.offset + 20), r.At<float>(boxes.offset + 24));
    }
    return true;
}

}
