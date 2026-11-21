#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/render/renderer.h"
#include "engine/render/scene_renderer.h"
#include "engine/render/texture_manager.h"
#include "engine/vfx/vfx_system.h"

namespace pt {

class Vfs;

class VfxPass {
public:
    bool Init(Renderer& renderer, TextureManager& textures, Vfs& vfs);
    void Shutdown();

    uint32_t Texture(const std::string& path);
    void DecodeAhead(const std::vector<std::string>& paths);
    uint32_t Cube(const std::string& path);
    void Submit(vfx::RenderList& list);
    void RecordForward(const SceneVfxContext& context);
    void RecordFilter(const SceneFilterContext& context);
    bool HasOpaqueLiquids() const;
    void RecordLiquidDepth(const SceneVfxContext& context);
    const vfx::RenderList& List() const { return list_; }
    void AddPipelineWarmup(vk::PipelineWarmup& warmup, gpu::Format color, gpu::Format depth, gpu::Format filter_color);

private:
    struct PipelineKey {
        gpu::Format color = gpu::Format::Undefined;
        gpu::Format depth = gpu::Format::Undefined;
        uint8_t blend = 0;
        bool depth_test = false;
        bool cull = false;
        bool offscreen = false;
        bool depth_write = false;
        bool operator==(const PipelineKey&) const = default;
    };

    struct FrameSlot {
        gpu::BufferResource quads;
        gpu::BufferResource fog;
        gpu::BindingSetHandle forward;
        gpu::BindingSetHandle filter;
        gpu::BindingSetHandle effects;
        gpu::BindingSetHandle depth_only;
        gpu::ImageViewHandle depth_only_scene;
        gpu::ImageViewHandle forward_view;
        gpu::ImageViewHandle filter_view;
        gpu::ImageViewHandle effects_depth_view;
        gpu::ImageViewHandle forward_scene;
        gpu::ImageViewHandle filter_scene;
        gpu::ImageViewHandle effects_scene;
        uint64_t forward_destroys = 0;
        uint64_t filter_destroys = 0;
        uint64_t effects_destroys = 0;
        uint64_t depth_only_destroys = 0;
        uint64_t uploaded = 0;
    };

    gpu::PipelineHandle Pipeline(const PipelineKey& key);
    bool Upload(uint32_t frame_index);
    void WriteSet(FrameSlot& slot, gpu::BindingSetHandle set, gpu::ImageViewHandle view, gpu::Layout depth_layout, gpu::ImageViewHandle scene,
                  uint64_t fog_offset);
    uint32_t DrawLayer(gpu::CommandList& cmd, gpu::BindingSetHandle set, vfx::Layer layer, gpu::Format color, gpu::Format depth, bool depth_test,
                       uint32_t mode, const glm::mat4& view_projection, const glm::vec3& eye, float exposure, float near_plane,
                       gpu::Extent2D extent, const std::function<void()>* copy_scene, uint32_t subset = kVfxAll, uint32_t* recorded = nullptr);

    Renderer* renderer_ = nullptr;
    TextureManager* textures_ = nullptr;
    Vfs* vfs_ = nullptr;
    gpu::Device* gpu_ = nullptr;
    gpu::BindingLayoutHandle set_layout_;
    gpu::BindingPoolHandle pool_;
    gpu::PipelineLayoutHandle layout_;
    gpu::SamplerHandle sampler_;
    gpu::SamplerHandle scene_sampler_;
    bool shaders_ = false;
    std::vector<std::pair<PipelineKey, gpu::PipelineHandle>> pipelines_;
    std::mutex pipelines_mutex_;
    FrameSlot slots_[Renderer::kFramesInFlight];
    std::unordered_map<std::string, uint32_t> texture_cache_;
    std::unordered_map<std::string, uint32_t> cube_cache_;
    vfx::RenderList list_;
    std::vector<uint32_t> order_;
    uint64_t generation_ = 1;
};

}
