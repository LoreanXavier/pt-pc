#pragma once

#include <glm/glm.hpp>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine/assets/fmdl.h"
#include "engine/fs/vfs.h"
#include "engine/render/scene_renderer.h"
#include "engine/render/texture_manager.h"

namespace pt {

struct ModelEntry {
    std::unique_ptr<GpuMesh> mesh;
    std::vector<FmdlMaterial> materials;
    std::vector<uint32_t> material_indices;
    std::vector<std::string> bone_names;
    std::vector<int16_t> bone_parents;
    std::vector<glm::vec3> bone_bind_world;
    std::vector<glm::vec3> raw_positions;
    bool skinned = false;
};

class ModelCache {
public:
    ModelCache(Vfs& vfs, SceneRenderer& scene, TextureManager& textures) : vfs_(vfs), scene_(scene), textures_(textures) {}

    const ModelEntry* Get(const std::string& path, FmdlModel* parsed = nullptr);
    const ModelEntry* GetSubset(const std::string& path, const std::string& key, const std::function<bool(const Vertex&)>& keep,
                                const std::function<void(Vertex&)>& transform = {});
    std::unordered_set<std::string> Paths() const;
    void Clear();
    bool ReadMesh(const std::string& path, MeshData& out);

    uint32_t Loaded() const { return loaded_; }
    uint32_t Failed() const { return failed_; }
    uint32_t MissingTextures() const { return missing_textures_; }
    SceneRenderer& Scene() { return scene_; }
    TextureManager& Textures() { return textures_; }

private:
    const ModelEntry* Load(const std::string& path, const std::string& key, const std::function<bool(const Vertex&)>* keep,
                           FmdlModel* parsed, const std::function<void(Vertex&)>* transform = nullptr);
    uint32_t LoadTexture(const std::string& path, uint32_t fallback = TextureManager::kWhite, bool raw = false);
    MaterialGpu BuildMaterial(const FmdlMaterial& material, uint32_t alpha_flags);
    bool ViewReflection(const FmdlMaterial& material);

    Vfs& vfs_;
    SceneRenderer& scene_;
    TextureManager& textures_;
    std::vector<float> reflection_;
    bool reflection_loaded_ = false;
    std::unordered_map<std::string, ModelEntry> entries_;
    uint32_t loaded_ = 0;
    uint32_t failed_ = 0;
    uint32_t missing_textures_ = 0;
};

RenderPass ClassifyMaterial(const FmdlMaterial& material);
uint32_t MaterialKindOf(const FmdlMaterial& material);

}
