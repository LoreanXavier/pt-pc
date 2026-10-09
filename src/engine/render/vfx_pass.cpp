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

struct FogBlock {
    glm::vec4 fog[8]{};
    glm::uvec4 mode{0u};
};

constexpr VkDeviceSize kFogStride = 256;

// offscreen: the effect buffer keeps the transmittance in alpha, which alpha blending multiplies by (1 - a) and the additive and
// subtractive modes leave alone (CB_BLEND 0x65000501 of the Prim_* draws: alpha ZERO and ONE_MINUS_SRC_ALPHA)
// scene: a draw on the HDR scene target (the forward pass's world layer, depth tested), not the effect buffer or the post chain
VkPipelineColorBlendAttachmentState BlendState(vfx::BlendMode mode, bool offscreen, bool scene) {
    VkPipelineColorBlendAttachmentState s{};
    s.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | (offscreen ? VK_COLOR_COMPONENT_A_BIT : 0u);
    s.blendEnable = VK_TRUE;
    s.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    s.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    s.alphaBlendOp = VK_BLEND_OP_ADD;
    s.colorBlendOp = VK_BLEND_OP_ADD;
    switch (mode) {
    case vfx::BlendMode::Alpha:
        s.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        s.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        if (offscreen) {
            s.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        } else if (scene) {
            // on the scene target only the alpha tested liquids blend this way (VfxPass::DrawLayer); they and the opaque draws
            // leave alpha 1 where they replace the pixel, which the handy light demodulation of a temporal upscaler
            // (upscale_demod.frag) leaves alone: the colour there is not the G-buffer surface's (the Freezer's view blood)
            s.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;
            s.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            s.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        }
        break;
    case vfx::BlendMode::Add:
        s.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        s.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
    case vfx::BlendMode::Sub:
        s.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        s.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        s.colorBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
        break;
    case vfx::BlendMode::Mul:
        s.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
        s.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        break;
    case vfx::BlendMode::Min:
        s.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        s.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        s.colorBlendOp = VK_BLEND_OP_MIN;
        break;
    case vfx::BlendMode::Opaque:
        s.blendEnable = VK_FALSE;
        if (scene) {
            s.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;
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
    device_ = renderer.Context().device;
    VkDescriptorSetLayoutBinding bindings[4]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[3] = {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_info.bindingCount = 4;
    layout_info.pBindings = bindings;
    if (!vk::Check(vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &set_layout_), "vfx set layout")) {
        return false;
    }
    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * Renderer::kFramesInFlight},
                                     {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6 * Renderer::kFramesInFlight},
                                     {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 * Renderer::kFramesInFlight}};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 3 * Renderer::kFramesInFlight;
    pool_info.poolSizeCount = 3;
    pool_info.pPoolSizes = sizes;
    if (!vk::Check(vkCreateDescriptorPool(device_, &pool_info, nullptr, &pool_), "vfx descriptor pool")) {
        return false;
    }
    VkDescriptorSetLayout set_layouts[2] = {textures.SetLayout(), set_layout_};
    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 2;
    pl.pSetLayouts = set_layouts;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &range;
    if (!vk::Check(vkCreatePipelineLayout(device_, &pl, nullptr, &layout_), "vfx pipeline layout")) {
        return false;
    }
    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = VK_FILTER_NEAREST;
    sampler_info.minFilter = VK_FILTER_NEAREST;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxLod = 0.0f;
    if (!vk::Check(vkCreateSampler(device_, &sampler_info, nullptr, &sampler_), "vfx sampler")) {
        return false;
    }
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    if (!vk::Check(vkCreateSampler(device_, &sampler_info, nullptr, &scene_sampler_), "vfx scene sampler")) {
        return false;
    }
    vertex_ = vk::LoadShaderModule(device_, "vfx_particle.vert");
    fragment_ = vk::LoadShaderModule(device_, "vfx_particle.frag");
    if (!vertex_ || !fragment_) {
        return false;
    }
    for (FrameSlot& slot : slots_) {
        VkDescriptorSetLayout layouts[3] = {set_layout_, set_layout_, set_layout_};
        VkDescriptorSet sets[3] = {};
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = pool_;
        alloc.descriptorSetCount = 3;
        alloc.pSetLayouts = layouts;
        if (!vk::Check(vkAllocateDescriptorSets(device_, &alloc, sets), "vfx descriptor sets")) {
            return false;
        }
        slot.forward = sets[0];
        slot.filter = sets[1];
        slot.effects = sets[2];
        if (!renderer.Context().CreateBuffer(slot.quads, sizeof(vfx::Quad) * kInitialQuads, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) {
            return false;
        }
        if (!renderer.Context().CreateBuffer(slot.fog, 3 * kFogStride, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true)) {
            return false;
        }
        std::memset(slot.fog.mapped, 0, 3 * kFogStride);
        vmaFlushAllocation(renderer.Context().allocator, slot.fog.allocation, 0, 3 * kFogStride);
    }
    return true;
}

void VfxPass::Shutdown() {
    if (!device_) {
        return;
    }
    for (auto& [key, pipeline] : pipelines_) {
        vkDestroyPipeline(device_, pipeline, nullptr);
    }
    pipelines_.clear();
    for (FrameSlot& slot : slots_) {
        renderer_->Context().DestroyBuffer(slot.quads);
        renderer_->Context().DestroyBuffer(slot.fog);
    }
    if (vertex_) {
        vkDestroyShaderModule(device_, vertex_, nullptr);
    }
    if (fragment_) {
        vkDestroyShaderModule(device_, fragment_, nullptr);
    }
    vkDestroySampler(device_, sampler_, nullptr);
    vkDestroySampler(device_, scene_sampler_, nullptr);
    vkDestroyPipelineLayout(device_, layout_, nullptr);
    vkDestroyDescriptorPool(device_, pool_, nullptr);
    vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
    device_ = VK_NULL_HANDLE;
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

// The liquid's reflection cube (TppLiquidMaterial2Node 0x23E90E5F, gr_cub01 by default) is bound as the effect loader leaves it,
// without its FTEX sRGB flag: liquid_trace_f010 op 578, the first Primitive_Liquid2Final draw of frame 1490, binds gr_cub01 as a
// 64x64 BC1 UNORM image whose first layer is the file's face 0 byte for byte, and the shader squares the texel
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
    // one finished ahead decode uploaded a frame (DecodeAhead), so the next stage's effect textures do not all upload in the
    // frame that first draws them
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

VkPipeline VfxPass::Pipeline(const PipelineKey& key) {
    for (const auto& [k, p] : pipelines_) {
        if (k == key) {
            return p;
        }
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertex_, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragment_, "main", nullptr};
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = key.cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = key.depth_test ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = VK_FALSE;
    depth.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    const VkPipelineColorBlendAttachmentState attachment = BlendState(static_cast<vfx::BlendMode>(key.blend), key.offscreen,
                                                                       key.depth_test && !key.offscreen);
    const VkPipelineColorBlendAttachmentState attachments[2] = {attachment, attachment};
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = key.offscreen ? 2u : 1u;
    blend.pAttachments = attachments;
    const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    const VkFormat color_formats[2] = {key.color, key.color};
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = key.offscreen ? 2u : 1u;
    rendering.pColorAttachmentFormats = color_formats;
    rendering.depthAttachmentFormat = key.depth;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = layout_;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (!vk::Check(vk::CreateGraphicsPipelinesCached(device_, 1, &info, nullptr, &pipeline), "vfx pipeline")) {
        pipeline = VK_NULL_HANDLE;
    }
    pipelines_.emplace_back(key, pipeline);
    return pipeline;
}

void VfxPass::WriteSet(FrameSlot& slot, VkDescriptorSet set, VkImageView view, VkImageLayout depth_layout, VkImageView scene,
                       VkDeviceSize fog_offset) {
    VkDescriptorBufferInfo buffer{slot.quads.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo image{sampler_, view, depth_layout};
    VkDescriptorImageInfo scene_image{scene_sampler_, scene, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo fog{slot.fog.buffer, fog_offset, sizeof(FogBlock)};
    VkWriteDescriptorSet writes[4]{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &buffer, nullptr};
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &image, nullptr, nullptr};
    writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &scene_image, nullptr,
                 nullptr};
    writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 3, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &fog, nullptr};
    vkUpdateDescriptorSets(device_, 4, writes, 0, nullptr);
}

bool VfxPass::Upload(uint32_t frame_index) {
    FrameSlot& slot = slots_[frame_index % Renderer::kFramesInFlight];
    if (slot.uploaded == generation_) {
        return true;
    }
    const VkDeviceSize bytes = sizeof(vfx::Quad) * list_.quads.size();
    if (bytes > slot.quads.size) {
        renderer_->Context().DestroyBuffer(slot.quads);
        const VkDeviceSize size = std::max<VkDeviceSize>(bytes + bytes / 2, sizeof(vfx::Quad) * kInitialQuads);
        if (!renderer_->Context().CreateBuffer(slot.quads, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) {
            return false;
        }
        slot.forward_view = VK_NULL_HANDLE;
        slot.filter_view = VK_NULL_HANDLE;
        slot.effects_depth_view = VK_NULL_HANDLE;
        slot.effects_scene = VK_NULL_HANDLE;
    }
    if (bytes) {
        std::memcpy(slot.quads.mapped, list_.quads.data(), bytes);
        vmaFlushAllocation(renderer_->Context().allocator, slot.quads.allocation, 0, bytes);
    }
    slot.uploaded = generation_;
    return true;
}

uint32_t VfxPass::DrawLayer(VkCommandBuffer cmd, VkDescriptorSet set, vfx::Layer layer, VkFormat color, VkFormat depth, bool depth_test,
                            uint32_t mode, const glm::mat4& view_projection, const glm::vec3& eye, float exposure, float near_plane, VkExtent2D extent,
                            const std::function<void()>* copy_scene, uint32_t subset, uint32_t* recorded) {
    uint32_t copies = 0;
    bool bound = false;
    bool copy_stale = true;
    // the original copies the scene buffer (compute 8c7e2adaa817965c into an 8-bit image) before liquid draws that follow other drawing,
    // and runs consecutive liquid draws on one copy (f010_dumps frame 1490: one copy, then 64 Primitive_Liquid2Final draws)
    const float refraction_scale = static_cast<float>(extent.height) / 1080.0f;
    for (uint32_t index : order_) {
        const vfx::Draw& d = list_.draws[index];
        if (d.layer != layer || d.count == 0) {
            continue;
        }
        // opaque liquids (Primitive_Liquid2Final with the alpha test, at full resolution), multiply, min and opaque draws stay on the
        // scene target. A liquid without the alpha test goes to the effect pass in its place among the other effects, as the
        // original's material setup sends it (formats/vfx.md 7: liquid_trace_f010 1490 draws the blood water at 960 x 540 between
        // the smoke and dust): it covers the effects drawn before it there by its alpha, so the f120 view distortion sprites
        // (fx_sh_viwdis01_s1, 02) take the CeilLamp's glow sprites with the image they move instead of leaving them, unmoved,
        // over the moved lamp. PT_VFX_LIQUID_SCENE=1 draws every liquid on the scene before the effect buffer, as before.
        static const bool liquid_scene = std::getenv("PT_VFX_LIQUID_SCENE") != nullptr;
        const bool effect_liquid = d.refract && !d.opaque && !liquid_scene;
        const bool buffered = (!d.refract || effect_liquid) &&
                              (d.blend == vfx::BlendMode::Alpha || d.blend == vfx::BlendMode::Add || d.blend == vfx::BlendMode::Sub);
        if ((subset == kVfxScene && buffered) || (subset == kVfxOffscreen && !buffered)) {
            continue;
        }
        const bool offscreen = subset == kVfxOffscreen;
        const VkPipeline pipeline = Pipeline({color, depth, static_cast<uint8_t>(d.blend), depth_test, d.cull, offscreen});
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
            VkDescriptorSet sets[2] = {textures_->Set(), set};
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 2, sets, 0, nullptr);
            VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
            VkRect2D scissor{{0, 0}, extent};
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            bound = true;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        Push push;
        push.view_projection = view_projection;
        // w: refraction pixels per 1080p pixel when the scene copy is valid for this draw, else 0 (scalar transmission)
        push.eye = glm::vec4(eye, refract ? refraction_scale : 0.0f);
        push.frame = glm::vec4(exposure, near_plane, 1.0f / static_cast<float>(std::max(1u, extent.width)),
                               1.0f / static_cast<float>(std::max(1u, extent.height)));
        static const uint32_t debug = std::getenv("PT_VFX_DEBUG") ? 1u : 0u;
        push.ids = glm::uvec4(d.first, offscreen ? 2u : mode, static_cast<uint32_t>(d.blend), debug);
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push), &push);
        vkCmdDraw(cmd, d.count * 6, 1, 0, 0);
        if (recorded) {
            ++*recorded;
        }
    }
    return copies;
}

void VfxPass::RecordForward(const SceneVfxContext& context) {
    if (context.mirrored || list_.draws.empty() || !device_) {
        return;
    }
    const uint32_t frame = context.frame_index % Renderer::kFramesInFlight;
    if (!Upload(frame)) {
        return;
    }
    FrameSlot& slot = slots_[frame];
    const bool offscreen = context.subset == kVfxOffscreen;
    if (offscreen) {
        if (slot.effects_depth_view != context.near_far_depth_view || slot.effects_scene != context.scene_copy_view) {
            WriteSet(slot, slot.effects, context.near_far_depth_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, context.scene_copy_view,
                     2 * kFogStride);
            slot.effects_depth_view = context.near_far_depth_view;
            slot.effects_scene = context.scene_copy_view;
        }
    } else if (slot.forward_view != context.depth_view || slot.forward_scene != context.scene_copy_view) {
        WriteSet(slot, slot.forward, context.depth_view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, context.scene_copy_view, 0);
        slot.forward_view = context.depth_view;
        slot.forward_scene = context.scene_copy_view;
    }
    FogBlock fog;
    std::copy(std::begin(context.fog), std::end(context.fog), std::begin(fog.fog));
    fog.mode = glm::uvec4(context.fog_mode, 0u, 0u, 0u);
    const VkDeviceSize fog_offset = offscreen ? 2 * kFogStride : 0;
    std::memcpy(static_cast<uint8_t*>(slot.fog.mapped) + fog_offset, &fog, sizeof(fog));
    vmaFlushAllocation(renderer_->Context().allocator, slot.fog.allocation, fog_offset, sizeof(fog));
    const float near_plane = context.projection[3][2];
    const uint32_t copies = DrawLayer(context.cmd, offscreen ? slot.effects : slot.forward, vfx::Layer::World, context.color_format,
                                      context.depth_format, true, 0, context.view_projection, context.eye, context.exposure, near_plane,
                                      context.extent, context.scene_copy_view && context.copy_scene ? &context.copy_scene : nullptr,
                                      context.subset, context.recorded);
    static const bool debug = std::getenv("PT_VFX_DEBUG") != nullptr;
    if (debug && (generation_ % 120) == 0) {
        LogDebug("vfx: exposure {:.5f} ev {:.2f} near {:.3f}, {} scene copies for liquids", context.exposure,
                 -std::log2(std::max(context.exposure, 1e-8f)), near_plane, copies);
    }
}

void VfxPass::RecordFilter(const SceneFilterContext& context) {
    if (list_.draws.empty() || !device_ || !context.depth_view) {
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
    if (slot.filter_view != context.depth_view || slot.filter_scene != context.source) {
        WriteSet(slot, slot.filter, context.depth_view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, context.source, kFogStride);
        slot.filter_view = context.depth_view;
        slot.filter_scene = context.source;
    }
    for (const vfx::Layer layer : {vfx::Layer::Flare, vfx::Layer::Screen}) {
        if (!wanted(layer)) {
            continue;
        }
        DrawLayer(context.cmd, slot.filter, layer, context.color_format, VK_FORMAT_UNDEFINED, false, 1, glm::mat4(1.0f), glm::vec3(0.0f),
                  context.exposure, list_.near_plane, context.extent, nullptr);
    }
}

}
