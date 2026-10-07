#include "engine/render/scene_renderer.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "engine/assets/ftex.h"
#include "engine/core/log.h"
#include "engine/fs/vfs.h"

namespace pt {
namespace {

constexpr VkFormat kAlbedoFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kNormalFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kMaterialFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kDepthFormat = SceneRenderer::kDepthTargetFormat;
constexpr VkFormat kLightFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kHdrFormat = SceneRenderer::kHdrTargetFormat;
constexpr VkFormat kLdrFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kAoFormat = VK_FORMAT_R8_UNORM;
constexpr VkFormat kRefMapFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kPostFormat = SceneRenderer::kPostTargetFormat;
constexpr VkFormat kBloomFormat = VK_FORMAT_R8G8B8A8_SRGB;
constexpr uint32_t kShadowAtlasSize = SceneRenderer::kShadowAtlasSize;

const char* const kDebugViewNames =
    "lit\0normals\0uv0\0vertex color\0albedo\0roughness\0specular\0material index\0depth\0translucency\0light diffuse\0light specular\0ambient "
    "(probes)\0reflection amount\0occlusion\0unlit albedo\0mirror capture\0";

glm::mat4 ReverseZPerspective(float fov_y, float aspect, float near_plane) {
    const float f = 1.0f / std::tan(fov_y * 0.5f);
    glm::mat4 p(0.0f);
    p[0][0] = f / aspect;
    p[1][1] = -f;
    p[2][3] = -1.0f;
    p[3][2] = near_plane;
    return p;
}

glm::mat4 FoxView(const glm::mat4& gl_view) {
    return glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, -1.0f)) * gl_view;
}

void ExtractPlanes(const glm::mat4& vp, glm::vec4 planes[6]) {
    const glm::mat4 m = glm::transpose(vp);
    planes[0] = m[3] + m[0];
    planes[1] = m[3] - m[0];
    planes[2] = m[3] + m[1];
    planes[3] = m[3] - m[1];
    planes[4] = m[2];
    planes[5] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    for (int i = 0; i < 5; ++i) {
        const float len = glm::length(glm::vec3(planes[i]));
        if (len > 0.0f) {
            planes[i] /= len;
        }
    }
}

glm::vec3 AnyPerpendicular(const glm::vec3& d) {
    return std::abs(d.y) < 0.95f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
}

uint8_t ToByte(float v) {
    return static_cast<uint8_t>(std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f));
}

float Hermite(float t, float s0, float s1) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    return 3.0f * t2 - 2.0f * t3 + s0 * (t3 - 2.0f * t2 + t) + s1 * (t3 - t2);
}

}

const char* DebugViewNames() {
    return kDebugViewNames;
}

bool SceneRenderer::EnsureShadowTarget() {
    const uint32_t tile = graphics.shadow_quality<=1?512u:graphics.shadow_quality==2?1024u:2048u;
    const uint32_t size=tile*4;
    if(shadow_.Valid() && shadow_.Extent().width==size) return true;
    RenderTarget replacement;
    if(!CreateTarget(replacement,kDepthFormat,{size,size},VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_DEPTH_BIT))
        return shadow_.Valid();
    auto& ctx=renderer_->Context();
    vkDeviceWaitIdle(device_);
    ctx.Submit([&](VkCommandBuffer cmd){
        UseTargets(cmd,{{&replacement,VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL}});
        BeginPass(cmd,{size,size},{},&replacement,false,true);vkCmdEndRendering(cmd);
        UseTargets(cmd,{{&replacement,VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}});
    });
    ctx.DestroyImage(shadow_.image);shadow_=replacement;
    if(extent_.width>0) WriteImageDescriptors();
    LogInfo("graphics: shadow atlas {}x{}, tile {} px",size,size,tile);
    return true;
}

bool SceneRenderer::Init(Renderer& renderer, TextureManager& textures) {
    renderer_ = &renderer;
    textures_ = &textures;
    vk::Context& ctx = renderer.Context();
    device_ = ctx.device;

    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.maxLod = VK_LOD_CLAMP_NONE;
    const VkFilter filters[4] = {VK_FILTER_NEAREST, VK_FILTER_LINEAR, VK_FILTER_NEAREST, VK_FILTER_LINEAR};
    const VkSamplerAddressMode clamp = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    const VkSamplerAddressMode repeat = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    const VkSamplerAddressMode modes[4] = {clamp, clamp, repeat, repeat};
    for (int i = 0; i < 4; ++i) {
        sampler.magFilter = sampler.minFilter = filters[i];
        sampler.mipmapMode = filters[i] == VK_FILTER_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = modes[i];
        vkCreateSampler(device_, &sampler, nullptr, &samplers_[i]);
    }
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.compareEnable = VK_TRUE;
    sampler.compareOp = VK_COMPARE_OP_LESS;
    vkCreateSampler(device_, &sampler, nullptr, &samplers_[gpu::kSmpShadow]);

    if (!CreateDescriptors()) {
        return false;
    }
    VkDescriptorSetLayout sets[2] = {textures.SetLayout(), frame_layout_};
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 2;
    layout_info.pSetLayouts = sets;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    if (!vk::Check(vkCreatePipelineLayout(device_, &layout_info, nullptr, &layout_), "scene pipeline layout")) {
        return false;
    }
    for (FrameSlot& slot : slots_) {
        ctx.CreateBuffer(slot.frame_data, sizeof(gpu::FrameData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        ctx.CreateBuffer(slot.skin, sizeof(glm::mat4) * gpu::kMaxSkinMatrices, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        ctx.CreateBuffer(slot.luminance, sizeof(glm::vec2) * gpu::kLuminanceEntries, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        query.queryType = VK_QUERY_TYPE_TIMESTAMP;
        query.queryCount = kTimestamps;
        vkCreateQueryPool(device_, &query, nullptr, &slot.queries);
        query.queryCount = 4;
        vkCreateQueryPool(device_, &query, nullptr, &slot.upscale_queries);
    }
    frame_ = new gpu::FrameData();
    CreateBuiltinResources();
    if (!EnsureShadowTarget()) return false;
    if (!CreatePipelines()) {
        return false;
    }
    sss_ready_ = sss_.Init(ctx, textures.SetLayout(), frame_layout_, kLightFormat, samplers_[gpu::kSmpPointClamp]);
    if (ctx.ray_query) {
        rt_ = std::make_unique<RayTracing>();
        PipelineDesc volume;
        volume.vertex = "volume.vert";
        volume.fragment = "light_rt.frag";
        volume.colors = {kLightFormat, kLightFormat};
        volume.depth = kDepthFormat;
        volume.depth_test = true;
        volume.depth_compare = VK_COMPARE_OP_LESS_OR_EQUAL;
        volume.blend = BlendMode::Additive;
        if (rt_->Init(ctx, textures.SetLayout(), frame_layout_)) {
            volume.layout = rt_->Layout();
            light_rt_ = CreateGraphicsPipeline(device_, volume);
            volume.fragment = "light_contact.frag";
            light_contact_ = CreateGraphicsPipeline(device_, volume);
            PipelineDesc reflect;
            reflect.layout = rt_->Layout();
            reflect.fragment = "reflect_make_rt.frag";
            reflect.colors = {kRefMapFormat, kRefMapFormat};
            reflect_make_rt_ = CreateGraphicsPipeline(device_, reflect);
            reflect.fragment = "reflect_blend_rt.frag";
            reflect.colors = {kHdrFormat};
            reflect_blend_rt_ = CreateGraphicsPipeline(device_, reflect);
            if (!reflect_make_rt_ || !reflect_blend_rt_) {
                LogError("ray tracing: cannot create the ray traced reflection passes, that option stays off");
            }
            reflect.fragment = "reflect_layer_rt.frag";
            reflect.colors = {kHdrFormat, kHdrFormat};
            reflect_layer_rt_ = CreateGraphicsPipeline(device_, reflect);
            rt_ao_trace_ = CreateComputePipeline(device_, rt_->Layout(), "rt_ao.comp");
            rt_ao_filter_ = CreateComputePipeline(device_, rt_->Layout(), "rt_ao_filter.comp");
            PipelineDesc probe;
            probe.layout = rt_->Layout();
            probe.vertex = "volume.vert";
            probe.fragment = "probe_ao.frag";
            probe.colors = {kLightFormat};
            probe.blend = BlendMode::ProbeAccumulate;
            probe_ao_ = CreateGraphicsPipeline(device_, probe);
            if (!rt_ao_trace_ || !rt_ao_filter_ || !probe_ao_) {
                LogError("ray tracing: AO pass creation failed, option disabled");
            }
        }
        if (!light_rt_) {
            LogError("ray tracing: RT shadow pass creation failed, option disabled");
            rt_->Shutdown();
            rt_.reset();
        }
    }
    SceneProbe ambient;
    ambient.world_to_box = glm::scale(glm::mat4(1.0f), glm::vec3(1.0e-4f));
    ambient.box_world = glm::scale(glm::mat4(1.0f), glm::vec3(1.0e4f));
    ambient.sh[0] = glm::vec3(0.35f);
    fallback_.probes.push_back(ambient);
    fallback_.screen.film_grain = false;
    fallback_.screen.screen_distortion = false;
    fallback_.screen.depth_of_field = false;
    return true;
}

bool SceneRenderer::CreateDescriptors() {
    VkDescriptorSetLayoutBinding bindings[6]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, gpu::kImageSlots, VK_SHADER_STAGE_ALL, nullptr};
    bindings[3] = {3, VK_DESCRIPTOR_TYPE_SAMPLER, gpu::kSmpCount, VK_SHADER_STAGE_ALL, nullptr};
    bindings[4] = {4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_ALL, nullptr};
    bindings[5] = {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr};
    const VkDescriptorBindingFlags binding_flags[6] = {0, 0, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, 0, 0, 0};
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flags_info.bindingCount = 6;
    flags_info.pBindingFlags = binding_flags;
    VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_info.pNext = &flags_info;
    layout_info.bindingCount = 6;
    layout_info.pBindings = bindings;
    if (!vk::Check(vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &frame_layout_), "frame set layout")) {
        return false;
    }
    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6 * Renderer::kFramesInFlight},
                                     {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * (gpu::kImageSlots + 1) * Renderer::kFramesInFlight},
                                     {VK_DESCRIPTOR_TYPE_SAMPLER, 2 * gpu::kSmpCount * Renderer::kFramesInFlight}};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 2 * Renderer::kFramesInFlight;
    pool_info.poolSizeCount = 3;
    pool_info.pPoolSizes = sizes;
    if (!vk::Check(vkCreateDescriptorPool(device_, &pool_info, nullptr, &pool_), "frame descriptor pool")) {
        return false;
    }
    for (FrameSlot& slot : slots_) {
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = pool_;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &frame_layout_;
        if (!vk::Check(vkAllocateDescriptorSets(device_, &alloc, &slot.set), "frame descriptor set") ||
            !vk::Check(vkAllocateDescriptorSets(device_, &alloc, &slot.post_set), "post descriptor set")) {
            return false;
        }
    }
    return true;
}

bool SceneRenderer::CreatePipelines() {
    PipelineDesc desc;
    desc.layout = layout_;
    desc.vertex = "mesh.vert";
    desc.fragment = "gbuffer.frag";
    desc.mesh_input = true;
    desc.colors = {kAlbedoFormat, kNormalFormat, kMaterialFormat};
    desc.depth = kDepthFormat;
    desc.depth_test = true;
    desc.depth_write = true;
    gbuffer_ = CreateGraphicsPipeline(device_, desc);
    desc.depth_write = false;
    desc.depth_bias = true;
    desc.blend = BlendMode::Alpha;
    constexpr uint32_t rgb = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    desc.write_masks = {rgb, rgb, VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT};
    decal_ = CreateGraphicsPipeline(device_, desc);

    PipelineDesc shadow;
    shadow.layout = layout_;
    shadow.vertex = "shadow.vert";
    shadow.fragment = "shadow.frag";
    shadow.mesh_input = true;
    shadow.depth = kDepthFormat;
    shadow.depth_test = true;
    shadow.depth_write = true;
    shadow.depth_bias = true;
    shadow_pipeline_ = CreateGraphicsPipeline(device_, shadow);

    PipelineDesc volume;
    volume.layout = layout_;
    volume.vertex = "volume.vert";
    volume.fragment = "probe.frag";
    volume.colors = {kLightFormat};
    volume.blend = BlendMode::ProbeAccumulate;
    probe_ = CreateGraphicsPipeline(device_, volume);
    {
        PipelineDesc resolve;
        resolve.layout = layout_;
        resolve.fragment = "probe_resolve.frag";
        resolve.colors = {kLightFormat, kLightFormat};
        resolve.depth = kDepthFormat;
        resolve.write_masks = {0xF, 0x0};
        probe_resolve_ = CreateGraphicsPipeline(device_, resolve);
    }
    volume.colors = {kLightFormat, kLightFormat};
    volume.depth = kDepthFormat;
    volume.depth_test = true;
    volume.depth_compare = VK_COMPARE_OP_LESS_OR_EQUAL;
    volume.write_masks = {0xF, 0x0};
    volume.fragment = "light.frag";
    volume.blend = BlendMode::Additive;
    volume.write_masks.clear();
    light_ = CreateGraphicsPipeline(device_, volume);

    PipelineDesc full;
    full.layout = layout_;
    full.fragment = "compose.frag";
    full.colors = {kHdrFormat};
    full.depth = kDepthFormat;
    compose_ = CreateGraphicsPipeline(device_, full);

    PipelineDesc forward;
    forward.layout = layout_;
    forward.vertex = "mesh.vert";
    forward.fragment = "forward.frag";
    forward.mesh_input = true;
    forward.colors = {kHdrFormat};
    forward.depth = kDepthFormat;
    forward.depth_test = true;
    forward.depth_write = true;
    forward.blend = BlendMode::PremultipliedFadeAlpha;
    forward_ = CreateGraphicsPipeline(device_, forward);
    forward_emissive_ = CreateGraphicsPipeline(device_, forward);

    luminance_ = CreateComputePipeline(device_, layout_, "luminance.comp");
    reflect_colour_ = CreateComputePipeline(device_, layout_, "reflect_colour.comp");

    PipelineDesc velocity;
    velocity.layout = layout_;
    velocity.vertex = "velocity.vert";
    velocity.fragment = "velocity.frag";
    velocity.mesh_input = true;
    velocity.colors = {kPostFormat};
    velocity.depth = kDepthFormat;
    velocity.depth_test = true;
    velocity_pipeline_ = CreateGraphicsPipeline(device_, velocity);

    PipelineDesc post;
    post.layout = layout_;
    post.fragment = "bright.frag";
    post.colors = {kBloomFormat};
    bright_ = CreateGraphicsPipeline(device_, post);
    post.fragment = "kawase.frag";
    kawase_ = CreateGraphicsPipeline(device_, post);
    post.blend = BlendMode::Additive;
    bloom_add_ = CreateGraphicsPipeline(device_, post);
    post.blend = BlendMode::None;
    post.colors = {kBloomFormat, kBloomFormat};
    post.blends = {BlendMode::None, BlendMode::Additive};
    post.fragment = "kawase_sum.frag";
    kawase_sum_ = CreateGraphicsPipeline(device_, post);
    post.blends.clear();
    post.colors = {kBloomFormat};
    post.fragment = "gaussian.frag";
    gaussian_ = CreateGraphicsPipeline(device_, post);
    post.colors = {kLdrFormat};
    post.fragment = "tonemap.frag";
    tonemap_ = CreateGraphicsPipeline(device_, post);
    post.fragment = "fxaa.frag";
    fxaa_ = CreateGraphicsPipeline(device_, post);
    post.colors = {kHdrFormat};
    post.fragment = "mirror_temporal.frag";
    mirror_temporal_pipeline_ = CreateGraphicsPipeline(device_, post);
    post.colors = {kLdrFormat};
    post.fragment = "dof_blend.frag";
    dof_blend_ = CreateGraphicsPipeline(device_, post);
    post.fragment = "dof_ratio.frag";
    post.write_mask = VK_COLOR_COMPONENT_A_BIT;
    dof_ratio_ = CreateGraphicsPipeline(device_, post);
    post.write_mask = 0xF;
    post.fragment = "mb_composite.frag";
    post.blend = BlendMode::Alpha;
    mb_composite_ = CreateGraphicsPipeline(device_, post);
    post.blend = BlendMode::None;
    post.colors = {kPostFormat};
    post.fragment = "dof_down.frag";
    dof_down_ = CreateGraphicsPipeline(device_, post);
    post.fragment = "dof_blur.frag";
    dof_blur_ = CreateGraphicsPipeline(device_, post);
    post.fragment = "mb_velocity.frag";
    mb_velocity_ = CreateGraphicsPipeline(device_, post);
    post.fragment = "mb_tile.frag";
    mb_tile_pipeline_ = CreateGraphicsPipeline(device_, post);
    post.fragment = "mb_bake.frag";
    mb_bake_pipeline_ = CreateGraphicsPipeline(device_, post);
    post.fragment = "mb_mcguire.frag";
    mb_mcguire_ = CreateGraphicsPipeline(device_, post);
    post.colors = {kLdrFormat};
    post.fragment = "fsblur.frag";
    fsblur_ = CreateGraphicsPipeline(device_, post);
    post.fragment = "banding.frag";
    banding_ = CreateGraphicsPipeline(device_, post);
    post.fragment = "screen_fx.frag";
    post.colors = {Renderer::kSceneColorFormat};
    screen_fx_ = CreateGraphicsPipeline(device_, post);
    post.fragment = "debug.frag";
    debug_ = CreateGraphicsPipeline(device_, post);
    post.fragment = "ssao.frag";
    post.colors = {kAoFormat};
    occlusion_ = CreateGraphicsPipeline(device_, post);
    post.fragment = "ssao_blur.frag";
    occlusion_blur_ = CreateGraphicsPipeline(device_, post);
    post.fragment = "reflect_make.frag";
    post.colors = {kRefMapFormat};
    reflect_make_ = CreateGraphicsPipeline(device_, post);
    post.fragment = "reflect_blend.frag";
    post.colors = {kHdrFormat};
    reflect_blend_ = CreateGraphicsPipeline(device_, post);
    post.colors = {kHdrFormat, kHdrFormat};
    post.fragment = "reflect_layer.frag";
    reflect_layer_pipeline_ = CreateGraphicsPipeline(device_, post);
    post.fragment = "reflect_temporal.frag";
    reflect_temporal_pipeline_ = CreateGraphicsPipeline(device_, post);
    post.colors = {kHdrFormat};
    post.fragment = "vfx_composite.frag";
    vfx_composite_ = CreateGraphicsPipeline(device_, post);

    const VkPipeline all[] = {gbuffer_, decal_, shadow_pipeline_, probe_, probe_resolve_, light_, compose_, forward_, forward_emissive_, luminance_, bright_,
                              reflect_colour_, kawase_, bloom_add_, kawase_sum_, gaussian_, tonemap_, fxaa_, dof_ratio_, dof_down_, dof_blur_, dof_blend_, mb_velocity_,
                              mb_tile_pipeline_, mb_bake_pipeline_, mb_mcguire_, mb_composite_, velocity_pipeline_, fsblur_, banding_, screen_fx_,
                              debug_, occlusion_, occlusion_blur_, reflect_make_, reflect_blend_, vfx_composite_, mirror_temporal_pipeline_,
                              reflect_layer_pipeline_, reflect_temporal_pipeline_};
    for (VkPipeline p : all) {
        if (!p) {
            LogError("scene renderer: pipeline creation failed");
            return false;
        }
    }
    return CreateUpscalePipelines();
}

void SceneRenderer::DestroyPipelines() {
    VkPipeline* all[] = {&gbuffer_, &decal_, &shadow_pipeline_, &probe_, &probe_resolve_, &light_, &compose_, &forward_, &forward_emissive_, &luminance_, &bright_,
                         &reflect_colour_, &kawase_, &bloom_add_, &kawase_sum_, &gaussian_, &tonemap_, &fxaa_, &dof_ratio_, &dof_down_, &dof_blur_, &dof_blend_,
                         &mb_velocity_, &mb_tile_pipeline_, &mb_bake_pipeline_, &mb_mcguire_, &mb_composite_, &velocity_pipeline_, &fsblur_,
                         &banding_, &screen_fx_, &debug_, &occlusion_, &occlusion_blur_, &reflect_make_, &reflect_blend_, &vfx_composite_,
                         &mirror_temporal_pipeline_, &reflect_layer_pipeline_, &reflect_temporal_pipeline_};
    for (VkPipeline* p : all) {
        if (*p) {
            vkDestroyPipeline(device_, *p, nullptr);
            *p = VK_NULL_HANDLE;
        }
    }
    DestroyUpscalePipelines();
}

void SceneRenderer::Shutdown() {
    if (!renderer_) {
        return;
    }
    vk::Context& ctx = renderer_->Context();
    vkDeviceWaitIdle(device_);
    sss_.Shutdown();
    if (last_timed_slot_ < Renderer::kFramesInFlight && slots_[last_timed_slot_].timed) {
        ReadTimestamps(slots_[last_timed_slot_]);
        LogInfo("scene renderer: last frame {}x{} gpu {:.2f} ms (shadows {:.2f}, mirror {:.2f}, gbuffer {:.2f}, lighting {:.2f}, compose {:.2f}, "
                "post {:.2f}), cpu {:.2f} ms",
                extent_.width, extent_.height, stats_.gpu_ms, stats_.pass_ms[0], stats_.pass_ms[1], stats_.pass_ms[2], stats_.pass_ms[3],
                stats_.pass_ms[4], stats_.pass_ms[5], stats_.cpu_ms);
    }
    if (gpu_samples_ > 0) {
        LogInfo("scene renderer: {}x{} -> {}x{}, {}: average gpu {:.3f} ms over {} frames, cpu {:.3f} ms", extent_.width, extent_.height,
                output_extent_.width, output_extent_.height, upscale_samples_ > 0 ? UpscalerName(upscale_stats_.kind) : "native",
                gpu_ms_sum_ / gpu_samples_, gpu_samples_, cpu_samples_ > 0 ? cpu_ms_sum_ / cpu_samples_ : 0.0);
    }
    if (timing_count_ > 0) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(timing_count_, timing_ring_.size()));
        double gpu = 0.0;
        double pass[6] = {};
        float lo = timing_ring_[0].gpu_ms;
        float hi = lo;
        for (size_t i = 0; i < n; ++i) {
            const FrameTiming& t = timing_ring_[i];
            gpu += t.gpu_ms;
            lo = std::min(lo, t.gpu_ms);
            hi = std::max(hi, t.gpu_ms);
            for (int k = 0; k < 6; ++k) {
                pass[k] += t.pass_ms[k];
            }
        }
        LogInfo("scene renderer: timing of the last {} frames at {}x{} -> {}x{}: gpu {:.3f} ms (min {:.3f}, max {:.3f}); shadows {:.3f}, mirror {:.3f}, "
                "gbuffer {:.3f}, lighting {:.3f}, compose {:.3f}, post {:.3f}",
                n, extent_.width, extent_.height, output_extent_.width, output_extent_.height, gpu / n, lo, hi, pass[0] / n, pass[1] / n,
                pass[2] / n, pass[3] / n, pass[4] / n, pass[5] / n);
        if (const char* csv = std::getenv("PT_TIMING_CSV")) {
            if (FILE* f = std::fopen(csv, "w")) {
                std::fprintf(f, "gpu,shadows,mirror,gbuffer,lighting,compose,post,forward,effects,upscale_inputs,reflections,upscaler,bloom,"
                                "device_mb,frame\n");
                for (size_t i = 0; i < n; ++i) {
                    const FrameTiming& t = timing_ring_[(timing_count_ - n + i) % timing_ring_.size()];
                    std::fprintf(f, "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.1f,%llu\n", t.gpu_ms, t.pass_ms[0],
                                 t.pass_ms[1], t.pass_ms[2], t.pass_ms[3], t.pass_ms[4], t.pass_ms[5], t.part_ms[0], t.part_ms[1], t.part_ms[2],
                                 t.part_ms[3], t.part_ms[4], t.part_ms[5], t.device_mb, static_cast<unsigned long long>(t.frame));
                }
                std::fclose(f);
            }
        }
    }
    if (const char* csv = std::getenv("PT_FRAME_CSV"); csv && !frame_trace_.empty()) {
        if (FILE* f = std::fopen(csv, "w")) {
            std::fprintf(f, "frame,cpu_ms,interval_ms,lights,shadow_views,draws\n");
            for (size_t i = 0; i < frame_trace_.size(); ++i) {
                const FrameTrace& t = frame_trace_[i];
                std::fprintf(f, "%zu,%.4f,%.4f,%u,%u,%u\n", i, t.cpu_ms, t.interval_ms, t.lights, t.shadow_views, t.draws);
            }
            std::fclose(f);
        }
    }
    if (upscale_samples_ > 0) {
        LogInfo("upscale: {} over {} frames: motion and reactive {:.3f} ms, upscaler and resolve {:.3f} ms", UpscalerName(upscale_stats_.kind),
                upscale_samples_, inputs_ms_sum_ / upscale_samples_, upscale_ms_sum_ / upscale_samples_);
    }
    UpscaleHost::Get().Shutdown();
    if (rt_) {
        const RtStats& rt = rt_->Stats();
        LogInfo("ray tracing: {} static BLAS ({} triangles, {:.1f} MB), last frame {} instances, {} skinned ({} vertices)", rt.static_blas,
                rt.static_triangles, static_cast<double>(rt.static_bytes) / (1024.0 * 1024.0), rt.instances, rt.skinned_instances,
                rt.skinned_vertices);
        rt_->Shutdown();
        rt_.reset();
    }
    for (VkPipeline* p : {&light_rt_, &light_contact_, &reflect_make_rt_, &reflect_blend_rt_, &reflect_layer_rt_, &rt_ao_trace_, &rt_ao_filter_, &probe_ao_}) {
        if (*p) {
            vkDestroyPipeline(device_, *p, nullptr);
            *p = VK_NULL_HANDLE;
        }
    }
    DestroyPipelines();
    DestroyTargets();
    ctx.DestroyBuffer(dump_compose_);
    ctx.DestroyBuffer(dump_forward_);
    ctx.DestroyBuffer(dump_scene_);
    ctx.DestroyBuffer(dump_mirror_);
    ctx.DestroyImage(shadow_.image);
    vk::Image* images[] = {&lut1_, &lut2_, &material_tex_, &dither_, &color_lut_, &color_lut_prev_, &noise_, &mask_, &white_};
    for (vk::Image* image : images) {
        ctx.DestroyImage(*image);
    }
    for (FrameSlot& slot : slots_) {
        ctx.DestroyBuffer(slot.frame_data);
        ctx.DestroyBuffer(slot.skin);
        ctx.DestroyBuffer(slot.luminance);
        vkDestroyQueryPool(device_, slot.queries, nullptr);
        vkDestroyQueryPool(device_, slot.upscale_queries, nullptr);
    }
    vkDestroyPipelineLayout(device_, layout_, nullptr);
    vkDestroyDescriptorPool(device_, pool_, nullptr);
    vkDestroyDescriptorSetLayout(device_, frame_layout_, nullptr);
    for (VkSampler s : samplers_) {
        vkDestroySampler(device_, s, nullptr);
    }
    delete frame_;
    frame_ = nullptr;
    renderer_ = nullptr;
}

bool SceneRenderer::CreateTarget(RenderTarget& target, VkFormat format, VkExtent2D extent, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    target = RenderTarget{};
    target.aspect = aspect;
    return renderer_->Context().CreateImage(target.image, format, {std::max(extent.width, 1u), std::max(extent.height, 1u), 1}, usage, 1, 1, aspect);
}

void SceneRenderer::DestroyTargets() {
    vk::Context& ctx = renderer_->Context();
    RenderTarget* targets[] = {&albedo_, &normal_, &material_, &depth_, &diffuse_, &specular_, &probe_acc_, &hdr_, &bloom_[0], &bloom_[1], &bloom_[2],
                               &flare_, &ldr_[0], &ldr_[1], &history_, &mirror_, &ao_[0], &ao_[1], &refmap_, &hdr_copy_, &particles_, &dof_half_,
                               &dof_quarter_[0], &dof_quarter_[1], &dof_eighth_[0], &dof_eighth_[1], &velocity_, &object_velocity_,
                               &mb_tile_[0], &mb_tile_[1], &mb_tile_[2], &mb_tile_[3], &mb_tile_[4], &mb_neighbour_, &mb_bake_,
                               &mb_blur_[0], &mb_blur_[1], &mirror_history_, &mirror_temporal_, &reflect_layer_, &reflect_offset_,
                               &reflect_history_[0], &reflect_history_[1]};
    for (RenderTarget* t : targets) {
        ctx.DestroyImage(t->image);
        *t = RenderTarget{};
    }
    ctx.DestroyImage(refmap_color_.image);
    refmap_color_ = RenderTarget{};
    sss_.DestroyTargets();
    for (RenderTarget& t : rt_ao_images_) {
        ctx.DestroyImage(t.image);
        t = RenderTarget{};
    }
    rt_ao_history_ = false;
    mirror_history_valid_ = false;
    mirror_history_size_ = 0;
    reflect_history_valid_ = false;
    DestroyUpscaleTargets();
    extent_ = {0, 0};
    output_extent_ = {0, 0};
}

bool SceneRenderer::EnsureTargets(VkExtent2D extent, VkExtent2D output, bool upscaling) {
    if (extent.width == 0 || extent.height == 0 || output.width == 0 || output.height == 0) {
        return false;
    }
    if (extent.width == extent_.width && extent.height == extent_.height && output.width == output_extent_.width &&
        output.height == output_extent_.height && upscaling == targets_upscaled_) {
        return true;
    }
    vkDeviceWaitIdle(device_);
    DestroyTargets();
    mirror_stale_.valid = false;
    const VkImageUsageFlags color = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const VkImageUsageFlags dumped = color | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const uint32_t bloom_width = std::max(1u, static_cast<uint32_t>(std::lround(270.0 * output.width / std::max(output.height, 1u))));
    const VkExtent2D quarter{bloom_width, 270u};
    const VkImageAspectFlags c = VK_IMAGE_ASPECT_COLOR_BIT;
    bool ok = CreateTarget(albedo_, kAlbedoFormat, extent, dumped, c) && CreateTarget(normal_, kNormalFormat, extent, dumped, c) &&
              CreateTarget(material_, kMaterialFormat, extent, dumped, c) &&
              CreateTarget(depth_, kDepthFormat, extent,
                           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           VK_IMAGE_ASPECT_DEPTH_BIT) &&
              CreateTarget(diffuse_, kLightFormat, extent, dumped, c) && CreateTarget(specular_, kLightFormat, extent, dumped, c) &&
              CreateTarget(probe_acc_, kLightFormat, extent, dumped, c) &&
              CreateTarget(hdr_, kHdrFormat, extent, color | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, c) &&
              CreateTarget(hdr_copy_, kHdrFormat, extent, color | VK_IMAGE_USAGE_TRANSFER_DST_BIT, c) &&
              CreateTarget(particles_, kHdrFormat, extent, color, c) &&
              CreateTarget(refmap_, kRefMapFormat, extent, color, c) &&
              CreateTarget(mirror_, kHdrFormat, extent, color | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, c) &&
              CreateTarget(bloom_[0], kBloomFormat, quarter, color, c) && CreateTarget(bloom_[1], kBloomFormat, quarter, color, c) &&
              CreateTarget(bloom_[2], kBloomFormat, quarter, color, c) && CreateTarget(flare_, kPostFormat, output, color, c) &&
              CreateTarget(ldr_[0], kLdrFormat, output, color | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, c) &&
              CreateTarget(ldr_[1], kLdrFormat, output, color | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, c) &&
              CreateTarget(history_, kLdrFormat, output, color | VK_IMAGE_USAGE_TRANSFER_DST_BIT, c) &&
              CreateTarget(ao_[0], kAoFormat, extent, color, c) && CreateTarget(ao_[1], kAoFormat, extent, color | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, c);
    const VkExtent2D shift1{std::max(output.width >> 1, 1u), std::max(output.height >> 1, 1u)};
    const VkExtent2D shift2{std::max(output.width >> 2, 1u), std::max(output.height >> 2, 1u)};
    const VkExtent2D shift3{std::max(output.width >> 3, 1u), std::max(output.height >> 3, 1u)};
    ok = ok && CreateTarget(dof_half_, kPostFormat, shift1, color, c) && CreateTarget(dof_quarter_[0], kPostFormat, shift2, color, c) &&
         CreateTarget(dof_quarter_[1], kPostFormat, shift2, color, c) && CreateTarget(dof_eighth_[0], kPostFormat, shift3, color, c) &&
         CreateTarget(dof_eighth_[1], kPostFormat, shift3, color, c) && CreateTarget(velocity_, kPostFormat, output, color, c) &&
         CreateTarget(object_velocity_, kPostFormat, extent, color, c) && CreateTarget(mb_blur_[0], kPostFormat, shift1, color, c) &&
         CreateTarget(mb_blur_[1], kPostFormat, shift1, color, c);
    for (uint32_t i = 0; i < 5 && ok; ++i) {
        const uint32_t round = (2u << i) - 1u;
        ok = CreateTarget(mb_tile_[i], kPostFormat, {(output.width + round) >> (i + 1), (output.height + round) >> (i + 1)}, color, c);
    }
    ok = ok && CreateTarget(mb_neighbour_, kPostFormat, mb_tile_[4].Extent(), color, c) &&
         CreateTarget(mb_bake_, kPostFormat, {(output.width + 1) >> 1, (output.height + 1) >> 1}, color, c);
    ok = ok && (!upscaling || CreateUpscaleTargets(extent, output));
    ok = ok && (!upscaling ||
                (CreateTarget(mirror_history_, kHdrFormat, extent, color | VK_IMAGE_USAGE_TRANSFER_DST_BIT, c) &&
                 CreateTarget(mirror_temporal_, kHdrFormat, extent, color | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, c) &&
                 CreateTarget(reflect_layer_, kHdrFormat, extent, color, c) && CreateTarget(reflect_offset_, kHdrFormat, extent, color, c) &&
                 CreateTarget(reflect_history_[0], kHdrFormat, extent, color, c) &&
                 CreateTarget(reflect_history_[1], kHdrFormat, extent, color, c)));
    ok = ok && (!rt_ || CreateTarget(refmap_color_, kRefMapFormat, extent, color, c));
    ok = ok && (!sss_ready_ || sss_.CreateTargets(extent));
    if (!ok) {
        LogError("scene renderer: cannot create render targets {}x{}", extent.width, extent.height);
        return false;
    }
    renderer_->Context().Submit([&](VkCommandBuffer cmd) {
        RenderTarget* colors[] = {&albedo_, &normal_, &material_, &diffuse_, &specular_, &probe_acc_, &hdr_, &mirror_, &bloom_[0], &bloom_[1], &bloom_[2],
                                  &flare_, &ldr_[0], &ldr_[1], &history_, &ao_[0], &ao_[1], &refmap_, &hdr_copy_, &particles_, &dof_half_,
                                  &dof_quarter_[0], &dof_quarter_[1], &dof_eighth_[0], &dof_eighth_[1], &velocity_, &object_velocity_, &mb_tile_[0],
                                  &mb_tile_[1], &mb_tile_[2], &mb_tile_[3], &mb_tile_[4], &mb_neighbour_, &mb_bake_, &mb_blur_[0],
                                  &mb_blur_[1]};
        for (RenderTarget* t : colors) {
            UseTargets(cmd, {{t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}});
            BeginPass(cmd, t->Extent(), {{t, true, {}}});
            vkCmdEndRendering(cmd);
            UseTargets(cmd, {{t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
        }
        UseTargets(cmd, {{&depth_, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL}});
        BeginPass(cmd, extent, {}, &depth_, false, true);
        vkCmdEndRendering(cmd);
        UseTargets(cmd, {{&depth_, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}});
        RenderTarget* upscale_colors[] = {&motion_, &reactive_, &opaque_, &post_hdr_, &post_depth_, &post_object_velocity_, &refmap_color_,
                                         &mirror_history_, &mirror_temporal_, &reflect_layer_, &reflect_offset_, &reflect_history_[0],
                                         &reflect_history_[1]};
        for (RenderTarget* t : upscale_colors) {
            if (t->Valid()) {
                UseTargets(cmd, {{t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}});
                BeginPass(cmd, t->Extent(), {{t, true, {}}});
                vkCmdEndRendering(cmd);
                UseTargets(cmd, {{t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
            }
        }
        RenderTarget* upscale_storage[] = {&upscaled_, &exposure_image_};
        for (RenderTarget* t : upscale_storage) {
            if (t->Valid()) {
                UseTargets(cmd, {{t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL}});
                const VkClearColorValue zero{};
                const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdClearColorImage(cmd, t->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
                UseTargets(cmd, {{t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
            }
        }
    });
    extent_ = extent;
    output_extent_ = output;
    targets_upscaled_ = upscaling;
    history_valid_ = false;
    WriteImageDescriptors();
    return true;
}

void SceneRenderer::WriteImageDescriptors() {
    VkDescriptorImageInfo images[gpu::kImageSlots];
    for (VkDescriptorImageInfo& info : images) {
        info = {VK_NULL_HANDLE, white_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    auto set_target = [&](uint32_t slot, const RenderTarget& t) {
        if (t.Valid()) {
            images[slot].imageView = t.image.view;
            images[slot].imageLayout =
                t.aspect == VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
    };
    auto set_image = [&](uint32_t slot, const vk::Image& image) {
        if (image.view) {
            images[slot].imageView = image.view;
        }
    };
    set_target(gpu::kImgAlbedo, albedo_);
    set_target(gpu::kImgNormal, normal_);
    set_target(gpu::kImgMaterial, material_);
    set_target(gpu::kImgDepth, depth_);
    set_target(gpu::kImgDiffuse, diffuse_);
    set_target(gpu::kImgSpecular, specular_);
    set_target(gpu::kImgProbeAcc, probe_acc_);
    set_target(gpu::kImgHdr, hdr_);
    set_target(gpu::kImgBloomA, bloom_[0]);
    set_target(gpu::kImgBloomB, bloom_[1]);
    set_target(gpu::kImgBloomSum, bloom_[2]);
    set_target(gpu::kImgLdrA, ldr_[0]);
    set_target(gpu::kImgLdrB, ldr_[1]);
    set_target(gpu::kImgHistory, history_);
    set_target(gpu::kImgDofHalf, dof_half_);
    set_target(gpu::kImgDofQuarterA, dof_quarter_[0]);
    set_target(gpu::kImgDofQuarterB, dof_quarter_[1]);
    set_target(gpu::kImgDofEighthA, dof_eighth_[0]);
    set_target(gpu::kImgDofEighthB, dof_eighth_[1]);
    set_target(gpu::kImgVelocity, velocity_);
    set_target(gpu::kImgObjectVelocity, object_velocity_);
    for (uint32_t i = 0; i < 5; ++i) {
        set_target(gpu::kImgMbTile + i, mb_tile_[i]);
    }
    set_target(gpu::kImgMbNeighbour, mb_neighbour_);
    set_target(gpu::kImgMbBake, mb_bake_);
    set_target(gpu::kImgMbBlurA, mb_blur_[0]);
    set_target(gpu::kImgMbBlurB, mb_blur_[1]);
    set_target(gpu::kImgMirror, mirror_);
    set_target(gpu::kImgMirrorHistory, mirror_history_);
    set_target(gpu::kImgMirrorTemporal, mirror_temporal_);
    set_target(gpu::kImgReflectLayer, reflect_layer_);
    set_target(gpu::kImgReflectOffset, reflect_offset_);
    set_target(gpu::kImgReflectHistoryA, reflect_history_[0]);
    set_target(gpu::kImgReflectHistoryB, reflect_history_[1]);
    set_target(gpu::kImgShadow, shadow_);
    set_target(gpu::kImgAo, ao_[0]);
    set_target(gpu::kImgAoBlur, ao_[1]);
    set_target(gpu::kImgRefMap, refmap_);
    if (rt_ && refmap_color_.Valid()) {
        rt_->SetReflectionImage(refmap_color_.image.view, samplers_[gpu::kSmpLinearWrap]);
    }
    set_target(gpu::kImgHdrCopy, hdr_copy_);
    set_target(gpu::kImgParticles, particles_);
    set_target(gpu::kImgFlare, flare_);
    set_image(gpu::kResLut1, lut1_);
    set_image(gpu::kResMaterial, material_tex_);
    set_image(gpu::kResDither, dither_);
    set_image(gpu::kResColorLut, color_lut_);
    set_image(gpu::kResColorLutPrev, color_lut_prev_);
    set_image(gpu::kResNoise, noise_);
    renderer_->SetGrainNoise(noise_.view);
    set_image(gpu::kResMask, mask_);
    set_image(gpu::kResWhite, white_);
    VkDescriptorImageInfo sampler_infos[gpu::kSmpCount];
    for (uint32_t i = 0; i < gpu::kSmpCount; ++i) {
        sampler_infos[i] = {samplers_[i], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    }
    VkDescriptorImageInfo lut2_info{VK_NULL_HANDLE, lut2_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    WriteUpscaleDescriptors(images, false);
    VkDescriptorImageInfo post_images[gpu::kImageSlots];
    std::copy(std::begin(images), std::end(images), std::begin(post_images));
    WriteUpscaleDescriptors(post_images, true);
    for (int set_index = 0; set_index < 2 * static_cast<int>(Renderer::kFramesInFlight); ++set_index) {
        FrameSlot& slot = slots_[set_index / 2];
        const bool post = (set_index & 1) != 0;
        VkDescriptorBufferInfo frame_info{slot.frame_data.buffer, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo skin_info{slot.skin.buffer, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo luminance_info{slot.luminance.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet writes[6]{};
        for (VkWriteDescriptorSet& w : writes) {
            w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = post ? slot.post_set : slot.set;
            w.descriptorCount = 1;
        }
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].pBufferInfo = &frame_info;
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &skin_info;
        writes[2].dstBinding = 2;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[2].descriptorCount = gpu::kImageSlots;
        writes[2].pImageInfo = post ? post_images : images;
        writes[3].dstBinding = 3;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[3].descriptorCount = gpu::kSmpCount;
        writes[3].pImageInfo = sampler_infos;
        writes[4].dstBinding = 4;
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[4].pImageInfo = &lut2_info;
        writes[5].dstBinding = 5;
        writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[5].pBufferInfo = &luminance_info;
        vkUpdateDescriptorSets(device_, 6, writes, 0, nullptr);
    }
}

bool SceneRenderer::UploadImage(vk::Image& image, VkFormat format, VkExtent3D extent, const void* data, size_t size) {
    vk::Context& ctx = renderer_->Context();
    if (!image.image || image.format != format || image.extent.width != extent.width || image.extent.height != extent.height ||
        image.extent.depth != extent.depth) {
        if (image.image) {
            vkDeviceWaitIdle(device_);
            ctx.DestroyImage(image);
        }
        if (!ctx.CreateImage(image, format, extent, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            return false;
        }
    }
    vk::Buffer staging;
    if (!ctx.CreateBuffer(staging, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) {
        return false;
    }
    std::memcpy(staging.mapped, data, size);
    vmaFlushAllocation(ctx.allocator, staging.allocation, 0, size);
    ctx.Submit([&](VkCommandBuffer cmd) {
        vk::ImageBarrier(cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = extent;
        vkCmdCopyBufferToImage(cmd, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        vk::ImageBarrier(cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    ctx.DestroyBuffer(staging);
    return true;
}

bool SceneRenderer::UploadImageMips(vk::Image& image, VkFormat format, VkExtent3D extent, const std::vector<std::vector<uint8_t>>& mips) {
    vk::Context& ctx = renderer_->Context();
    const uint32_t levels = static_cast<uint32_t>(mips.size());
    if (image.image) {
        vkDeviceWaitIdle(device_);
        ctx.DestroyImage(image);
    }
    if (levels == 0 || !ctx.CreateImage(image, format, extent, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, levels)) {
        return false;
    }
    size_t total = 0;
    for (const std::vector<uint8_t>& mip : mips) {
        total += mip.size();
    }
    vk::Buffer staging;
    if (!ctx.CreateBuffer(staging, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) {
        return false;
    }
    std::vector<VkBufferImageCopy> regions;
    size_t offset = 0;
    for (uint32_t level = 0; level < levels; ++level) {
        std::memcpy(static_cast<uint8_t*>(staging.mapped) + offset, mips[level].data(), mips[level].size());
        VkBufferImageCopy region{};
        region.bufferOffset = offset;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        region.imageExtent = {std::max(1u, extent.width >> level), std::max(1u, extent.height >> level), 1};
        regions.push_back(region);
        offset += mips[level].size();
    }
    vmaFlushAllocation(ctx.allocator, staging.allocation, 0, total);
    ctx.Submit([&](VkCommandBuffer cmd) {
        vk::ImageBarrier(cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels, 1);
        vkCmdCopyBufferToImage(cmd, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(regions.size()),
                               regions.data());
        vk::ImageBarrier(cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, levels, 1);
    });
    ctx.DestroyBuffer(staging);
    return true;
}

void SceneRenderer::CreateBuiltinResources() {
    const uint8_t white[4] = {255, 255, 255, 255};
    UploadImage(white_, VK_FORMAT_R8G8B8A8_UNORM, {1, 1, 1}, white, 4);
    UploadImage(noise_, VK_FORMAT_R8G8B8A8_UNORM, {1, 1, 1}, white, 4);
    UploadImage(mask_, VK_FORMAT_R8G8B8A8_UNORM, {1, 1, 1}, white, 4);

    std::vector<uint8_t> lut1(64 * 64);
    for (int j = 0; j < 64; ++j) {
        for (int i = 0; i < 64; ++i) {
            const double c = i / 63.0;
            const double f0 = j / 63.0;
            lut1[j * 64 + i] = static_cast<uint8_t>(std::floor(255.0 * (f0 + (1.0 - f0) * std::pow(1.0 - c, 5.0))));
        }
    }
    UploadImage(lut1_, VK_FORMAT_R8_UNORM, {64, 64, 1}, lut1.data(), lut1.size());

    std::vector<uint8_t> lut2(64 * 16 * 16 * 2);
    for (int k = 0; k < 16; ++k) {
        for (int j = 0; j < 16; ++j) {
            for (int i = 0; i < 64; ++i) {
                const double c = std::sqrt(i / 63.0);
                const double r = std::sqrt(j / 15.0);
                const double m = std::sqrt(k / 15.0);
                const double n = std::exp2(12.0 - 11.0 * std::max(r, m));
                const double ny = std::exp2(12.0 - 11.0 * r);
                const double s = std::sqrt(std::pow(c, n) * (std::sqrt(n * ny) + 2.04) / 8.0);
                const size_t at = ((static_cast<size_t>(k) * 16 + j) * 64 + i) * 2;
                lut2[at] = static_cast<uint8_t>(std::lround(255.0 * std::min(s, 1.0)));
                lut2[at + 1] = static_cast<uint8_t>(std::max<long>(1, std::lround(255.0 / std::max(s, 1.0))));
            }
        }
    }
    UploadImage(lut2_, VK_FORMAT_R8G8_UNORM, {64, 16, 16}, lut2.data(), lut2.size());

    static const int kBayer[8][8] = {{0, 32, 8, 40, 2, 34, 10, 42},  {48, 16, 56, 24, 50, 18, 58, 26}, {12, 44, 4, 36, 14, 46, 6, 38},
                                     {60, 28, 52, 20, 62, 30, 54, 22}, {3, 35, 11, 43, 1, 33, 9, 41},   {51, 19, 59, 27, 49, 17, 57, 25},
                                     {15, 47, 7, 39, 13, 45, 5, 37},   {63, 31, 55, 23, 61, 29, 53, 21}};
    std::vector<uint8_t> dither(8 * 8 * 4);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const int b = kBayer[y][x];
            uint8_t* p = &dither[(y * 8 + x) * 4];
            p[0] = static_cast<uint8_t>(4 * b + 2);
            p[1] = static_cast<uint8_t>(2 * b + 1);
            p[2] = 64;
            p[3] = 64;
        }
    }
    UploadImage(dither_, VK_FORMAT_R8G8B8A8_UNORM, {8, 8, 1}, dither.data(), dither.size());

    std::vector<uint8_t> material(256 * 2 * 4, 0);
    for (int i = 0; i < 256; ++i) {
        uint8_t* row0 = &material[i * 4];
        uint8_t* row1 = &material[(256 + i) * 4];
        row0[0] = row0[1] = row0[2] = 255;
        row1[0] = 10;
    }
    UploadImage(material_tex_, VK_FORMAT_R8G8B8A8_UNORM, {256, 2, 1}, material.data(), material.size());

    ScreenSettings identity;
    std::vector<uint8_t> lut;
    BuildColorLut(std::string(), identity, lut);
    UploadImage(color_lut_, VK_FORMAT_R8G8B8A8_UNORM, {256, 16, 1}, lut.data(), lut.size());
    UploadImage(color_lut_prev_, VK_FORMAT_R8G8B8A8_UNORM, {256, 16, 1}, lut.data(), lut.size());
}

bool SceneRenderer::LoadResources(Vfs& vfs) {
    vfs_ = &vfs;
    auto load = [&](const char* path, vk::Image& target, bool rgba_only) {
        FtexTexture ftex;
        if (!LoadFtex(vfs.Textures(), path, ftex) || ftex.mips.empty() || ftex.mips[0].empty()) {
            LogWarn("scene renderer: resource {} missing", path);
            return false;
        }
        const VkFormat format = ftex.Format();
        if (rgba_only && format != VK_FORMAT_B8G8R8A8_UNORM && format != VK_FORMAT_B8G8R8A8_SRGB) {
            LogWarn("scene renderer: resource {} unexpected format", path);
            return false;
        }
        const bool ok = UploadImage(target, format, {ftex.width, ftex.height, 1}, ftex.mips[0].data(), ftex.mips[0].size());
        LogInfo("scene renderer: resource {} {}x{} format {} flags {:#x}", path, ftex.width, ftex.height, static_cast<int>(format), ftex.flags);
        return ok;
    };
    load("/Assets/fox/effect/gr_pic/materials_alp_rgba32_nomip_nrt", material_tex_, true);
    {
        FtexTexture ftex;
        const char* path = "/Assets/sh/effect/vfx_pic/view/fx_viwfilnis01_iy";
        if (LoadFtex(vfs.Textures(), path, ftex) && !ftex.mips.empty() && !ftex.mips[0].empty()) {
            std::vector<std::vector<uint8_t>> mips;
            for (uint32_t level = 0; level < ftex.mip_count && !ftex.mips[level].empty(); ++level) {
                mips.push_back(ftex.mips[level]);
            }
            UploadImageMips(noise_, ftex.Format(), {ftex.width, ftex.height, 1}, mips);
            LogInfo("scene renderer: resource {} {}x{} format {} with {} levels", path, ftex.width, ftex.height, static_cast<int>(ftex.Format()),
                    mips.size());
        } else {
            LogWarn("scene renderer: resource {} missing", path);
        }
    }
    load("/Assets/sh/effect/vfx_pic/fx_flr23_iy_alp_clp", mask_, false);
    color_lut_key_.clear();
    if (extent_.width > 0) {
        vkDeviceWaitIdle(device_);
        WriteImageDescriptors();
    }
    return true;
}

bool SceneRenderer::BuildColorLut(const std::string& path, const ScreenSettings& screen, std::vector<uint8_t>& out) {
    std::vector<glm::vec4> filter;
    if (!path.empty() && vfs_) {
        FtexTexture ftex;
        if (LoadFtex(vfs_->Textures(), path, ftex) && ftex.width == 16 && ftex.height == 16 && ftex.depth == 16 && !ftex.mips.empty() &&
            ftex.mips[0].size() >= 16 * 16 * 16 * 4) {
            filter.resize(16 * 16 * 16);
            const uint8_t* data = ftex.mips[0].data();
            for (size_t i = 0; i < filter.size(); ++i) {
                glm::vec4 v(data[i * 4 + 2], data[i * 4 + 1], data[i * 4 + 0], data[i * 4 + 3]);
                v /= 255.0f;
                if (ftex.Srgb()) {
                    for (int c = 0; c < 3; ++c) {
                        v[c] = v[c] <= 0.04045f ? v[c] / 12.92f : std::pow((v[c] + 0.055f) / 1.055f, 2.4f);
                    }
                }
                filter[i] = v;
            }
        } else {
            LogWarn("scene renderer: color LUT {} unavailable, using identity", path);
        }
    }
    auto fetch = [&](int x, int y, int z) {
        if (filter.empty()) {
            return glm::vec4(x / 15.0f, y / 15.0f, z / 15.0f, 1.0f);
        }
        return filter[(static_cast<size_t>(z) * 16 + y) * 16 + x];
    };
    auto sample = [&](glm::vec3 coord) {
        coord = glm::clamp(coord, glm::vec3(0.0f), glm::vec3(15.0f));
        const glm::ivec3 i0 = glm::min(glm::ivec3(glm::floor(coord)), glm::ivec3(14));
        const glm::vec3 f = coord - glm::vec3(i0);
        glm::vec4 result(0.0f);
        for (int dz = 0; dz < 2; ++dz) {
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const float w = (dx ? f.x : 1.0f - f.x) * (dy ? f.y : 1.0f - f.y) * (dz ? f.z : 1.0f - f.z);
                    result += w * fetch(i0.x + dx, i0.y + dy, i0.z + dz);
                }
            }
        }
        return result;
    };
    out.assign(256 * 16 * 4, 0);
    for (int g = 0; g < 16; ++g) {
        for (int b = 0; b < 16; ++b) {
            for (int r = 0; r < 16; ++r) {
                const glm::vec4 v = sample(glm::vec3(r, g, b) * screen.color_scale);
                uint8_t* p = &out[(static_cast<size_t>(g) * 256 + b * 16 + r) * 4];
                for (int c = 0; c < 4; ++c) {
                    p[c] = ToByte(Hermite(std::clamp(v[c], 0.0f, 1.0f), screen.start_slope, screen.end_slope));
                }
            }
        }
    }
    return !filter.empty();
}

void SceneRenderer::UpdateColorLut(const ScreenSettings& screen) {
    const std::string key = std::format("{}|{:.4f}|{:.4f}|{:.4f}|{:.4f}|{:.4f}", screen.lut_path, screen.color_scale.x, screen.color_scale.y,
                                        screen.color_scale.z, screen.start_slope, screen.end_slope);
    if (key == color_lut_key_) {
        return;
    }
    std::vector<uint8_t> lut;
    BuildColorLut(screen.lut_path, screen, lut);
    vkDeviceWaitIdle(device_);
    std::swap(color_lut_, color_lut_prev_);
    UploadImage(color_lut_, VK_FORMAT_R8G8B8A8_UNORM, {256, 16, 1}, lut.data(), lut.size());
    if (color_lut_key_.empty()) {
        UploadImage(color_lut_prev_, VK_FORMAT_R8G8B8A8_UNORM, {256, 16, 1}, lut.data(), lut.size());
    }
    color_lut_prev_key_ = color_lut_key_;
    color_lut_key_ = key;
    if (extent_.width > 0) {
        WriteImageDescriptors();
    }
}

std::unique_ptr<GpuMesh> SceneRenderer::Upload(const MeshData& data) {
    auto& ctx = renderer_->Context();
    auto mesh = std::make_unique<GpuMesh>();
    mesh->name = data.name;
    mesh->submeshes = data.submeshes;
    mesh->groups = data.groups;
    mesh->bounds_min = data.bounds_min;
    mesh->bounds_max = data.bounds_max;
    for (const SubMesh& sub : data.submeshes) {
        mesh->skinned = mesh->skinned || sub.skinned;
    }
    const VkDeviceSize vertex_bytes = data.vertices.size() * sizeof(Vertex);
    const VkDeviceSize index_bytes = data.indices.size() * sizeof(uint32_t);
    if (vertex_bytes == 0 || index_bytes == 0) {
        return nullptr;
    }
    const VkBufferUsageFlags rt_usage =
        ctx.ray_query ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR : 0;
    ctx.CreateBuffer(mesh->vertices, vertex_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | rt_usage, false);
    ctx.CreateBuffer(mesh->indices, index_bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | rt_usage, false);
    ctx.Upload(mesh->vertices, data.vertices.data(), vertex_bytes);
    ctx.Upload(mesh->indices, data.indices.data(), index_bytes);
    return mesh;
}

void SceneRenderer::Destroy(GpuMesh& mesh) {
    auto& ctx = renderer_->Context();
    if (rt_) {
        rt_->Forget(mesh);
    }
    ctx.DestroyBuffer(mesh.vertices);
    ctx.DestroyBuffer(mesh.indices);
}

bool SceneRenderer::RayTracingSupported(std::string& reason) const {
    const vk::Context& ctx = renderer_->Context();
    if (!ctx.ray_query_supported) {
        reason = ctx.ray_query_missing.empty() ? std::string("no Vulkan ray queries") : ctx.ray_query_missing + " missing";
        return false;
    }
    reason.clear();
    return true;
}

float SceneRenderer::ExposureFor(float ev, const ExposureSettings& s) const {
    struct Point {
        float ev;
        float comp;
    };
    Point points[3] = {{s.add_comp_ev[0], s.add_comp[0]}, {s.add_comp_ev[1], s.add_comp[1]}, {s.add_comp_ev[2], s.add_comp[2]}};
    std::sort(std::begin(points), std::end(points), [](const Point& a, const Point& b) { return a.ev < b.ev; });
    float comp = points[0].comp;
    if (ev >= points[2].ev) {
        comp = points[2].comp;
    } else if (ev >= points[1].ev) {
        const float t = (ev - points[1].ev) / std::max(points[2].ev - points[1].ev, 1.0e-6f);
        comp = points[1].comp + t * (points[2].comp - points[1].comp);
    } else if (ev >= points[0].ev) {
        const float t = (ev - points[0].ev) / std::max(points[1].ev - points[0].ev, 1.0e-6f);
        comp = points[0].comp + t * (points[1].comp - points[0].comp);
    }
    return std::exp2(ev + s.compensation + comp);
}

void SceneRenderer::ReadTimestamps(FrameSlot& slot) {
    uint64_t stamps[kTimestamps] = {};
    const VkResult result =
        vkGetQueryPoolResults(device_, slot.queries, 0, kTimestamps, sizeof(stamps), stamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if (result != VK_SUCCESS) {
        return;
    }
    const double period = renderer_->Context().properties.limits.timestampPeriod * 1.0e-6;
    const auto span = [&](uint32_t from, uint32_t to) {
        return stamps[to] >= stamps[from] ? static_cast<float>(static_cast<double>(stamps[to] - stamps[from]) * period) : 0.0f;
    };
    for (uint32_t i = 1; i <= 6; ++i) {
        stats_.pass_ms[i - 1] = span(i - 1, i);
    }
    stats_.part_ms[0] = span(4, 7);
    stats_.part_ms[1] = span(7, 5);
    stats_.part_ms[2] = span(5, 8);
    stats_.part_ms[3] = span(8, 9);
    stats_.part_ms[4] = span(9, 10);
    stats_.part_ms[5] = span(10, 11);
    stats_.gpu_ms = span(0, 6);
}

void SceneRenderer::ReadMeasurements(FrameSlot& slot, float dt, const ExposureSettings& settings) {
    if (slot.timed) {
        ReadTimestamps(slot);
    }
    if (slot.reflection_sampled) {
        vmaInvalidateAllocation(renderer_->Context().allocator, slot.luminance.allocation, 0, VK_WHOLE_SIZE);
        const glm::vec2* entries = static_cast<const glm::vec2*>(slot.luminance.mapped);
        const glm::vec2 rg = entries[gpu::kReflectionReadback];
        const glm::vec2 b = entries[gpu::kReflectionReadback + 1];
        if (b.y > 0.5f) {
            stats_.reflection_readback = glm::vec3(rg, b.x);
            ++stats_.reflection_readbacks;
        }
        slot.reflection_sampled = false;
    }
    if (!slot.measured || slot.luminance_groups == 0) {
        return;
    }
    vmaInvalidateAllocation(renderer_->Context().allocator, slot.luminance.allocation, 0, VK_WHOLE_SIZE);
    const glm::vec2* partial = static_cast<const glm::vec2*>(slot.luminance.mapped);
    double sum = 0.0;
    double count = 0.0;
    for (uint32_t i = 0; i < slot.luminance_groups; ++i) {
        sum += partial[i].x;
        count += partial[i].y;
    }
    slot.measured = false;
    if (count <= 0.0) {
        return;
    }
    const float measured = static_cast<float>(sum / count);
    stats_.luminance = measured;
    if (!adaptation_valid_ || !toggles.adaptation || !lighting_ || !lighting_->valid) {
        return;
    }
    ev_ = std::clamp(ev_, settings.min_ev, settings.max_ev);
    const float raw = measured / std::max(slot.exposure_used, 1.0e-12f);
    const float adapted = raw * std::exp2(ev_);
    const float target = std::clamp(ev_ + 0.5f * (std::log2(settings.key + 0.001f) - std::log2(adapted + 0.001f)), settings.min_ev, settings.max_ev);
    const float diff = target - ev_;
    float step = diff * (1.0f - std::pow(0.98f, 60.0f * dt)) * settings.speed * std::min(1.0f, std::abs(diff));
    if (std::abs(step) > std::abs(diff)) {
        step = diff;
    }
    ev_ += step;
}

namespace {

uint32_t DumpTexelBytes(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8_UNORM:
        return 1;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        return 16;
    default:
        return 4;
    }
}

}

void SceneRenderer::RecordDumpCopy(VkCommandBuffer cmd, RenderTarget& target, vk::Buffer& buffer) {
    vk::Context& ctx = renderer_->Context();
    const VkExtent3D e = target.image.extent;
    const VkDeviceSize size = VkDeviceSize(e.width) * e.height * DumpTexelBytes(target.image.format);
    if (buffer.size < size) {
        ctx.DestroyBuffer(buffer);
        if (!ctx.CreateBuffer(buffer, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) {
            return;
        }
    }
    const VkImageLayout back = target.layout;
    UseTargets(cmd, {{&target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL}});
    VkBufferImageCopy region{};
    region.imageSubresource = {target.aspect, 0, 0, 1};
    region.imageExtent = {e.width, e.height, 1};
    vkCmdCopyImageToBuffer(cmd, target.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.buffer, 1, &region);
    UseTargets(cmd, {{&target, back}});
}

bool SceneRenderer::DumpTargets(const std::string& prefix) {
    dump_requested_ = false;
    vk::Context& ctx = renderer_->Context();
    vkDeviceWaitIdle(device_);
    std::ofstream index(prefix + ".targets.txt");
    auto write = [&](const char* name, const void* data, VkExtent3D e, VkFormat format) {
        const size_t bytes = size_t(e.width) * e.height * DumpTexelBytes(format);
        std::ofstream out(prefix + "." + name + ".bin", std::ios::binary);
        out.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
        index << name << " " << e.width << " " << e.height << " " << static_cast<int>(format) << "\n";
    };
    RenderTarget* targets[] = {&albedo_, &normal_, &material_, &depth_, &diffuse_, &specular_, &reactive_, &motion_, &ao_[1], &particles_,
                               &refmap_, &reflect_layer_, &hdr_};
    const char* names[] = {"albedo", "normal", "material", "depth", "diffuse", "specular", "reactive", "motion", "occlusion", "particles",
                           "refmap", "reflection", "hdr"};
    for (size_t i = 0; i < std::size(targets); ++i) {
        RenderTarget& target = *targets[i];
        if (!target.Valid()) {
            continue;
        }
        vk::Buffer buffer;
        const VkExtent3D e = target.image.extent;
        if (!ctx.CreateBuffer(buffer, VkDeviceSize(e.width) * e.height * DumpTexelBytes(target.image.format), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              true)) {
            continue;
        }
        ctx.Submit([&](VkCommandBuffer cmd) { RecordDumpCopy(cmd, target, buffer); });
        vmaInvalidateAllocation(ctx.allocator, buffer.allocation, 0, VK_WHOLE_SIZE);
        write(names[i], buffer.mapped, e, target.image.format);
        ctx.DestroyBuffer(buffer);
    }
    if (dump_recorded_ && hdr_.Valid()) {
        const std::pair<const char*, vk::Buffer*> copies[] = {{"compose", &dump_compose_}, {"forward", &dump_forward_}, {"scene", &dump_scene_}};
        for (const auto& [name, buffer] : copies) {
            if (buffer->mapped) {
                vmaInvalidateAllocation(ctx.allocator, buffer->allocation, 0, VK_WHOLE_SIZE);
                write(name, buffer->mapped, hdr_.image.extent, hdr_.image.format);
            }
        }
    }
    if (dump_mirror_.mapped && mirror_.Valid()) {
        vmaInvalidateAllocation(ctx.allocator, dump_mirror_.allocation, 0, VK_WHOLE_SIZE);
        write("mirror", dump_mirror_.mapped, mirror_.image.extent, mirror_.image.format);
        const VkExtent3D e = mirror_.image.extent;
        const uint32_t size = mirror_view_.area.width;
        const float* src = static_cast<const float*>(dump_mirror_.mapped);
        std::vector<uint8_t> image(size_t(size) * size * 4);
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                const size_t at = (size_t(y) * e.width + x) * 4;
                for (uint32_t c = 0; c < 4; ++c) {
                    const float v = std::clamp(src[at + c], 0.0f, 1.0f);
                    image[(size_t(y) * size + x) * 4 + c] = static_cast<uint8_t>(std::lround(std::pow(v, 1.0f / 2.2f) * 255.0f));
                }
            }
        }
        std::ofstream png(prefix + ".mirror.raw", std::ios::binary);
        png.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
        png.close();
        index << "mirror_square " << size << " " << size << " 0\n";
        index.flush();
    }
    dump_recorded_ = false;
    LogInfo("render targets written to {}.*.bin", prefix);
    return true;
}

}
