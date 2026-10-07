#include "engine/render/model_cache.h"

#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "engine/core/log.h"
#include "engine/render/gpu_types.h"

namespace pt {
namespace {

bool MaterialLegacy() {
    static const bool legacy = std::getenv("PT_MATERIAL_LEGACY") != nullptr;
    return legacy;
}

bool LisaHairShadow(std::string_view path, const FmdlMaterial& material) {
    return path == "/Assets/sh/chara/och/Scenes/och0_main0_def.fmdl" && material.name == "hair";
}

const std::string* FindTexture(const FmdlMaterial& material, std::string_view prefix) {
    for (const auto& [name, path] : material.textures) {
        if (name.starts_with(prefix)) {
            return &path;
        }
    }
    return nullptr;
}

bool HasVector(const FmdlMaterial& material, std::string_view name) {
    for (const auto& [n, v] : material.vectors) {
        if (n == name) {
            return true;
        }
    }
    return false;
}

uint32_t MultiMaterialCount(const FmdlMaterial& material) {
    if (material.technique.find("_4MT") != std::string::npos) {
        return 4;
    }
    if (material.technique.find("_3MT") != std::string::npos) {
        return 3;
    }
    if (material.technique.find("_2MT") != std::string::npos) {
        return 2;
    }
    return 1;
}

}

RenderPass ClassifyMaterial(const FmdlMaterial& material) {
    if (material.shader.starts_with("fox_3ddc")) {
        return RenderPass::Decal;
    }
    if (material.shader.starts_with("fox_3dfw_constant") || material.shader.starts_with("tpp_3dfw_constant")) {
        return RenderPass::Unlit;
    }
    if (material.shader.starts_with("fox_3dfw")) {
        return RenderPass::Transparent;
    }
    return RenderPass::Opaque;
}

uint32_t MaterialKindOf(const FmdlMaterial& material) {
    if (material.shader.starts_with("fox_3dfw_constant")) {
        return gpu::kKindConstant;
    }
    if (material.shader.starts_with("tpp_3dfw_constant")) {
        return gpu::kKindSky;
    }
    if (material.shader.starts_with("fox_3dfw_glass")) {
        return gpu::kKindGlass;
    }
    if (material.shader.starts_with("fox_3dfw_parallax")) {
        return gpu::kKindParallax;
    }
    if (material.shader.starts_with("fox_3dfw")) {
        return gpu::kKindGlass;
    }
    if (material.shader.starts_with("fox_3dndw_shadow")) {
        return gpu::kKindShadowOnly;
    }
    return gpu::kKindDeferred;
}

bool ModelCache::ViewReflection(const FmdlMaterial& material) {
    if (!reflection_loaded_) {
        reflection_loaded_ = true;
        if (auto data = vfs_.ReadFile("/Assets/fox/effect/gr_pic/material_params.fmtt"); data && data->size() >= 256 * 32) {
            reflection_.resize(256);
            for (size_t i = 0; i < reflection_.size(); ++i) {
                std::memcpy(&reflection_[i], data->data() + i * 32 + 8, sizeof(float));
            }
        } else {
            LogWarn("model cache: material_params.fmtt missing, no view reflection");
        }
    }
    if (reflection_.empty()) {
        return false;
    }
    for (const char* name : {"MatParamIndex_0", "MatParamIndex_1", "MatParamIndex_2", "MatParamIndex_3"}) {
        if (!HasVector(material, name)) {
            continue;
        }
        const int index = static_cast<int>(material.Vector(name).x);
        if (index >= 0 && index < 256 && reflection_[index] > 0.0f) {
            return true;
        }
    }
    return false;
}

uint32_t ModelCache::LoadTexture(const std::string& path, uint32_t fallback, bool raw) {
    if (path.empty()) {
        return fallback;
    }
    bool ok = false;
    const uint32_t index = textures_.LoadFox(vfs_.Textures(), path, &ok, raw);
    if (!ok) {
        ++missing_textures_;
        return fallback;
    }
    return index;
}

MaterialGpu ModelCache::BuildMaterial(const FmdlMaterial& m, uint32_t alpha_flags) {
    MaterialGpu gpu;
    gpu.kind = MaterialKindOf(m);
    gpu.normal = TextureManager::kFlatNormal;
    gpu.specular = TextureManager::kBlack;
    auto texture = [&](std::string_view prefix, uint32_t fallback, bool raw = false) {
        const std::string* path = FindTexture(m, prefix);
        return path ? LoadTexture(*path, fallback, raw) : fallback;
    };
    auto deferred = [&](std::string_view prefix, uint32_t absent, uint32_t missing) {
        const std::string* path = FindTexture(m, prefix);
        return path ? LoadTexture(*path, missing) : absent;
    };
    auto scalar = [&](std::string_view name, float fallback) { return HasVector(m, name) ? m.Vector(name).x : fallback; };
    gpu.indices = glm::vec4(scalar("MatParamIndex_0", 0.0f), scalar("MatParamIndex_1", 0.0f), scalar("MatParamIndex_2", 0.0f),
                            scalar("MatParamIndex_3", 0.0f)) / 255.0f;
    if (alpha_flags & gpu::kMatAlphaTest) {
        gpu.flags |= alpha_flags;
        gpu.params.x = 1.0f;
    }
    switch (gpu.kind) {
    case gpu::kKindConstant:
    case gpu::kKindSky:
        if (m.shader == "fox_3dfw_constant_srgb" || !FindTexture(m, "Mask_Tex")) {
            gpu.albedo = texture("Base_Tex", TextureManager::kWhite);
            gpu.flags |= gpu::kMatSrgbBase;
        } else {
            gpu.albedo = texture("Mask_Tex", TextureManager::kWhite, true);
        }
        gpu.albedo_factor = m.Vector("SelfColor", glm::vec4(1.0f));
        gpu.params.y = scalar("SelfColorIntensity", 1.0f);
        return gpu;
    case gpu::kKindGlass:
        gpu.albedo = texture("Base_Tex", TextureManager::kWhite, true);
        if (const std::string* path = FindTexture(m, "NormalMap_Tex")) {
            gpu.normal = LoadTexture(*path, TextureManager::kFlatNormal);
            gpu.flags |= gpu::kMatNormalMap;
        }
        gpu.albedo_factor = m.Vector("GlassColor", glm::vec4(1.0f));
        gpu.extra = m.Vector("ReflectionColor", glm::vec4(1.0f));
        gpu.params.y = scalar("GlassRoughness", 0.0f);
        gpu.aux0 = 0;
        if (m.shader.find("commonref") != std::string::npos) {
            gpu.flags |= gpu::kMatCommonReflection;
        } else if (const std::string* path = FindTexture(m, "GlassReflection_Tex")) {
            const uint32_t slot = textures_.CubeSlot(LoadTexture(*path, TextureManager::kBlack, true));
            gpu.aux0 = slot == TextureManager::kNoCube ? 0 : slot;
        }
        return gpu;
    case gpu::kKindParallax: {
        static constexpr const char* kMirrorDirt = "/Assets/sh/environ/object/shsb/bath/shsb_bath001/sourceimages/shsb_bath001_dc_bsm_alp.ftex";
        gpu.albedo = texture("Base_Tex", TextureManager::kWhite, true);
        gpu.aux0 = texture("Height_Tex", TextureManager::kBlack, true);
        gpu.aux1 = texture("Mask_Tex", TextureManager::kWhite, true);
        gpu.aux2 = LoadTexture(kMirrorDirt, TextureManager::kBlack, true);
        gpu.params.z = scalar("HeightScale", 0.0f);
        gpu.params.w = gpu.aux2 != TextureManager::kBlack ? 1.0f : 0.0f;
        return gpu;
    }
    default: break;
    }
    if (gpu.kind == gpu::kKindShadowOnly) {
        gpu.albedo = texture("Mask_Tex", TextureManager::kWhite);
        return gpu;
    }
    if (m.shader.starts_with("fox_3ddf_tempolary")) {
        gpu.albedo = texture("Base_Tex", TextureManager::kWhite);
        if (m.shader == "fox_3ddf_tempolary_albedoview" && !MaterialLegacy()) {
            gpu.flags |= gpu::kMatAlbedoView;
        }
        if (HasVector(m, "TempolaryBaseColor")) {
            gpu.albedo_factor = glm::vec4(glm::vec3(m.Vector("TempolaryBaseColor")), 1.0f);
            gpu.flags |= gpu::kMatConstantColor;
        }
        return gpu;
    }
    gpu.albedo = deferred("Base_Tex", TextureManager::kWhite, TextureManager::kGreySrgb);
    gpu.normal = deferred("NormalMap_Tex", TextureManager::kFlatNormal, TextureManager::kGrey);
    if (gpu.normal != TextureManager::kFlatNormal) {
        gpu.flags |= gpu::kMatNormalMap;
    }
    gpu.specular = deferred("SpecularMap_Tex", TextureManager::kBlack, TextureManager::kGrey);
    if (m.shader.starts_with("fox_3ddc")) {
        gpu.flags |= gpu::kMatDecal;
    } else if (ViewReflection(m)) {
        gpu.flags |= gpu::kMatViewReflection;
    }
    if (m.technique.find("_NC_") != std::string::npos || m.technique.ends_with("_NC")) {
        gpu.flags |= gpu::kMatTwoSided;
    }
    if (m.technique.find("DirectiveAlpha") != std::string::npos) {
        gpu.flags |= gpu::kMatDirectiveAlpha;
        gpu.params.y = scalar("StartFadeDot", 1.0f);
        gpu.extra.w = scalar("EndFadeDot", 0.0f);
    }
    const uint32_t multi = MultiMaterialCount(m);
    if (multi > 1) {
        gpu.aux0 = deferred("MatParamMap_Tex", TextureManager::kBlack, TextureManager::kGrey);
        gpu.flags |= (multi - 1) << gpu::kMatMultiShift;
    }
    const bool layer = m.shader.starts_with("fox_3ddf_layer") && m.technique.find("MaskUV") != std::string::npos && !MaterialLegacy();
    if (const std::string* path = FindTexture(m, "SubNormalMap_Tex")) {
        gpu.aux1 = LoadTexture(*path, TextureManager::kGrey);
        gpu.aux2 = deferred("SubNormalMask_Tex", TextureManager::kWhite, TextureManager::kGrey);
        gpu.extra = glm::vec4(scalar("SubNormal_Blend", 0.0f), scalar("URepeat_UV", 1.0f), scalar("VRepeat_UV", 1.0f), 0.0f);
        if (layer) {
            gpu.extra.y = scalar("URepeat_SubNorm_UV", 1.0f);
            gpu.extra.z = scalar("VRepeat_SubNorm_UV", 1.0f);
        }
        if (gpu.aux1 != TextureManager::kFlatNormal) {
            gpu.flags |= gpu::kMatSubNormal;
        }
    }
    if (HasVector(m, "Incidence_Color")) {
        gpu.flags |= gpu::kMatIncidence;
        gpu.params.z = scalar("Incidence_Roughness", 1.0f);
        gpu.albedo_factor = m.Vector("Incidence_Color");
    }
    if (HasVector(m, "MinRoughness")) {
        gpu.flags |= gpu::kMatMicroRoughness;
        gpu.params.z = scalar("MinRoughness", 1.0f);
        gpu.params.w = scalar("RoughnessFrequency", 1.0f);
    }
    const bool translucent_slot = m.technique.starts_with("fox3DDF_") &&
                                  (m.technique.find("_Translucent") != std::string::npos || m.technique.find("_Skin") != std::string::npos ||
                                   m.technique.find("_Eye") != std::string::npos || m.technique.find("_Hair") != std::string::npos);
    if (translucent_slot && !(gpu.flags & gpu::kMatSubNormal)) {
        gpu.aux1 = deferred("Translucent_Tex", TextureManager::kGrey, TextureManager::kGrey);
        gpu.flags |= gpu::kMatTranslucentTex;
    }
    if (layer) {
        gpu.flags |= gpu::kMatLayer;
        gpu.aux0 = texture("Layer_Tex", TextureManager::kBlack);
        gpu.aux2 = texture("LayerMask_Tex", TextureManager::kBlack);
        gpu.albedo_factor = glm::vec4(scalar("URepeat_UV", 1.0f), scalar("VRepeat_UV", 1.0f), scalar("UShift_UV", 0.0f), scalar("VShift_UV", 0.0f));
    }
    if (m.shader == "fox_3ddf_normal_wave_directivealpha" && !MaterialLegacy()) {
        const glm::vec3 dir(m.Vector("WindDir", glm::vec4(0.0f)));
        const glm::vec3 offset(m.Vector("WindOffset", glm::vec4(0.0f)));
        const float amplitude = scalar("WindAmplitude", 0.0f);
        const float wave = std::sin(scalar("WindAnimTime", 0.0f) + offset.x) * amplitude * scalar("WindRandAmplitude", 0.0f);
        const glm::vec3 bend = amplitude * offset + (glm::dot(dir, dir) > 0.0f ? glm::normalize(dir) : glm::vec3(0.0f)) * wave;
        gpu.flags |= gpu::kMatNormalWave;
        gpu.albedo_factor = glm::vec4(bend, scalar("WeightDiffusion", 1.0f));
        gpu.params.w = scalar("WeightOffset", 0.0f);
        gpu.extra.x = scalar("Specular_Value", 0.0f);
        gpu.extra.y = scalar("Roughness_Value", 0.0f);
        gpu.extra.z = scalar("Translucent_Value", 0.0f);
    }
    if (m.shader == "fox_3ddf_reflector" && !MaterialLegacy()) {
        gpu.flags |= gpu::kMatReflector;
        gpu.params.z = scalar("AnglePow", 1.0f);
    }
    static const bool hair_legacy = std::getenv("PT_HAIR_LEGACY") != nullptr;
    if (m.shader == "fox_3ddf_hair" && !hair_legacy) {
        gpu.flags = (gpu.flags & ~gpu::kMatIncidence) | gpu::kMatHair;
        gpu.aux0 = texture("Shift_Tex", TextureManager::kBlack);
        gpu.extra = glm::vec4(scalar("Anistropic_Diffusion", 0.0f), scalar("HairShiftScale", 0.0f), scalar("URepeat_UV", 1.0f),
                              scalar("VRepeat_UV", 1.0f));
    }
    if (m.shader.starts_with("fox_3ddf_lightcover")) {
        gpu.flags |= gpu::kMatLightCover;
        gpu.aux0 = texture("InternalNormalMap_Tex", TextureManager::kFlatNormal);
        gpu.aux1 = texture("CoverTranslucentMap_Tex", TextureManager::kWhite, true);
    }
    if (m.shader.starts_with("fox_3ddf_eye")) {
        gpu.flags |= gpu::kMatEye;
        gpu.aux0 = texture("Base_Tex2", TextureManager::kWhite, true);
        gpu.aux2 = texture("LensHeight_Tex", TextureManager::kBlack, true);
        gpu.extra.x = std::bit_cast<float>(texture("ViewReflection_Tex", TextureManager::kBlack, true));
        gpu.params.z = scalar("HeightScale", 0.0f);
    }
    return gpu;
}

std::unordered_set<std::string> ModelCache::Paths() const {
    std::unordered_set<std::string> paths;
    for (const auto& [path, entry] : entries_) {
        paths.insert(path);
    }
    return paths;
}

const ModelEntry* ModelCache::Get(const std::string& path, FmdlModel* parsed) {
    return Load(path, path, nullptr, parsed);
}

const ModelEntry* ModelCache::GetSubset(const std::string& path, const std::string& key, const std::function<bool(const Vertex&)>& keep,
                                        const std::function<void(Vertex&)>& transform) {
    return Load(path, key, &keep, nullptr, transform ? &transform : nullptr);
}

const ModelEntry* ModelCache::Load(const std::string& path, const std::string& key, const std::function<bool(const Vertex&)>* keep,
                                   FmdlModel* parsed, const std::function<void(Vertex&)>* transform) {
    auto it = entries_.find(key);
    if (it != entries_.end()) {
        return it->second.mesh ? &it->second : nullptr;
    }
    ModelEntry entry;
    FmdlModel read;
    FmdlModel& model = parsed ? *parsed : read;
    if (!parsed) {
        auto bytes = vfs_.ReadFile(path);
        if (!bytes || !LoadFmdl(*bytes, path, read)) {
            ++failed_;
            entries_.emplace(key, std::move(entry));
            return nullptr;
        }
    }
    if (keep) {
        std::vector<uint32_t> indices;
        for (SubMesh& sub : model.mesh.submeshes) {
            const uint32_t first = static_cast<uint32_t>(indices.size());
            for (uint32_t i = 0; i + 2 < sub.index_count; i += 3) {
                const uint32_t* tri = model.mesh.indices.data() + sub.first_index + i;
                bool all = true;
                for (int k = 0; k < 3 && all; ++k) {
                    all = (*keep)(model.mesh.vertices[static_cast<size_t>(static_cast<int64_t>(tri[k]) + sub.vertex_offset)]);
                }
                if (all) indices.insert(indices.end(), tri, tri + 3);
            }
            sub.first_index = first;
            sub.index_count = static_cast<uint32_t>(indices.size()) - first;
        }
        model.mesh.indices = std::move(indices);
    }
    if (transform && !model.mesh.vertices.empty()) {
        model.mesh.bounds_min = glm::vec3(1e30f);
        model.mesh.bounds_max = glm::vec3(-1e30f);
        for (Vertex& v : model.mesh.vertices) {
            (*transform)(v);
            model.mesh.bounds_min = glm::min(model.mesh.bounds_min, v.position);
            model.mesh.bounds_max = glm::max(model.mesh.bounds_max, v.position);
        }
    }
    std::vector<int32_t> alpha_tested[2] = {std::vector<int32_t>(model.materials.size(), -1), std::vector<int32_t>(model.materials.size(), -1)};
    for (const FmdlMaterial& m : model.materials) {
        entry.material_indices.push_back(textures_.AddMaterial(BuildMaterial(m, 0)));
    }
    bool skinned = false;
    for (size_t i = 0; i < model.mesh.submeshes.size(); ++i) {
        SubMesh& sub = model.mesh.submeshes[i];
        const uint32_t local = sub.material;
        const FmdlMeshInfo& info = model.meshes[i];
        if (local < model.materials.size()) {
            const FmdlMaterial& m = model.materials[local];
            sub.pass = ClassifyMaterial(m);
            sub.material = entry.material_indices[local];
            if ((info.render_flags & 0x80) && sub.pass != RenderPass::Unlit) {
                /* MASK_PASS: mesh flag bit 7 alone selects the dithered Bayer alpha reference, bit 14 the fixed 64/255 (states 0xCBDF50). */
                const bool dithered = (info.render_flags & 0x4000) == 0;
                int32_t& slot = alpha_tested[dithered ? 1 : 0][local];
                if (slot < 0) {
                    const uint32_t alpha_flags = gpu::kMatAlphaTest | (dithered ? gpu::kMatAlphaDither : 0u);
                    slot = static_cast<int32_t>(textures_.AddMaterial(BuildMaterial(m, alpha_flags)));
                }
                sub.material = static_cast<uint32_t>(slot);
            }
            const uint32_t kind = MaterialKindOf(m);
            sub.kind = static_cast<uint8_t>(kind);
            const uint32_t f = info.render_flags;
            sub.shadow = (f & 0x140) == 0 && ((f & 0x10) == 0 || (f & 0xF) == 0);
            if (LisaHairShadow(path, m)) {
                sub.shadow = true;
                sub.material = textures_.AddMaterial(BuildMaterial(m, gpu::kMatAlphaTest | gpu::kMatAlphaDither | gpu::kMatLisaHairShadow));
            }
        } else {
            sub.material = 0;
        }
        static const bool constant_two_sided = std::getenv("PT_CONSTANT_TWO_SIDED") != nullptr;
        const bool culled_forward = sub.kind == gpu::kKindConstant && !constant_two_sided;
        sub.double_sided = (info.render_flags & 0x20) != 0 ||
                           ((sub.pass == RenderPass::Transparent || sub.pass == RenderPass::Unlit) && !culled_forward);
        sub.shadow_double_sided = (info.render_flags & 0x20) != 0;
        skinned = skinned || sub.skinned;
    }
    entry.bone_names = model.bone_names;
    entry.bone_parents = model.bone_parent;
    entry.bone_bind_world = model.bone_world;
    entry.skinned = skinned && !model.bone_names.empty();
    entry.mesh = scene_.Upload(model.mesh);
    if (entry.mesh) {
        entry.mesh->skinned = entry.skinned;
    }
    entry.materials = std::move(model.materials);
    entry.raw_positions = std::move(model.raw_positions);
    ++loaded_;
    auto [slot, inserted] = entries_.emplace(key, std::move(entry));
    return slot->second.mesh ? &slot->second : nullptr;
}

bool ModelCache::ReadMesh(const std::string& path, MeshData& out) {
    auto bytes = vfs_.ReadFile(path);
    FmdlModel model;
    if (!bytes || !LoadFmdl(*bytes, path, model)) return false;
    out = std::move(model.mesh);
    return true;
}

void ModelCache::Clear() {
    for (auto& [path, entry] : entries_) {
        if (entry.mesh) {
            scene_.Destroy(*entry.mesh);
        }
    }
    entries_.clear();
}

}
