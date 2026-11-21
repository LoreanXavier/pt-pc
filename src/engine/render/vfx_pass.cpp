#include "engine/render/vfx_pass.h"

#include "engine/render/pipeline_cache_store.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <numeric>

#include "engine/core/log.h"
#include "engine/fs/vfs.h"

namespace pt {

namespace {

struct Push {
    glm::mat4 view_projection{1.0f};
    glm::vec4 eye{0.0f};
    glm::vec4 frame{0.0f};
    glm::uvec4 ids{0u};
};

constexpr uint32_t kInitialQuads = 4096;

uint32_t InitialQuads() {
    static const uint32_t quads = [] {
        const char* value = std::getenv("PT_VFX_INITIAL_QUADS");
        const int n = value ? std::atoi(value) : 0;
        return n > 0 ? static_cast<uint32_t>(n) : kInitialQuads;
    }();
    return quads;
}

struct FogBlock {
    glm::vec4 fog[8]{};
    glm::uvec4 mode{0u};
};

constexpr uint64_t kFogStride = 256;
constexpr const char* kShaders[2] = {"vfx_particle.vert", "vfx_particle.frag"};

gpu::BlendState BlendState(vfx::BlendMode mode, bool offscreen, bool scene) {
    using gpu::BlendFactor;
    using gpu::BlendOp;
    gpu::BlendState s;
    s.write_mask = gpu::ColorComponent::R | gpu::ColorComponent::G | gpu::ColorComponent::B;
    if (offscreen) {
        s.write_mask |= gpu::ColorComponent::A;
    }
    s.enable = true;
    s.src_alpha = BlendFactor::Zero;
    s.dst_alpha = BlendFactor::One;
    s.alpha_op = BlendOp::Add;
    s.color_op = BlendOp::Add;
    s.src_color = BlendFactor::Zero;
    s.dst_color = BlendFactor::Zero;
    switch (mode) {
    case vfx::BlendMode::Alpha:
        s.src_color = BlendFactor::One;
        s.dst_color = BlendFactor::OneMinusSrcAlpha;
        if (offscreen) {
            s.dst_alpha = BlendFactor::OneMinusSrcAlpha;
        } else if (scene) {
            s.write_mask |= gpu::ColorComponent::A;
            s.src_alpha = BlendFactor::One;
            s.dst_alpha = BlendFactor::OneMinusSrcAlpha;
        }
        break;
    case vfx::BlendMode::Add:
        s.src_color = BlendFactor::One;
        s.dst_color = BlendFactor::One;
        break;
    case vfx::BlendMode::Sub:
        s.src_color = BlendFactor::One;
        s.dst_color = BlendFactor::One;
        s.color_op = BlendOp::ReverseSubtract;
        break;
    case vfx::BlendMode::Mul:
        s.src_color = BlendFactor::DstColor;
        s.dst_color = BlendFactor::Zero;
        break;
    case vfx::BlendMode::Min:
        s.src_color = BlendFactor::One;
        s.dst_color = BlendFactor::One;
        s.color_op = BlendOp::Min;
        break;
    case vfx::BlendMode::Opaque:
        s.enable = false;
        if (scene) {
            s.write_mask |= gpu::ColorComponent::A;
        }
        break;
    }
    return s;
}

}

bool VfxPass::Init(Renderer& renderer, TextureManager& textures, Vfs& vfs) {
    renderer_ = &renderer;
    textures_ = &textures;
    vfs_ = &vfs;
    gpu_ = &renderer.Gpu();
    gpu::BindingLayoutDesc layout;
    layout.bindings = {{0, gpu::BindingType::StorageBuffer, 1, gpu::ShaderStage::Vertex},
                       {1, gpu::BindingType::CombinedImageSampler, 1, gpu::ShaderStage::Fragment},
                       {2, gpu::BindingType::CombinedImageSampler, 1, gpu::ShaderStage::Fragment},
                       {3, gpu::BindingType::UniformBuffer, 1, gpu::ShaderStage::Fragment}};
    set_layout_ = gpu_->CreateBindingLayout(layout);
    if (!set_layout_) {
        LogError("vfx: set layout failed");
        return false;
    }
    gpu::BindingPoolDesc pool;
    pool.sizes = {{gpu::BindingType::StorageBuffer, 4 * Renderer::kFramesInFlight},
                  {gpu::BindingType::CombinedImageSampler, 8 * Renderer::kFramesInFlight},
                  {gpu::BindingType::UniformBuffer, 4 * Renderer::kFramesInFlight}};
    pool.max_sets = 4 * Renderer::kFramesInFlight;
    pool_ = gpu_->CreateBindingPool(pool);
    if (!pool_) {
        LogError("vfx: descriptor pool failed");
        return false;
    }
    gpu::PipelineLayoutDesc pl;
    pl.sets = {textures.BindingLayout(), set_layout_};
    pl.push_bytes = sizeof(Push);
    pl.push_stages = gpu::ShaderStage::Vertex | gpu::ShaderStage::Fragment;
    layout_ = gpu_->CreatePipelineLayout(pl);
    if (!layout_) {
        LogError("vfx: pipeline layout failed");
        return false;
    }
    gpu::SamplerDesc sampler_info;
    sampler_info.mag = gpu::Filter::Nearest;
    sampler_info.min = gpu::Filter::Nearest;
    sampler_info.mipmap = gpu::MipmapMode::Nearest;
    sampler_info.address_u = gpu::AddressMode::ClampToEdge;
    sampler_info.address_v = gpu::AddressMode::ClampToEdge;
    sampler_info.address_w = gpu::AddressMode::ClampToEdge;
    sampler_info.max_anisotropy = 0.0f;
    sampler_info.max_lod = 0.0f;
    sampler_ = gpu_->CreateSampler(sampler_info);
    if (!sampler_) {
        return false;
    }
    sampler_info.mag = gpu::Filter::Linear;
    sampler_info.min = gpu::Filter::Linear;
    sampler_info.address_u = gpu::AddressMode::Repeat;
    sampler_info.address_v = gpu::AddressMode::Repeat;
    sampler_info.address_w = gpu::AddressMode::Repeat;
    scene_sampler_ = gpu_->CreateSampler(sampler_info);
    if (!scene_sampler_) {
        return false;
    }
    shaders_ = gpu_->PreloadShaders(kShaders);
    if (!shaders_) {
        gpu_->ReleaseShaders(kShaders);
        return false;
    }
    for (FrameSlot& slot : slots_) {
        const gpu::BindingLayoutHandle layouts[4] = {set_layout_, set_layout_, set_layout_, set_layout_};
        gpu::BindingSetHandle sets[4] = {};
        if (!gpu_->AllocateBindingSets(pool_, layouts, sets)) {
            LogError("vfx: descriptor sets failed");
            return false;
        }
        slot.forward = sets[0];
        slot.filter = sets[1];
        slot.effects = sets[2];
        slot.depth_only = sets[3];
        if (!gpu_->CreateBuffer({sizeof(vfx::Quad) * InitialQuads(), gpu::BufferUsage::StorageBuffer, true}, slot.quads)) {
            return false;
        }
        if (!gpu_->CreateBuffer({3 * kFogStride, gpu::BufferUsage::UniformBuffer, true}, slot.fog)) {
            return false;
        }
        std::memset(slot.fog.mapped, 0, 3 * kFogStride);
        gpu_->FlushMapped(slot.fog, 0, 3 * kFogStride);
    }
    return true;
}

void VfxPass::Shutdown() {
    if (!gpu_) {
        return;
    }
    std::lock_guard lock(pipelines_mutex_);
    for (auto& [key, pipeline] : pipelines_) {
        gpu_->DestroyPipeline(pipeline);
    }
    pipelines_.clear();
    for (FrameSlot& slot : slots_) {
        gpu_->DestroyBuffer(slot.quads);
        gpu_->DestroyBuffer(slot.fog);
    }
    if (shaders_) {
        gpu_->ReleaseShaders(kShaders);
        shaders_ = false;
    }
    gpu_->DestroySampler(sampler_);
    gpu_->DestroySampler(scene_sampler_);
    gpu_->DestroyPipelineLayout(layout_);
    gpu_->DestroyBindingPool(pool_);
    gpu_->DestroyBindingLayout(set_layout_);
    gpu_ = nullptr;
}

uint32_t VfxPass::Texture(const std::string& path) {
    if (path.empty() || !textures_ || !vfs_) {
        return TextureManager::kWhite;
    }
    if (auto it = texture_cache_.find(path); it != texture_cache_.end()) {
        return it->second;
    }
    bool ok = false;
    const auto started = std::chrono::steady_clock::now();
    const uint32_t index = textures_->LoadFox(vfs_->Textures(), path, &ok);
    if (!ok) {
        LogWarn("vfx: texture {} not found", path);
    }
    if (const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(); ms > 5.0) {
        LogInfo("vfx: texture {} loaded in {:.1f} ms", path, ms);
    }
    texture_cache_[path] = index;
    return index;
}

void VfxPass::DecodeAhead(const std::vector<std::string>& paths) {
    if (!textures_ || !vfs_) {
        return;
    }
    std::vector<std::string> pending;
    for (const std::string& path : paths) {
        if (!path.empty() && !texture_cache_.contains(path)) {
            pending.push_back(path);
        }
    }
    textures_->DecodeAhead(vfs_->Textures(), pending);
}

uint32_t VfxPass::Cube(const std::string& path) {
    if (path.empty() || !textures_ || !vfs_) {
        return vfx::kNoTexture;
    }
    if (auto it = cube_cache_.find(path); it != cube_cache_.end()) {
        return it->second;
    }
    bool ok = false;
    const uint32_t index = textures_->LoadFox(vfs_->Textures(), path, &ok, true);
    const uint32_t slot = ok ? textures_->CubeSlot(index) : TextureManager::kNoCube;
    if (slot == TextureManager::kNoCube) {
        LogWarn("vfx: reflection cube {} {}", path, ok ? "is not a cube map" : "not found");
    }
    const uint32_t result = slot == TextureManager::kNoCube ? vfx::kNoTexture : slot;
    cube_cache_[path] = result;
    return result;
}

void VfxPass::Submit(vfx::RenderList& list) {
    if (textures_ && vfs_) {
        textures_->PumpDecoded(vfs_->Textures(), 1);
    }
    std::swap(list_, list);
    list.Clear();
    order_.resize(list_.draws.size());
    std::iota(order_.begin(), order_.end(), 0u);
    std::stable_sort(order_.begin(), order_.end(), [&](uint32_t a, uint32_t b) {
        const vfx::Draw& da = list_.draws[a];
        const vfx::Draw& db = list_.draws[b];
        if (da.layer != db.layer) {
            return da.layer < db.layer;
        }
        if (da.layer == vfx::Layer::Screen) {
            return da.priority < db.priority;
        }
        return da.depth > db.depth;
    });
    ++generation_;
}

gpu::PipelineHandle VfxPass::Pipeline(const PipelineKey& key) {
    {
        std::lock_guard lock(pipelines_mutex_);
        for (const auto& [k, p] : pipelines_) {
            if (k == key) {
                return p;
            }
        }
    }
    gpu::GraphicsPipelineDesc desc;
    desc.vertex = kShaders[0];
    desc.fragment = kShaders[1];
    desc.layout = layout_;
    desc.fixed_state = true;
    desc.cull = key.cull ? gpu::CullMode::Back : gpu::CullMode::None;
    desc.depth_test = key.depth_test;
    desc.depth_write = key.depth_write;
    desc.depth_compare = gpu::CompareOp::GreaterOrEqual;
    const gpu::BlendState attachment = BlendState(static_cast<vfx::BlendMode>(key.blend), key.offscreen, key.depth_test && !key.offscreen);
    const uint32_t color_count = key.color == gpu::Format::Undefined ? 0u : key.offscreen ? 2u : 1u;
    desc.colors.assign(color_count, key.color);
    desc.blend_states.assign(color_count, attachment);
    desc.depth = key.depth;
    gpu::PipelineHandle pipeline = gpu_->CreateGraphicsPipeline(desc);
    std::lock_guard lock(pipelines_mutex_);
    for (const auto& [k, p] : pipelines_) {
        if (k == key) {
            if (pipeline) gpu_->DestroyPipeline(pipeline);
            return p;
        }
    }
    pipelines_.emplace_back(key, pipeline);
    return pipeline;
}

void VfxPass::AddPipelineWarmup(vk::PipelineWarmup& warmup, gpu::Format color, gpu::Format depth, gpu::Format filter_color) {
    if (!gpu_ || !shaders_) {
        return;
    }
    std::vector<PipelineKey> keys;
    for (uint8_t blend = 0; blend <= static_cast<uint8_t>(vfx::BlendMode::Opaque); ++blend) {
        for (const bool cull : {false, true}) {
            keys.push_back({color, depth, blend, true, cull, false});
            const auto mode = static_cast<vfx::BlendMode>(blend);
            if (mode == vfx::BlendMode::Alpha || mode == vfx::BlendMode::Add || mode == vfx::BlendMode::Sub) {
                keys.push_back({color, depth, blend, true, cull, true});
            }
            keys.push_back({gpu::Format::Undefined, depth, blend, true, cull, false, true});
            keys.push_back({filter_color, gpu::Format::Undefined, blend, false, cull, false});
        }
    }
    for (const PipelineKey& key : keys) {
        warmup.Add([this, key]() -> uint32_t { return Pipeline(key) ? 1u : 0u; });
    }
}

void VfxPass::WriteSet(FrameSlot& slot, gpu::BindingSetHandle set, gpu::ImageViewHandle view, gpu::Layout depth_layout,
                       gpu::ImageViewHandle scene, uint64_t fog_offset) {
    const gpu::BufferBinding buffer{slot.quads.buffer};
    const gpu::ImageBinding image{view, depth_layout, sampler_};
    const gpu::ImageBinding scene_image{scene, gpu::Layout::ShaderReadOnly, scene_sampler_};
    const gpu::BufferBinding fog{slot.fog.buffer, fog_offset, sizeof(FogBlock)};
    const gpu::BindingWrite writes[4] = {{set, 0, 0, gpu::BindingType::StorageBuffer, {}, {&buffer, 1}},
                                         {set, 1, 0, gpu::BindingType::CombinedImageSampler, {&image, 1}},
                                         {set, 2, 0, gpu::BindingType::CombinedImageSampler, {&scene_image, 1}},
                                         {set, 3, 0, gpu::BindingType::UniformBuffer, {}, {&fog, 1}}};
    gpu_->WriteBindings(writes);
}

bool VfxPass::Upload(uint32_t frame_index) {
    const vk::AllocTag tag("effects");
    FrameSlot& slot = slots_[frame_index % Renderer::kFramesInFlight];
    if (slot.uploaded == generation_) {
        return true;
    }
    const uint64_t bytes = sizeof(vfx::Quad) * list_.quads.size();
    if (bytes > slot.quads.size) {
        slot.forward_view = {};
        slot.forward_scene = {};
        slot.filter_view = {};
        slot.filter_scene = {};
        slot.effects_depth_view = {};
        slot.effects_scene = {};
        slot.depth_only_scene = {};
        gpu_->DestroyBuffer(slot.quads);
        const uint64_t size = std::max<uint64_t>(bytes + bytes / 2, sizeof(vfx::Quad) * InitialQuads());
        if (!gpu_->CreateBuffer({size, gpu::BufferUsage::StorageBuffer, true}, slot.quads)) {
            return false;
        }
        LogDebug("vfx: quad buffer of frame slot {} grown to {} quads for {}", frame_index % Renderer::kFramesInFlight,
                 size / sizeof(vfx::Quad), list_.quads.size());
    }
    if (bytes) {
        std::memcpy(slot.quads.mapped, list_.quads.data(), bytes);
        gpu_->FlushMapped(slot.quads, 0, bytes);
    }
    slot.uploaded = generation_;
    return true;
}

uint32_t VfxPass::DrawLayer(gpu::CommandList& cmd, gpu::BindingSetHandle set, vfx::Layer layer, gpu::Format color, gpu::Format depth,
                            bool depth_test, uint32_t mode, const glm::mat4& view_projection, const glm::vec3& eye, float exposure,
                            float near_plane, gpu::Extent2D extent, const std::function<void()>* copy_scene, uint32_t subset,
                            uint32_t* recorded) {
    uint32_t copies = 0;
    bool bound = false;
    bool copy_stale = true;
    const float refraction_scale = static_cast<float>(extent.height) / 1080.0f;
    for (uint32_t index : order_) {
        const vfx::Draw& d = list_.draws[index];
        if (d.layer != layer || d.count == 0) {
            continue;
        }
        static const bool liquid_scene = std::getenv("PT_VFX_LIQUID_SCENE") != nullptr;
        const bool effect_liquid = d.refract && !d.opaque && !liquid_scene;
        const bool buffered = (!d.refract || effect_liquid) &&
                              (d.blend == vfx::BlendMode::Alpha || d.blend == vfx::BlendMode::Add || d.blend == vfx::BlendMode::Sub);
        if ((subset == kVfxScene && buffered) || (subset == kVfxOffscreen && !buffered)) {
            continue;
        }
        const bool offscreen = subset == kVfxOffscreen;
        const gpu::PipelineHandle pipeline = Pipeline({color, depth, static_cast<uint8_t>(d.blend), depth_test, d.cull, offscreen});
        if (!pipeline) {
            continue;
        }
        const bool refract = d.refract && copy_scene && *copy_scene;
        if (refract && copy_stale) {
            (*copy_scene)();
            ++copies;
            copy_stale = false;
            bound = false;
        } else if (!refract) {
            copy_stale = true;
        }
        if (!bound) {
            const gpu::BindingSetHandle sets[2] = {textures_->BindingSet(), set};
            cmd.BindSets(gpu::BindPoint::Graphics, layout_, 0, sets);
            cmd.SetViewport({{0, 0}, extent});
            bound = true;
        }
        cmd.BindPipeline(gpu::BindPoint::Graphics, pipeline);
        Push push;
        push.view_projection = view_projection;
        push.eye = glm::vec4(eye, refract ? refraction_scale : 0.0f);
        push.frame = glm::vec4(exposure, near_plane, 1.0f / static_cast<float>(std::max(1u, extent.width)),
                               1.0f / static_cast<float>(std::max(1u, extent.height)));
        static const uint32_t debug = std::getenv("PT_VFX_DEBUG") ? 1u : 0u;
        push.ids = glm::uvec4(d.first, offscreen ? 2u : mode, static_cast<uint32_t>(d.blend), debug);
        cmd.PushConstants(layout_, gpu::ShaderStage::Vertex | gpu::ShaderStage::Fragment, 0, sizeof(Push), &push);
        cmd.Draw(d.count * 6, 1, 0, 0);
        if (recorded) {
            ++*recorded;
        }
    }
    return copies;
}

void VfxPass::RecordForward(const SceneVfxContext& context) {
    if (context.mirrored || list_.draws.empty() || !gpu_) {
        return;
    }
    const uint32_t frame = context.frame_index % Renderer::kFramesInFlight;
    if (!Upload(frame)) {
        return;
    }
    FrameSlot& slot = slots_[frame];
    const bool offscreen = context.subset == kVfxOffscreen;
    const uint64_t destroys = gpu_->ImageDestroyCount();
    if (offscreen) {
        if (slot.effects_depth_view != context.near_far_depth_view || slot.effects_scene != context.scene_copy_view ||
            slot.effects_destroys != destroys) {
            WriteSet(slot, slot.effects, context.near_far_depth_view, gpu::Layout::ShaderReadOnly, context.scene_copy_view,
                     2 * kFogStride);
            slot.effects_depth_view = context.near_far_depth_view;
            slot.effects_scene = context.scene_copy_view;
            slot.effects_destroys = destroys;
        }
    } else if (slot.forward_view != context.depth_view || slot.forward_scene != context.scene_copy_view || slot.forward_destroys != destroys) {
        WriteSet(slot, slot.forward, context.depth_view, gpu::Layout::DepthReadOnly, context.scene_copy_view, 0);
        slot.forward_view = context.depth_view;
        slot.forward_scene = context.scene_copy_view;
        slot.forward_destroys = destroys;
    }
    FogBlock fog;
    std::copy(std::begin(context.fog), std::end(context.fog), std::begin(fog.fog));
    fog.mode = glm::uvec4(context.fog_mode, 0u, 0u, 0u);
    const uint64_t fog_offset = offscreen ? 2 * kFogStride : 0;
    std::memcpy(static_cast<uint8_t*>(slot.fog.mapped) + fog_offset, &fog, sizeof(fog));
    gpu_->FlushMapped(slot.fog, fog_offset, sizeof(fog));
    const float near_plane = context.projection[3][2];
    const uint32_t copies = DrawLayer(*context.cmd, offscreen ? slot.effects : slot.forward, vfx::Layer::World, context.color_format,
                                      context.depth_format, true, 0, context.view_projection, context.eye, context.exposure, near_plane,
                                      context.extent, context.scene_copy_view && context.copy_scene ? &context.copy_scene : nullptr,
                                      context.subset, context.recorded);
    static const bool debug = std::getenv("PT_VFX_DEBUG") != nullptr;
    if (debug && (generation_ % 120) == 0) {
        LogDebug("vfx: exposure {:.5f} ev {:.2f} near {:.3f}, {} scene copies for liquids", context.exposure,
                 -std::log2(std::max(context.exposure, 1e-8f)), near_plane, copies);
    }
}

bool VfxPass::HasOpaqueLiquids() const {
    return std::any_of(list_.draws.begin(), list_.draws.end(),
                       [](const vfx::Draw& d) { return d.layer == vfx::Layer::World && d.refract && d.opaque && d.count > 0; });
}

void VfxPass::RecordLiquidDepth(const SceneVfxContext& context) {
    if (context.mirrored || !gpu_ || !HasOpaqueLiquids() || !context.scene_copy_view) {
        return;
    }
    const uint32_t frame = context.frame_index % Renderer::kFramesInFlight;
    if (!Upload(frame)) {
        return;
    }
    FrameSlot& slot = slots_[frame];
    const uint64_t destroys = gpu_->ImageDestroyCount();
    if (slot.depth_only_scene != context.scene_copy_view || slot.depth_only_destroys != destroys) {
        WriteSet(slot, slot.depth_only, context.scene_copy_view, gpu::Layout::ShaderReadOnly, context.scene_copy_view, 0);
        slot.depth_only_scene = context.scene_copy_view;
        slot.depth_only_destroys = destroys;
    }
    gpu::CommandList& cmd = *context.cmd;
    const gpu::BindingSetHandle sets[2] = {textures_->BindingSet(), slot.depth_only};
    cmd.BindSets(gpu::BindPoint::Graphics, layout_, 0, sets);
    cmd.SetViewport({{0, 0}, context.extent});
    const float near_plane = context.projection[3][2];
    for (uint32_t index : order_) {
        const vfx::Draw& d = list_.draws[index];
        if (d.layer != vfx::Layer::World || !d.refract || !d.opaque || d.count == 0) {
            continue;
        }
        const gpu::PipelineHandle pipeline = Pipeline({gpu::Format::Undefined, context.depth_format, static_cast<uint8_t>(d.blend), true, d.cull, false, true});
        if (!pipeline) {
            continue;
        }
        cmd.BindPipeline(gpu::BindPoint::Graphics, pipeline);
        Push push;
        push.view_projection = context.view_projection;
        push.eye = glm::vec4(context.eye, 0.0f);
        push.frame = glm::vec4(context.exposure, near_plane, 1.0f / static_cast<float>(std::max(1u, context.extent.width)),
                               1.0f / static_cast<float>(std::max(1u, context.extent.height)));
        push.ids = glm::uvec4(d.first, 0u, static_cast<uint32_t>(d.blend), 0u);
        cmd.PushConstants(layout_, gpu::ShaderStage::Vertex | gpu::ShaderStage::Fragment, 0, sizeof(Push), &push);
        cmd.Draw(d.count * 6, 1, 0, 0);
    }
}

void VfxPass::RecordFilter(const SceneFilterContext& context) {
    if (list_.draws.empty() || !gpu_ || !context.depth_view) {
        return;
    }
    auto wanted = [&](vfx::Layer layer) {
        return (layer == vfx::Layer::Flare && (context.layers & 1u) != 0u) || (layer == vfx::Layer::Screen && (context.layers & 2u) != 0u);
    };
    const bool any = std::any_of(list_.draws.begin(), list_.draws.end(), [&](const vfx::Draw& d) { return wanted(d.layer); });
    if (!any) {
        return;
    }
    const uint32_t frame = context.frame_index % Renderer::kFramesInFlight;
    if (!Upload(frame)) {
        return;
    }
    FrameSlot& slot = slots_[frame];
    const uint64_t destroys = gpu_->ImageDestroyCount();
    if (slot.filter_view != context.depth_view || slot.filter_scene != context.source || slot.filter_destroys != destroys) {
        WriteSet(slot, slot.filter, context.depth_view, gpu::Layout::DepthReadOnly, context.source, kFogStride);
        slot.filter_view = context.depth_view;
        slot.filter_scene = context.source;
        slot.filter_destroys = destroys;
    }
    for (const vfx::Layer layer : {vfx::Layer::Flare, vfx::Layer::Screen}) {
        if (!wanted(layer)) {
            continue;
        }
        DrawLayer(*context.cmd, slot.filter, layer, context.color_format, gpu::Format::Undefined, false, 1, glm::mat4(1.0f), glm::vec3(0.0f),
                  context.exposure, list_.near_plane, context.extent, nullptr);
    }
}

}
