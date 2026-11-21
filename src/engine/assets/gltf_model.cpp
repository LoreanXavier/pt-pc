#include "engine/core/ascii.h"
#include "engine/assets/gltf_model.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <optional>
#include <unordered_map>

#include "engine/core/json.h"
#include "engine/core/log.h"
#include "engine/fs/mods.h"
#include "engine/fs/vfs.h"

namespace pt {
namespace {

constexpr uint32_t kGlbMagic = 0x46546C67;
constexpr uint32_t kChunkJson = 0x4E4F534A;
constexpr uint32_t kChunkBin = 0x004E4942;
constexpr uint32_t kFlagDoubleSided = 0x20;
constexpr uint32_t kFlagAlphaTest = 0x80;

std::string Folder(const std::string& asset_path) {
    const size_t slash = asset_path.find_last_of('/');
    return slash == std::string::npos ? std::string() : asset_path.substr(0, slash + 1);
}

std::string Stem(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of('/');
    return dot != std::string::npos && (slash == std::string::npos || dot > slash) ? path.substr(0, dot) : path;
}

std::string LowerCopy(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(pt::AsciiLower(c));
    return out;
}

std::string Unescape(std::string_view uri) {
    std::string out;
    for (size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            out += static_cast<char>(std::stoi(std::string(uri.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        } else {
            out += uri[i];
        }
    }
    return out;
}

std::optional<std::vector<uint8_t>> Base64(std::string_view text) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    out.reserve(text.size() * 3 / 4);
    uint32_t bits = 0;
    int count = 0;
    for (char c : text) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        const int v = value(c);
        if (v < 0) return std::nullopt;
        bits = (bits << 6) | static_cast<uint32_t>(v);
        count += 6;
        if (count >= 8) {
            count -= 8;
            out.push_back(static_cast<uint8_t>((bits >> count) & 0xFF));
        }
    }
    return out;
}

class GltfReader {
public:
    GltfReader(const std::string& asset_path, const FmdlModel* original, std::string_view only_node)
        : asset_path_(asset_path), original_(original), only_node_(only_node) {}

    bool Read(std::span<const uint8_t> data, FmdlModel& out, std::string& error) {
        std::string_view json_text;
        if (data.size() >= 12 && Get32(data, 0) == kGlbMagic) {
            size_t at = 12;
            while (at + 8 <= data.size()) {
                const uint32_t length = Get32(data, at);
                const uint32_t type = Get32(data, at + 4);
                if (at + 8 + length > data.size()) return Fail(error, "the .glb is cut short");
                if (type == kChunkJson) {
                    json_text = std::string_view(reinterpret_cast<const char*>(data.data() + at + 8), length);
                } else if (type == kChunkBin && glb_bin_.empty()) {
                    glb_bin_.assign(data.begin() + static_cast<ptrdiff_t>(at + 8), data.begin() + static_cast<ptrdiff_t>(at + 8 + length));
                }
                at += 8 + ((length + 3) & ~3u);
            }
            if (json_text.empty()) return Fail(error, "the .glb has no JSON chunk");
        } else {
            json_text = std::string_view(reinterpret_cast<const char*>(data.data()), data.size());
        }
        std::string parse_error;
        if (!json::Parse(json_text, doc_, &parse_error)) return Fail(error, "the glTF JSON is invalid: " + parse_error);
        if (!doc_.IsObject()) return Fail(error, "the glTF is not a JSON object");
        if (const json::Value* asset = doc_.Find("asset"); !asset || asset->String("version").substr(0, 1) != "2") {
            return Fail(error, "only glTF 2.0 is read");
        }
        if (!LoadBuffers(error)) return false;
        Materials(out);
        if (!Skeleton(out, error)) return false;
        if (!Nodes(out, error)) return false;
        if (out.mesh.submeshes.empty()) return Fail(error, "the glTF has no triangle meshes");
        Finish(out);
        return true;
    }

private:
    static uint32_t Get32(std::span<const uint8_t> data, size_t at) {
        uint32_t v = 0;
        std::memcpy(&v, data.data() + at, 4);
        return v;
    }

    static bool Fail(std::string& error, std::string what) {
        error = std::move(what);
        return false;
    }

    std::optional<std::vector<uint8_t>> ReadUri(const std::string& uri) const {
        if (uri.starts_with("data:")) {
            const size_t comma = uri.find(',');
            if (comma == std::string::npos || uri.substr(0, comma).find(";base64") == std::string::npos) return std::nullopt;
            return Base64(std::string_view(uri).substr(comma + 1));
        }
        return mods::ReadOverride(Folder(asset_path_) + Unescape(uri));
    }

    bool LoadBuffers(std::string& error) {
        const json::Value* buffers = doc_.Find("buffers");
        for (size_t i = 0; buffers && i < buffers->Size(); ++i) {
            const json::Value& b = *buffers->At(i);
            const std::string uri = b.String("uri");
            if (uri.empty()) {
                if (glb_bin_.empty()) return Fail(error, std::format("buffer {} has no data", i));
                buffers_.push_back(glb_bin_);
                continue;
            }
            auto bytes = ReadUri(uri);
            if (!bytes) return Fail(error, std::format("buffer {} ({}) was not found beside the model", i, uri.starts_with("data:") ? "embedded" : uri));
            buffers_.push_back(std::move(*bytes));
        }
        return true;
    }

    std::optional<std::span<const uint8_t>> View(int index, size_t* stride = nullptr) const {
        const json::Value* views = doc_.Find("bufferViews");
        const json::Value* view = views ? views->At(static_cast<size_t>(index)) : nullptr;
        if (!view) return std::nullopt;
        const size_t buffer = static_cast<size_t>(view->Number("buffer", -1));
        const size_t offset = static_cast<size_t>(view->Number("byteOffset", 0));
        const size_t length = static_cast<size_t>(view->Number("byteLength", 0));
        if (buffer >= buffers_.size() || offset + length > buffers_[buffer].size()) return std::nullopt;
        if (stride) *stride = static_cast<size_t>(view->Number("byteStride", 0));
        return std::span<const uint8_t>(buffers_[buffer].data() + offset, length);
    }

    bool Accessor(int index, std::vector<float>& out, int& components) const {
        const json::Value* accessors = doc_.Find("accessors");
        const json::Value* a = accessors ? accessors->At(static_cast<size_t>(index)) : nullptr;
        if (!a) return false;
        static const std::unordered_map<std::string, int> kComponents = {{"SCALAR", 1}, {"VEC2", 2}, {"VEC3", 3}, {"VEC4", 4}, {"MAT4", 16}};
        const auto kind = kComponents.find(a->String("type"));
        if (kind == kComponents.end()) return false;
        components = kind->second;
        const size_t count = static_cast<size_t>(a->Number("count", 0));
        const int type = static_cast<int>(a->Number("componentType", 0));
        const bool normalized = a->Bool("normalized", false);
        size_t size = 0;
        switch (type) {
        case 5120: case 5121: size = 1; break;
        case 5122: case 5123: size = 2; break;
        case 5125: case 5126: size = 4; break;
        default: return false;
        }
        out.assign(count * static_cast<size_t>(components), 0.0f);
        const json::Value* view_index = a->Find("bufferView");
        if (!view_index) return true;
        size_t stride = 0;
        const auto view = View(static_cast<int>(view_index->number), &stride);
        if (!view) return false;
        const size_t element = size * static_cast<size_t>(components);
        if (stride == 0) stride = element;
        const size_t offset = static_cast<size_t>(a->Number("byteOffset", 0));
        if (count > 0 && offset + (count - 1) * stride + element > view->size()) return false;
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* p = view->data() + offset + i * stride;
            for (int c = 0; c < components; ++c, p += size) {
                float v = 0.0f;
                switch (type) {
                case 5120: { int8_t x; std::memcpy(&x, p, 1); v = normalized ? std::max(x / 127.0f, -1.0f) : x; break; }
                case 5121: { uint8_t x = *p; v = normalized ? x / 255.0f : x; break; }
                case 5122: { int16_t x; std::memcpy(&x, p, 2); v = normalized ? std::max(x / 32767.0f, -1.0f) : x; break; }
                case 5123: { uint16_t x; std::memcpy(&x, p, 2); v = normalized ? x / 65535.0f : x; break; }
                case 5125: { uint32_t x; std::memcpy(&x, p, 4); v = static_cast<float>(x); break; }
                case 5126: std::memcpy(&v, p, 4); break;
                }
                out[i * static_cast<size_t>(components) + static_cast<size_t>(c)] = v;
            }
        }
        return true;
    }

    std::string Image(int texture_index, const char* suffix) {
        const json::Value* textures = doc_.Find("textures");
        const json::Value* texture = textures ? textures->At(static_cast<size_t>(texture_index)) : nullptr;
        if (!texture || !texture->Find("source")) return {};
        const int image_index = static_cast<int>(texture->Number("source", -1));
        const std::string key = std::format("{}#{}", image_index, suffix);
        if (auto it = images_.find(key); it != images_.end()) return it->second;
        const json::Value* images = doc_.Find("images");
        const json::Value* image = images ? images->At(static_cast<size_t>(image_index)) : nullptr;
        std::optional<std::vector<uint8_t>> bytes;
        if (image && image->Find("bufferView")) {
            if (const auto view = View(static_cast<int>(image->Number("bufferView", -1)))) bytes.emplace(view->begin(), view->end());
        } else if (image) {
            bytes = ReadUri(image->String("uri"));
        }
        std::string path;
        if (bytes) {
            const std::string stem = std::format("{}_gltf_image{}_{}", Stem(asset_path_), image_index, suffix);
            mods::RegisterGenerated(stem + ".png", std::move(*bytes));
            path = stem;
        } else {
            LogWarn("mods: {}: image {} not found, drawn without it", asset_path_, image_index);
        }
        images_[key] = path;
        return path;
    }

    std::string ColourImage(glm::vec4 colour) {
        const std::string key = std::format("colour {:.4f} {:.4f} {:.4f} {:.4f}", colour.r, colour.g, colour.b, colour.a);
        if (auto it = images_.find(key); it != images_.end()) return it->second;
        auto crc = [](const uint8_t* p, size_t n) {
            uint32_t c = 0xFFFFFFFFu;
            for (size_t i = 0; i < n; ++i) {
                c ^= p[i];
                for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
            }
            return ~c;
        };
        std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        auto chunk = [&](const char* type, std::vector<uint8_t> body) {
            const uint32_t n = static_cast<uint32_t>(body.size());
            for (int s = 24; s >= 0; s -= 8) png.push_back(static_cast<uint8_t>(n >> s));
            std::vector<uint8_t> typed(type, type + 4);
            typed.insert(typed.end(), body.begin(), body.end());
            png.insert(png.end(), typed.begin(), typed.end());
            const uint32_t c = crc(typed.data(), typed.size());
            for (int s = 24; s >= 0; s -= 8) png.push_back(static_cast<uint8_t>(c >> s));
        };
        chunk("IHDR", {0, 0, 0, 1, 0, 0, 0, 1, 8, 6, 0, 0, 0});
        auto byte = [](float v, bool srgb) {
            v = std::clamp(v, 0.0f, 1.0f);
            if (srgb) v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
            return static_cast<uint8_t>(std::lround(v * 255.0f));
        };
        const std::vector<uint8_t> raw = {0, byte(colour.r, true), byte(colour.g, true), byte(colour.b, true), byte(colour.a, false)};
        uint32_t a = 1, b = 0;
        for (uint8_t x : raw) {
            a = (a + x) % 65521;
            b = (b + a) % 65521;
        }
        const uint32_t adler = (b << 16) | a;
        std::vector<uint8_t> idat = {0x78, 0x01, 0x01, static_cast<uint8_t>(raw.size()), 0, static_cast<uint8_t>(~raw.size()), 0xFF};
        idat.insert(idat.end(), raw.begin(), raw.end());
        for (int s = 24; s >= 0; s -= 8) idat.push_back(static_cast<uint8_t>(adler >> s));
        chunk("IDAT", idat);
        chunk("IEND", {});
        const std::string stem = std::format("{}_gltf_colour{}_srgb", Stem(asset_path_), images_.size());
        mods::RegisterGenerated(stem + ".png", std::move(png));
        images_[key] = stem;
        return images_[key];
    }

    static std::string ExtraString(const json::Value& extras, const char* key) { return extras.String(key); }

    void Materials(FmdlModel& out) {
        const json::Value* materials = doc_.Find("materials");
        for (size_t i = 0; materials && i < materials->Size(); ++i) {
            const json::Value& m = *materials->At(i);
            FmdlMaterial material;
            material.name = m.String("name", std::format("material{}", i));
            const json::Value* extras = m.Find("extras");
            if (extras && extras->IsObject() && !extras->String("shader").empty()) {
                material.shader = extras->String("shader");
                material.technique = extras->String("technique");
                if (const json::Value* textures = extras->Find("textures"); textures && textures->IsObject()) {
                    for (const auto& [name, value] : textures->object) {
                        if (value.IsString()) {
                            material.textures.emplace_back(name, Stem(value.string));
                        } else if (value.IsObject() && !value.String("stem").empty()) {
                            material.textures.emplace_back(name, value.String("directory") + value.String("stem"));
                        }
                    }
                }
                if (const json::Value* vectors = extras->Find("vectors"); vectors && vectors->IsObject()) {
                    for (const auto& [name, value] : vectors->object) {
                        glm::vec4 v(0.0f);
                        for (size_t c = 0; c < 4 && c < value.Size(); ++c) v[static_cast<int>(c)] = static_cast<float>(value.At(c)->number);
                        material.vectors.emplace_back(name, v);
                    }
                }
            } else {
                const bool double_sided = m.Bool("doubleSided", false);
                material.shader = "fox_3ddf_blin";
                material.technique = double_sided ? "fox3DDF_Blin_NC" : "fox3DDF_Blin";
                const json::Value* pbr = m.Find("pbrMetallicRoughness");
                glm::vec4 factor(1.0f);
                if (const json::Value* f = pbr ? pbr->Find("baseColorFactor") : nullptr; f && f->Size() == 4) {
                    for (int c = 0; c < 4; ++c) factor[c] = static_cast<float>(f->At(static_cast<size_t>(c))->number);
                }
                std::string base;
                if (const json::Value* t = pbr ? pbr->Find("baseColorTexture") : nullptr) {
                    base = Image(static_cast<int>(t->Number("index", -1)), "srgb");
                }
                if (base.empty()) base = ColourImage(factor);
                material.textures.emplace_back("Base_Tex_SRGB", base);
                if (const json::Value* t = m.Find("normalTexture")) {
                    const std::string normal = Image(static_cast<int>(t->Number("index", -1)), "nrm");
                    if (!normal.empty()) material.textures.emplace_back("NormalMap_Tex_NRM", normal);
                }
                if (extras && extras->IsObject()) {
                    if (const json::Value* s = extras->Find("pt_specular"); s && s->IsNumber()) {
                        const std::string srm = Image(static_cast<int>(s->number), "srm");
                        if (!srm.empty()) material.textures.emplace_back("SpecularMap_Tex_LIN", srm);
                    }
                    if (const json::Value* index = extras->Find("pt_material_index"); index && index->IsNumber()) {
                        material.vectors.emplace_back("MatParamIndex_0", glm::vec4(static_cast<float>(index->number), 0.0f, 0.0f, 0.0f));
                    }
                }
            }
            for (const auto& [name, path] : material.textures) {
                if (name.starts_with("Base_Tex") && material.base_color.empty()) material.base_color = path;
                else if (name.starts_with("NormalMap_Tex") && material.normal_map.empty()) material.normal_map = path;
                else if (name.starts_with("SpecularMap_Tex") && material.specular_map.empty()) material.specular_map = path;
                else if (name.starts_with("Mask_Tex") && material.mask.empty()) material.mask = path;
            }
            alpha_test_.push_back(m.String("alphaMode") == "MASK");
            double_sided_.push_back(m.Bool("doubleSided", false));
            out.materials.push_back(std::move(material));
        }
        if (out.materials.empty() || !materials) {
            FmdlMaterial material;
            material.name = "default";
            material.shader = "fox_3ddf_blin";
            material.technique = "fox3DDF_Blin";
            material.textures.emplace_back("Base_Tex_SRGB", ColourImage(glm::vec4(0.8f, 0.8f, 0.8f, 1.0f)));
            default_material_ = static_cast<uint32_t>(out.materials.size());
            out.materials.push_back(std::move(material));
            alpha_test_.push_back(false);
            double_sided_.push_back(false);
        }
    }

    bool Skeleton(FmdlModel& out, std::string& error) {
        if (original_) {
            out.bone_names = original_->bone_names;
            out.bone_parent = original_->bone_parent;
            out.bone_world = original_->bone_world;
            out.bone_groups = original_->bone_groups;
            out.mesh_group_names = original_->mesh_group_names;
            out.mesh.groups = original_->mesh.groups;
            out.raw_positions = original_->raw_positions;
        }
        const json::Value* skins = doc_.Find("skins");
        const json::Value* nodes = doc_.Find("nodes");
        for (size_t s = 0; skins && s < skins->Size(); ++s) {
            std::vector<int> map;
            const json::Value* joints = skins->At(s)->Find("joints");
            std::vector<std::string> missing;
            for (size_t j = 0; joints && j < joints->Size(); ++j) {
                const json::Value* node = nodes ? nodes->At(static_cast<size_t>(joints->At(j)->number)) : nullptr;
                const std::string name = node ? node->String("name") : std::string();
                int bone = -1;
                for (size_t b = 0; b < out.bone_names.size(); ++b) {
                    if (out.bone_names[b] == name) {
                        bone = static_cast<int>(b);
                        break;
                    }
                }
                if (bone < 0) missing.push_back(name.empty() ? std::format("joint {}", j) : name);
                map.push_back(std::max(bone, 0));
            }
            if (!missing.empty()) {
                if (out.bone_names.empty()) {
                    return Fail(error, "a skinned glTF replaces a game model that has bones (a character); this path has none");
                }
                std::string list;
                for (size_t i = 0; i < missing.size() && i < 8; ++i) list += (i ? ", " : "") + missing[i];
                LogWarn("mods: {}: {} joint(s) have no bone of that name in the original skeleton (weighted to its root): {}{}",
                        asset_path_, missing.size(), list, missing.size() > 8 ? ", ..." : "");
            }
            joint_maps_.push_back(std::move(map));
        }
        return true;
    }

    static glm::mat4 LocalMatrix(const json::Value& node) {
        if (const json::Value* m = node.Find("matrix"); m && m->Size() == 16) {
            glm::mat4 out;
            for (int i = 0; i < 16; ++i) glm::value_ptr(out)[i] = static_cast<float>(m->At(static_cast<size_t>(i))->number);
            return out;
        }
        glm::vec3 t(0.0f), s(1.0f);
        glm::quat r(1.0f, 0.0f, 0.0f, 0.0f);
        if (const json::Value* v = node.Find("translation"); v && v->Size() == 3) {
            t = glm::vec3(v->At(0)->number, v->At(1)->number, v->At(2)->number);
        }
        if (const json::Value* v = node.Find("rotation"); v && v->Size() == 4) {
            r = glm::quat(static_cast<float>(v->At(3)->number), static_cast<float>(v->At(0)->number), static_cast<float>(v->At(1)->number),
                          static_cast<float>(v->At(2)->number));
        }
        if (const json::Value* v = node.Find("scale"); v && v->Size() == 3) {
            s = glm::vec3(v->At(0)->number, v->At(1)->number, v->At(2)->number);
        }
        return glm::translate(glm::mat4(1.0f), t) * glm::mat4_cast(r) * glm::scale(glm::mat4(1.0f), s);
    }

    bool Nodes(FmdlModel& out, std::string& error) {
        const json::Value* nodes = doc_.Find("nodes");
        std::vector<int> roots;
        const json::Value* scenes = doc_.Find("scenes");
        const size_t scene = static_cast<size_t>(doc_.Number("scene", 0));
        if (const json::Value* s = scenes ? scenes->At(scene) : nullptr; s && s->Find("nodes")) {
            for (const json::Value& n : s->Find("nodes")->array) roots.push_back(static_cast<int>(n.number));
        } else if (nodes) {
            std::vector<bool> child(nodes->Size(), false);
            for (const json::Value& n : nodes->array) {
                if (const json::Value* c = n.Find("children")) {
                    for (const json::Value& i : c->array) {
                        if (static_cast<size_t>(i.number) < child.size()) child[static_cast<size_t>(i.number)] = true;
                    }
                }
            }
            for (size_t i = 0; i < child.size(); ++i) {
                if (!child[i]) roots.push_back(static_cast<int>(i));
            }
        }
        for (int root : roots) {
            if (!Node(root, glm::mat4(1.0f), out, error, 0, only_node_.empty())) return false;
        }
        return true;
    }

    bool Node(int index, const glm::mat4& parent, FmdlModel& out, std::string& error, int depth, bool wanted) {
        const json::Value* nodes = doc_.Find("nodes");
        const json::Value* node = nodes ? nodes->At(static_cast<size_t>(index)) : nullptr;
        if (!node || depth > 64) return true;
        const glm::mat4 world = parent * LocalMatrix(*node);
        wanted = wanted || node->String("name") == only_node_;
        if (const json::Value* mesh = wanted ? node->Find("mesh") : nullptr) {
            const json::Value* skin = node->Find("skin");
            node_extras_ = node->Find("extras");
            const int skin_index = skin ? static_cast<int>(skin->number) : -1;
            if (!Mesh(static_cast<int>(mesh->number), skin_index >= 0 ? glm::mat4(1.0f) : world, skin_index, node->String("name"), out, error)) {
                return false;
            }
        }
        if (const json::Value* children = node->Find("children")) {
            for (const json::Value& c : children->array) {
                if (!Node(static_cast<int>(c.number), world, out, error, depth + 1, wanted)) return false;
            }
        }
        return true;
    }

    uint16_t GroupOf(const std::string& node_name, const json::Value* mesh_extras) const {
        std::string name = mesh_extras ? mesh_extras->String("mesh_group") : std::string();
        if (name.empty()) {
            const size_t dot = node_name.find_last_of('.');
            name = dot == std::string::npos ? node_name : node_name.substr(0, dot);
        }
        for (size_t i = 0; i < mesh_group_names_.size(); ++i) {
            if (mesh_group_names_[i] == name) return static_cast<uint16_t>(i);
        }
        return 0;
    }

    bool Mesh(int index, const glm::mat4& transform, int skin, const std::string& node_name, FmdlModel& out, std::string& error) {
        const json::Value* meshes = doc_.Find("meshes");
        const json::Value* mesh = meshes ? meshes->At(static_cast<size_t>(index)) : nullptr;
        if (!mesh) return Fail(error, std::format("mesh {} is missing", index));
        mesh_group_names_ = out.mesh_group_names;
        const glm::mat3 normal_matrix = glm::transpose(glm::inverse(glm::mat3(transform)));
        const bool mirrored = glm::determinant(glm::mat3(transform)) < 0.0f;
        const json::Value* primitives = mesh->Find("primitives");
        for (size_t p = 0; primitives && p < primitives->Size(); ++p) {
            const json::Value& prim = *primitives->At(p);
            if (static_cast<int>(prim.Number("mode", 4)) != 4) {
                LogWarn("mods: {}: mesh {} primitive {} is not triangles, skipped", asset_path_, index, p);
                continue;
            }
            const json::Value* attributes = prim.Find("attributes");
            const json::Value* position_index = attributes ? attributes->Find("POSITION") : nullptr;
            if (!position_index) continue;
            std::vector<float> positions, normals, tangents, uv0, uv1, colours, joints, weights, indices;
            int n = 0;
            if (!Accessor(static_cast<int>(position_index->number), positions, n) || n != 3) {
                return Fail(error, std::format("mesh {} primitive {}: unreadable positions", index, p));
            }
            const size_t count = positions.size() / 3;
            auto optional = [&](const char* name, std::vector<float>& values, int want) {
                const json::Value* a = attributes->Find(name);
                int components = 0;
                if (!a || !Accessor(static_cast<int>(a->number), values, components) || components < want || values.size() / static_cast<size_t>(components) != count) {
                    values.clear();
                    return 0;
                }
                return components;
            };
            optional("NORMAL", normals, 3);
            optional("TANGENT", tangents, 4);
            optional("TEXCOORD_0", uv0, 2);
            optional("TEXCOORD_1", uv1, 2);
            const int colour_components = optional("COLOR_0", colours, 3);
            if (skin >= 0) {
                optional("JOINTS_0", joints, 4);
                optional("WEIGHTS_0", weights, 4);
            }
            if (const json::Value* i = prim.Find("indices")) {
                int c = 0;
                if (!Accessor(static_cast<int>(i->number), indices, c) || c != 1) {
                    return Fail(error, std::format("mesh {} primitive {}: unreadable indices", index, p));
                }
            } else {
                indices.resize(count);
                for (size_t k = 0; k < count; ++k) indices[k] = static_cast<float>(k);
            }
            const std::vector<int>* joint_map = skin >= 0 && static_cast<size_t>(skin) < joint_maps_.size() ? &joint_maps_[static_cast<size_t>(skin)] : nullptr;
            const bool skinned = joint_map && !joints.empty() && !weights.empty();
            const uint32_t base = static_cast<uint32_t>(out.mesh.vertices.size());
            for (size_t v = 0; v < count; ++v) {
                Vertex vertex;
                vertex.position = glm::vec3(transform * glm::vec4(positions[v * 3], positions[v * 3 + 1], positions[v * 3 + 2], 1.0f));
                if (!normals.empty()) {
                    vertex.normal = glm::normalize(normal_matrix * glm::vec3(normals[v * 3], normals[v * 3 + 1], normals[v * 3 + 2]));
                }
                if (!tangents.empty()) {
                    vertex.tangent = glm::vec4(glm::normalize(glm::mat3(transform) * glm::vec3(tangents[v * 4], tangents[v * 4 + 1], tangents[v * 4 + 2])),
                                               tangents[v * 4 + 3] < 0.0f ? -1.0f : 1.0f);
                }
                if (!uv0.empty()) vertex.uv0 = glm::vec2(uv0[v * 2], uv0[v * 2 + 1]);
                if (!uv1.empty()) vertex.uv1 = glm::vec2(uv1[v * 2], uv1[v * 2 + 1]);
                if (!colours.empty()) {
                    const size_t c = static_cast<size_t>(colour_components);
                    vertex.color = glm::vec4(colours[v * c], colours[v * c + 1], colours[v * c + 2], c == 4 ? colours[v * c + 3] : 1.0f);
                }
                if (skinned) {
                    float total = 0.0f;
                    for (int c = 0; c < 4; ++c) total += std::max(weights[v * 4 + static_cast<size_t>(c)], 0.0f);
                    int sum = 0;
                    for (int c = 0; c < 4; ++c) {
                        const size_t j = static_cast<size_t>(joints[v * 4 + static_cast<size_t>(c)]);
                        vertex.joints[c] = static_cast<uint8_t>(std::min(j < joint_map->size() ? (*joint_map)[j] : 0, 255));
                        const float w = total > 0.0f ? std::max(weights[v * 4 + static_cast<size_t>(c)], 0.0f) / total : (c == 0 ? 1.0f : 0.0f);
                        vertex.weights[c] = static_cast<uint8_t>(std::lround(w * 255.0f));
                        sum += vertex.weights[c];
                    }
                    vertex.weights[0] = static_cast<uint8_t>(std::clamp(vertex.weights[0] + 255 - sum, 0, 255));
                }
                out.mesh.vertices.push_back(vertex);
            }
            SubMesh sub;
            sub.first_index = static_cast<uint32_t>(out.mesh.indices.size());
            sub.vertex_offset = static_cast<int32_t>(base);
            for (size_t k = 0; k + 2 < indices.size(); k += 3) {
                const uint32_t a = static_cast<uint32_t>(indices[k]), b = static_cast<uint32_t>(indices[k + 1]), c = static_cast<uint32_t>(indices[k + 2]);
                if (a >= count || b >= count || c >= count) return Fail(error, std::format("mesh {} primitive {}: index out of range", index, p));
                out.mesh.indices.push_back(a);
                out.mesh.indices.push_back(mirrored ? c : b);
                out.mesh.indices.push_back(mirrored ? b : c);
            }
            sub.index_count = static_cast<uint32_t>(out.mesh.indices.size()) - sub.first_index;
            const json::Value* material_index = prim.Find("material");
            sub.material = material_index ? static_cast<uint32_t>(material_index->number) : default_material_;
            if (sub.material >= out.materials.size()) sub.material = default_material_ < out.materials.size() ? default_material_ : 0;
            sub.skinned = skinned;
            const json::Value* prim_extras = prim.Find("extras");
            sub.group = GroupOf(node_name, node_extras_);
            FmdlMeshInfo info;
            info.material = sub.material;
            info.vertex_count = static_cast<uint32_t>(count);
            info.index_count = sub.index_count;
            info.skinned = skinned;
            const std::string flags = prim_extras ? prim_extras->String("flags") : std::string();
            if (!flags.empty()) {
                info.render_flags = static_cast<uint32_t>(std::stoul(flags, nullptr, 0));
            } else {
                if (sub.material < double_sided_.size() && double_sided_[sub.material]) info.render_flags |= kFlagDoubleSided;
                if (sub.material < alpha_test_.size() && alpha_test_[sub.material]) info.render_flags |= kFlagAlphaTest | 0x4000;
            }
            if (normals.empty() || tangents.empty()) {
                Tangents(out.mesh, sub, normals.empty(), tangents.empty());
            }
            out.mesh.submeshes.push_back(sub);
            out.meshes.push_back(info);
        }
        return true;
    }

    static void Tangents(MeshData& mesh, const SubMesh& sub, bool normals, bool tangents) {
        const size_t first = static_cast<size_t>(sub.vertex_offset);
        std::vector<glm::vec3> n_sum, t_sum, b_sum;
        size_t last = first;
        for (uint32_t i = 0; i < sub.index_count; ++i) last = std::max(last, first + mesh.indices[sub.first_index + i]);
        const size_t count = last - first + 1;
        n_sum.assign(count, glm::vec3(0.0f));
        t_sum.assign(count, glm::vec3(0.0f));
        b_sum.assign(count, glm::vec3(0.0f));
        for (uint32_t i = 0; i + 2 < sub.index_count; i += 3) {
            const uint32_t k[3] = {mesh.indices[sub.first_index + i], mesh.indices[sub.first_index + i + 1], mesh.indices[sub.first_index + i + 2]};
            const Vertex& a = mesh.vertices[first + k[0]];
            const Vertex& b = mesh.vertices[first + k[1]];
            const Vertex& c = mesh.vertices[first + k[2]];
            const glm::vec3 e1 = b.position - a.position, e2 = c.position - a.position;
            const glm::vec3 face = glm::cross(e1, e2);
            const glm::vec2 d1 = b.uv0 - a.uv0, d2 = c.uv0 - a.uv0;
            const float det = d1.x * d2.y - d2.x * d1.y;
            const glm::vec3 t = std::abs(det) > 1e-12f ? (e1 * d2.y - e2 * d1.y) / det : glm::vec3(0.0f);
            const glm::vec3 bt = std::abs(det) > 1e-12f ? (e2 * d1.x - e1 * d2.x) / det : glm::vec3(0.0f);
            for (uint32_t j : k) {
                n_sum[j] += face;
                t_sum[j] += t;
                b_sum[j] += bt;
            }
        }
        for (size_t v = 0; v < count; ++v) {
            Vertex& vertex = mesh.vertices[first + v];
            if (normals && glm::length(n_sum[v]) > 1e-12f) vertex.normal = glm::normalize(n_sum[v]);
            if (tangents) {
                glm::vec3 t = t_sum[v] - vertex.normal * glm::dot(vertex.normal, t_sum[v]);
                if (glm::length(t) < 1e-8f) {
                    t = glm::cross(std::abs(vertex.normal.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0), vertex.normal);
                }
                t = glm::normalize(t);
                const float w = glm::dot(glm::cross(vertex.normal, t), b_sum[v]) < 0.0f ? -1.0f : 1.0f;
                vertex.tangent = glm::vec4(t, w);
            }
        }
    }

    static void Finish(FmdlModel& out) {
        out.mesh.bounds_min = glm::vec3(1e30f);
        out.mesh.bounds_max = glm::vec3(-1e30f);
        for (const Vertex& v : out.mesh.vertices) {
            out.mesh.bounds_min = glm::min(out.mesh.bounds_min, v.position);
            out.mesh.bounds_max = glm::max(out.mesh.bounds_max, v.position);
        }
    }

    std::string asset_path_;
    const FmdlModel* original_;
    std::string only_node_;
    json::Value doc_;
    std::vector<uint8_t> glb_bin_;
    std::vector<std::vector<uint8_t>> buffers_;
    std::unordered_map<std::string, std::string> images_;
    std::vector<bool> alpha_test_;
    std::vector<bool> double_sided_;
    std::vector<std::vector<int>> joint_maps_;
    std::vector<std::string> mesh_group_names_;
    const json::Value* node_extras_ = nullptr;
    uint32_t default_material_ = 0;
};

}

bool LoadGltfModel(std::span<const uint8_t> data, const std::string& asset_path, const FmdlModel* original, FmdlModel& out,
                   std::string* error, std::string_view only_node) {
    out = FmdlModel{};
    out.mesh.name = asset_path;
    std::string why;
    GltfReader reader(asset_path, original, only_node);
    if (!reader.Read(data, out, why)) {
        if (error) *error = why;
        out = FmdlModel{};
        return false;
    }
    return true;
}

bool LoadModelFile(const Vfs& vfs, const std::string& path, FmdlModel& out) {
    return LoadModelFile(vfs, path, nullptr, out);
}

bool LoadModelFile(const Vfs& vfs, const std::string& path, const std::vector<uint8_t>* game_bytes, FmdlModel& out) {
    if (mods::Active()) {
        const std::string lower = LowerCopy(path);
        const bool direct = lower.ends_with(".glb") || lower.ends_with(".gltf");
        for (const char* extension : {".glb", ".gltf"}) {
            const std::string candidate = direct ? path : Stem(path) + extension;
            if (auto bytes = mods::ReadOverride(candidate)) {
                FmdlModel original;
                bool has_original = false;
                if (!direct) {
                    std::optional<std::vector<uint8_t>> read;
                    const std::vector<uint8_t>* original_bytes = game_bytes;
                    if (!original_bytes) {
                        read = vfs.ReadFile(path);
                        original_bytes = read ? &*read : nullptr;
                    }
                    has_original = original_bytes && LoadFmdl(*original_bytes, path, original);
                }
                std::string error;
                if (LoadGltfModel(*bytes, candidate, has_original ? &original : nullptr, out, &error)) {
                    LogInfo("mods: model {} from {} ({} vertices, {} meshes{})", path, candidate, out.mesh.vertices.size(),
                            out.mesh.submeshes.size(), has_original ? ", the original's skeleton and groups" : "");
                    return true;
                }
                LogWarn("mods: {} refused: {}{}", candidate, error, direct ? "" : "; the game's model is used");
                break;
            }
            if (direct) break;
        }
        if (direct) return false;
    }
    if (game_bytes) return LoadFmdl(*game_bytes, path, out);
    const auto bytes = vfs.ReadFile(path);
    return bytes && LoadFmdl(*bytes, path, out);
}

}
