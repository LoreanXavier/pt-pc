#include "engine/render/scene_renderer.h"
#include "engine/render/hdr_output.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <string_view>

#include "engine/assets/ftex.h"
#include "engine/core/log.h"
#include "engine/fs/vfs.h"

namespace pt {
namespace {

constexpr gpu::Format kAlbedoFormat = gpu::Format::R8G8B8A8Unorm;
constexpr gpu::Format kNormalFormat = gpu::Format::R16G16B16A16Sfloat;
constexpr gpu::Format kMaterialFormat = gpu::Format::R8G8B8A8Unorm;
constexpr gpu::Format kDepthFormat = SceneRenderer::kDepthTargetFormat;
constexpr gpu::Format kLightFormat = gpu::Format::R16G16B16A16Sfloat;
constexpr gpu::Format kHdrFormat = SceneRenderer::kHdrTargetFormat;
constexpr gpu::Format kAoFormat = gpu::Format::R8Unorm;
constexpr gpu::Format kRefMapFormat = gpu::Format::R16G16B16A16Sfloat;
constexpr gpu::Format kPostFormat = SceneRenderer::kPostTargetFormat;
constexpr gpu::Format kBloomFormat = gpu::Format::R8G8B8A8Srgb;
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
    const vk::AllocTag tag("shadow atlas");
    uint32_t tile = graphics.shadow_quality<=1?512u:graphics.shadow_quality==2?1024u:2048u;
    tile = std::min(tile, shadow_tile_cap_);
    uint32_t size=tile*4;
    if(shadow_.Valid() && shadow_.Extent().width==size) return true;
    RenderTarget replacement;
    while(!CreateTarget(replacement,kDepthFormat,{size,size},gpu::ImageUsage::DepthStencilAttachment|gpu::ImageUsage::Sampled,gpu::Aspect::Depth)) {
        if(shadow_.Valid() || tile<=512u) return shadow_.Valid();
        tile/=2;
        size=tile*4;
        shadow_tile_cap_=tile;
        LogWarn("graphics: shadow atlas tile lowered to {} px: out of video memory",tile);
    }
    gpu_->WaitIdle();
    gpu_->Submit([&](gpu::CommandList& cmd){
        cmd.UseTargets({{&replacement,gpu::Layout::DepthAttachment}});
        cmd.BeginPass({size,size},{},&replacement,false,true);cmd.EndPass();
        cmd.UseTargets({{&replacement,gpu::Layout::DepthReadOnly}});
    });
    gpu_->DestroyImage(shadow_.image);shadow_=replacement;
    if(extent_.width>0) WriteImageDescriptors();
    LogInfo("graphics: shadow atlas {}x{}, tile {} px",size,size,tile);
    return true;
}

bool SceneRenderer::Init(Renderer& renderer, TextureManager& textures) {
    renderer_ = &renderer;
    hdr_post_ = renderer.OutputMode() != RendererOutputMode::Sdr;
    ldr_format_ = hdr_post_ ? gpu::Format::R16G16B16A16Sfloat : gpu::Format::R8G8B8A8Unorm;
    textures_ = &textures;
    gpu_ = &renderer.Gpu();

    gpu::SamplerDesc sampler;
    sampler.max_anisotropy = 0.0f;
    const gpu::Filter filters[4] = {gpu::Filter::Nearest, gpu::Filter::Linear, gpu::Filter::Nearest, gpu::Filter::Linear};
    const gpu::AddressMode clamp = gpu::AddressMode::ClampToEdge;
    const gpu::AddressMode repeat = gpu::AddressMode::Repeat;
    const gpu::AddressMode modes[4] = {clamp, clamp, repeat, repeat};
    for (int i = 0; i < 4; ++i) {
        sampler.mag = sampler.min = filters[i];
        sampler.mipmap = filters[i] == gpu::Filter::Linear ? gpu::MipmapMode::Linear : gpu::MipmapMode::Nearest;
        sampler.address_u = sampler.address_v = sampler.address_w = modes[i];
        samplers_[i] = gpu_->CreateSampler(sampler);
    }
    sampler.mag = sampler.min = gpu::Filter::Linear;
    sampler.address_u = sampler.address_v = sampler.address_w = gpu::AddressMode::ClampToEdge;
    sampler.compare = true;
    sampler.compare_op = gpu::CompareOp::Less;
    samplers_[gpu::kSmpShadow] = gpu_->CreateSampler(sampler);

    if (!CreateDescriptors()) {
        return false;
    }
    gpu::PipelineLayoutDesc layout_desc;
    layout_desc.sets = {textures.BindingLayout(), frame_layout_};
    layout_desc.push_bytes = 128;
    layout_ = gpu_->CreatePipelineLayout(layout_desc);
    if (!layout_) {
        LogError("scene renderer: scene pipeline layout failed");
        return false;
    }
    if (gpu_->Info().backend == gpu::Backend::D3D12) {
        UpscaleHost::Get().SetD3D12Device(gpu_);
        gpu::BindingLayoutDesc dispatch;
        dispatch.bindings = {{0, gpu::BindingType::UniformBuffer, 1, gpu::ShaderStage::Compute}};
        dispatch_layout_ = gpu_->CreateBindingLayout(dispatch);
        gpu::BindingPoolDesc pool;
        pool.sizes = {{gpu::BindingType::UniformBuffer, Renderer::kFramesInFlight}};
        pool.max_sets = Renderer::kFramesInFlight;
        dispatch_pool_ = gpu_->CreateBindingPool(pool);
        for (FrameSlot& slot : slots_) {
            slot.dispatch_set = gpu_->AllocateBindingSet(dispatch_pool_, dispatch_layout_);
            if (!slot.dispatch_set || !gpu_->CreateBuffer({256, gpu::BufferUsage::UniformBuffer, true}, slot.dispatch)) {
                LogError("scene renderer: DispatchInfo set failed");
                return false;
            }
            const gpu::BufferBinding info{slot.dispatch.buffer, 0, 16};
            const gpu::BindingWrite write{slot.dispatch_set, 0, 0, gpu::BindingType::UniformBuffer, {}, {&info, 1}};
            gpu_->WriteBindings({&write, 1});
        }
    }
    for (FrameSlot& slot : slots_) {
        if (!gpu_->CreateBuffer({sizeof(gpu::FrameData), gpu::BufferUsage::StorageBuffer, true, false, "frame data", true}, slot.frame_data) ||
            !gpu_->CreateBuffer({sizeof(glm::mat4) * gpu::kMaxSkinMatrices, gpu::BufferUsage::StorageBuffer, true, false, "skin matrices", true}, slot.skin) ||
            !gpu_->CreateBuffer({sizeof(glm::vec2) * gpu::kLuminanceEntries, gpu::BufferUsage::StorageBuffer, true}, slot.luminance)) {
            return false;
        }
        slot.queries = gpu_->CreateTimestampPool(kTimestamps);
        slot.upscale_queries = gpu_->CreateTimestampPool(4);
        if (std::getenv("PT_LIGHT_TIMING")) {
            slot.light_queries = gpu_->CreateTimestampPool(2 * gpu::kMaxLights);
        }
    }
    frame_ = new gpu::FrameData();
    CreateBuiltinResources();
    if (!EnsureShadowTarget()) return false;
    LogInfo("startup: scene pipelines");
    if (!CreatePipelines()) {
        return false;
    }
    LogInfo("startup: subsurface scattering pass");
    sss_ready_ = sss_.Init(*gpu_, textures.BindingLayout(), frame_layout_, kLightFormat, samplers_[gpu::kSmpPointClamp]);
    if (gpu_->Info().ray_query && (gpu_->Info().backend == gpu::Backend::Vulkan || D3D12RayTracingAvailable())) {
        LogInfo("startup: ray tracing structures and shadow pass");
        rt_ = CreateRayTracing(*gpu_);
        gpu::GraphicsPipelineDesc volume;
        volume.vertex = "volume.vert";
        volume.fragment = "light_rt.frag";
        volume.colors = {kLightFormat, kLightFormat};
        volume.depth = kDepthFormat;
        volume.depth_test = true;
        volume.depth_compare = gpu::CompareOp::LessOrEqual;
        volume.blend = gpu::BlendMode::Additive;
        volume.depth_bounds = light_depth_bounds_;
        if (rt_->Init(*gpu_, textures.BindingLayout(), frame_layout_)) {
            volume.layout = rt_->Layout();
            light_rt_ = gpu_->CreateGraphicsPipeline(volume);
            volume.fragment = "light_contact.frag";
            light_contact_ = gpu_->CreateGraphicsPipeline(volume);
            LogInfo("startup: ray tracing Full tier passes");
            CreateFullPipelines(kLightFormat, light_depth_bounds_);
            LogInfo("startup: path traced lighting passes");
            CreatePathPipelines(kLightFormat);
            if (pt_direct_) {
                gpu::GraphicsPipelineDesc desc;
                desc.layout = layout_;
                desc.vertex = "mesh.vert";
                desc.fragment = "gbuffer_pt.frag";
                desc.mesh_input = true;
                desc.colors = {kAlbedoFormat, kNormalFormat, kMaterialFormat};
                desc.depth = kDepthFormat;
                desc.depth_test = true;
                desc.depth_write = true;
                pt_gbuffer_ = gpu_->CreateGraphicsPipeline(desc);
                desc.depth_write = false;
                desc.depth_bias = true;
                desc.blend = gpu::BlendMode::Alpha;
                const gpu::ColorMask rgb = gpu::ColorComponent::R | gpu::ColorComponent::G | gpu::ColorComponent::B;
                desc.write_masks = {rgb, rgb, gpu::ColorComponent::R | gpu::ColorComponent::G};
                pt_decal_ = gpu_->CreateGraphicsPipeline(desc);
            }
            LogInfo("startup: ray traced reflection and ambient occlusion passes");
            gpu::GraphicsPipelineDesc reflect;
            reflect.layout = rt_->Layout();
            reflect.fragment = "reflect_make_rt.frag";
            reflect.colors = {kRefMapFormat, kRefMapFormat};
            reflect_make_rt_ = gpu_->CreateGraphicsPipeline(reflect);
            reflect.fragment = "reflect_blend_rt.frag";
            reflect.colors = {kHdrFormat};
            reflect_blend_rt_ = gpu_->CreateGraphicsPipeline(reflect);
            if (!reflect_make_rt_ || !reflect_blend_rt_) {
                LogError("ray tracing: cannot create the ray traced reflection passes, that option stays off");
            }
            reflect.fragment = "reflect_layer_rt.frag";
            reflect.colors = {kHdrFormat, kHdrFormat};
            reflect_layer_rt_ = gpu_->CreateGraphicsPipeline(reflect);
            rt_ao_trace_ = gpu_->CreateComputePipeline({rt_->Layout(), "rt_ao.comp"});
            rt_ao_filter_ = gpu_->CreateComputePipeline({rt_->Layout(), "rt_ao_filter.comp"});
            gpu::GraphicsPipelineDesc probe;
            probe.layout = rt_->Layout();
            probe.vertex = "volume.vert";
            probe.fragment = "probe_ao.frag";
            probe.colors = {kLightFormat};
            probe.blend = gpu::BlendMode::ProbeAccumulate;
            probe_ao_ = gpu_->CreateGraphicsPipeline(probe);
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
    const bool d3d12 = gpu_->Info().backend == gpu::Backend::D3D12;
    gpu::BindingLayoutDesc layout;
    layout.bindings = {{0, gpu::BindingType::StorageBuffer, 1, gpu::ShaderStage::All},
                       {1, gpu::BindingType::StorageBuffer, 1, gpu::ShaderStage::All},
                       {2, gpu::BindingType::SampledImage, gpu::kImageSlots, gpu::ShaderStage::All, gpu::BindingFlag::PartiallyBound},
                       {3, gpu::BindingType::Sampler, d3d12 ? uint32_t(gpu::kSmpShadow) : uint32_t(gpu::kSmpCount), gpu::ShaderStage::All},
                       {4, gpu::BindingType::SampledImage, 1, gpu::ShaderStage::All},
                       {5, gpu::BindingType::StorageBuffer, 1, gpu::ShaderStage::All}};
    if (d3d12) {
        layout.bindings.push_back({6, gpu::BindingType::Sampler, 1, gpu::ShaderStage::All});
    }
    frame_layout_ = gpu_->CreateBindingLayout(layout);
    if (!frame_layout_) {
        LogError("scene renderer: frame set layout failed");
        return false;
    }
    gpu::BindingPoolDesc pool;
    pool.sizes = {{gpu::BindingType::StorageBuffer, 6 * Renderer::kFramesInFlight},
                  {gpu::BindingType::SampledImage, 2 * (gpu::kImageSlots + 1) * Renderer::kFramesInFlight},
                  {gpu::BindingType::Sampler, 2 * gpu::kSmpCount * Renderer::kFramesInFlight}};
    pool.max_sets = 2 * Renderer::kFramesInFlight;
    pool_ = gpu_->CreateBindingPool(pool);
    if (!pool_) {
        LogError("scene renderer: frame descriptor pool failed");
        return false;
    }
    for (FrameSlot& slot : slots_) {
        slot.set = gpu_->AllocateBindingSet(pool_, frame_layout_);
        if (!slot.set) {
            return false;
        }
        slot.post_set = gpu_->AllocateBindingSet(pool_, frame_layout_);
        if (!slot.post_set) {
            return false;
        }
    }
    return true;
}

bool SceneRenderer::CreatePipelines() {
    light_depth_bounds_ = gpu_->Info().depth_bounds && [] {
        const char* v = std::getenv("PT_LIGHT_DEPTH_BOUNDS");
        return !(v && v[0] == '0');
    }();
    gpu::GraphicsPipelineDesc desc;
    desc.layout = layout_;
    desc.vertex = "mesh.vert";
    desc.fragment = "gbuffer.frag";
    desc.mesh_input = true;
    desc.colors = {kAlbedoFormat, kNormalFormat, kMaterialFormat};
    desc.depth = kDepthFormat;
    desc.depth_test = true;
    desc.depth_write = true;
    gbuffer_ = gpu_->CreateGraphicsPipeline(desc);
    desc.depth_write = false;
    desc.depth_bias = true;
    desc.blend = gpu::BlendMode::Alpha;
    const gpu::ColorMask rgb = gpu::ColorComponent::R | gpu::ColorComponent::G | gpu::ColorComponent::B;
    desc.write_masks = {rgb, rgb, gpu::ColorComponent::R | gpu::ColorComponent::G};
    decal_ = gpu_->CreateGraphicsPipeline(desc);

    gpu::GraphicsPipelineDesc shadow;
    shadow.layout = layout_;
    shadow.vertex = "shadow.vert";
    shadow.fragment = "shadow.frag";
    shadow.mesh_input = true;
    shadow.depth = kDepthFormat;
    shadow.depth_test = true;
    shadow.depth_write = true;
    shadow.depth_bias = true;
    shadow_pipeline_ = gpu_->CreateGraphicsPipeline(shadow);

    gpu::GraphicsPipelineDesc volume;
    volume.layout = layout_;
    volume.vertex = "volume.vert";
    volume.fragment = "probe.frag";
    volume.colors = {kLightFormat};
    volume.blend = gpu::BlendMode::ProbeAccumulate;
    probe_ = gpu_->CreateGraphicsPipeline(volume);
    {
        gpu::GraphicsPipelineDesc resolve;
        resolve.layout = layout_;
        resolve.fragment = "probe_resolve.frag";
        resolve.colors = {kLightFormat, kLightFormat};
        resolve.depth = kDepthFormat;
        resolve.write_masks = {gpu::kColorMaskAll, gpu::ColorMask{}};
        probe_resolve_ = gpu_->CreateGraphicsPipeline(resolve);
    }
    volume.colors = {kLightFormat, kLightFormat};
    volume.depth = kDepthFormat;
    volume.depth_test = true;
    volume.depth_compare = gpu::CompareOp::LessOrEqual;
    volume.write_masks = {gpu::kColorMaskAll, gpu::ColorMask{}};
    volume.fragment = "light.frag";
    volume.blend = gpu::BlendMode::Additive;
    volume.write_masks.clear();
    volume.depth_bounds = light_depth_bounds_;
    light_ = gpu_->CreateGraphicsPipeline(volume);

    gpu::GraphicsPipelineDesc full;
    full.layout = layout_;
    full.fragment = "compose.frag";
    full.colors = {kHdrFormat};
    full.depth = kDepthFormat;
    compose_ = gpu_->CreateGraphicsPipeline(full);

    gpu::GraphicsPipelineDesc forward;
    forward.layout = layout_;
    forward.vertex = "mesh.vert";
    forward.fragment = "forward.frag";
    forward.mesh_input = true;
    forward.colors = {kHdrFormat};
    forward.depth = kDepthFormat;
    forward.depth_test = true;
    forward.depth_write = true;
    forward.blend = gpu::BlendMode::PremultipliedFadeAlpha;
    forward_ = gpu_->CreateGraphicsPipeline(forward);
    forward_emissive_ = gpu_->CreateGraphicsPipeline(forward);
    forward.depth_write = false;
    forward_glow_ = gpu_->CreateGraphicsPipeline(forward);

    luminance_ = gpu_->CreateComputePipeline({layout_, "luminance.comp"});
    reflect_colour_ = gpu_->CreateComputePipeline({layout_, "reflect_colour.comp"});

    gpu::GraphicsPipelineDesc velocity;
    velocity.layout = layout_;
    velocity.vertex = "velocity.vert";
    velocity.fragment = "velocity.frag";
    velocity.mesh_input = true;
    velocity.colors = {kPostFormat};
    velocity.depth = kDepthFormat;
    velocity.depth_test = true;
    velocity_pipeline_ = gpu_->CreateGraphicsPipeline(velocity);

    gpu::GraphicsPipelineDesc post;
    post.layout = layout_;
    post.fragment = "bright.frag";
    const gpu::Format bloom_format = BloomFormat();
    post.colors = {bloom_format};
    bright_ = gpu_->CreateGraphicsPipeline(post);
    post.fragment = "kawase.frag";
    kawase_ = gpu_->CreateGraphicsPipeline(post);
    post.blend = gpu::BlendMode::Additive;
    bloom_add_ = gpu_->CreateGraphicsPipeline(post);
    post.blend = gpu::BlendMode::None;
    post.colors = {bloom_format, bloom_format};
    post.blends = {gpu::BlendMode::None, gpu::BlendMode::Additive};
    post.fragment = "kawase_sum.frag";
    kawase_sum_ = gpu_->CreateGraphicsPipeline(post);
    post.blends.clear();
    post.colors = {bloom_format};
    post.fragment = "gaussian.frag";
    gaussian_ = gpu_->CreateGraphicsPipeline(post);
    post.colors = {ldr_format_};
    post.fragment = "tonemap.frag";
    tonemap_ = gpu_->CreateGraphicsPipeline(post);
    post.colors = {ldr_format_};
    post.fragment = "fxaa.frag";
    fxaa_ = gpu_->CreateGraphicsPipeline(post);
    post.fragment = "date_resolve.frag";
    date_resolve_out_ = gpu_->CreateGraphicsPipeline(post);
    post.colors = {kPostFormat};
    date_resolve_ = gpu_->CreateGraphicsPipeline(post);
    post.colors = {ldr_format_};
    post.colors = {kHdrFormat};
    post.fragment = "mirror_temporal.frag";
    mirror_temporal_pipeline_ = gpu_->CreateGraphicsPipeline(post);
    post.colors = {ldr_format_};
    post.fragment = "dof_blend.frag";
    dof_blend_ = gpu_->CreateGraphicsPipeline(post);
    post.fragment = "dof_merge.frag";
    dof_merge_ = gpu_->CreateGraphicsPipeline(post);
    post.colors = {kPostFormat, kPostFormat};
    post.fragment = "dof_layers.frag";
    dof_layers_ = gpu_->CreateGraphicsPipeline(post);
    post.fragment = "dof_gather.frag";
    dof_gather_ = gpu_->CreateGraphicsPipeline(post);
    post.colors = {ldr_format_};
    post.fragment = "dof_ratio.frag";
    post.write_mask = gpu::ColorComponent::A;
    dof_ratio_ = gpu_->CreateGraphicsPipeline(post);
    post.write_mask = gpu::kColorMaskAll;
    post.fragment = "mb_composite.frag";
    post.blend = gpu::BlendMode::Alpha;
    mb_composite_ = gpu_->CreateGraphicsPipeline(post);
    post.blend = gpu::BlendMode::None;
    post.colors = {kPostFormat};
    mb_decode_ = gpu_->CreateGraphicsPipeline(post);
    post.fragment = "dof_down.frag";
    dof_down_ = gpu_->CreateGraphicsPipeline(post);
    post.fragment = "dof_blur.frag";
    dof_blur_ = gpu_->CreateGraphicsPipeline(post);
    post.fragment = "mb_velocity.frag";
    mb_velocity_ = gpu_->CreateGraphicsPipeline(post);
    post.fragment = "mb_tile.frag";
    mb_tile_pipeline_ = gpu_->CreateGraphicsPipeline(post);
    post.fragment = "mb_bake.frag";
    mb_bake_pipeline_ = gpu_->CreateGraphicsPipeline(post);
    post.fragment = "mb_mcguire.frag";
    mb_mcguire_ = gpu_->CreateGraphicsPipeline(post);
    post.colors = {ldr_format_};
    post.fragment = "fsblur.frag";
    fsblur_ = gpu_->CreateGraphicsPipeline(post);
    post.fragment = "banding.frag";
    banding_ = gpu_->CreateGraphicsPipeline(post);
    post.fragment = "screen_fx.frag";
    post.colors = {renderer_->SceneColorFormat()};
    screen_fx_ = gpu_->CreateGraphicsPipeline(post);
    post.fragment = "debug.frag";
    debug_ = gpu_->CreateGraphicsPipeline(post);
    post.fragment = "subliminal_string.frag";
    post.colors = {ldr_format_};
    post.blend = gpu::BlendMode::None;
    subliminal_string_ = gpu_->CreateGraphicsPipeline(post);
    post.fragment = "backdrop.frag";
    backdrop_ = gpu_->CreateGraphicsPipeline(post);
    post.blend = gpu::BlendMode::None;
    post.fragment = "ssao.frag";
    post.colors = {kAoFormat};
    occlusion_ = gpu_->CreateGraphicsPipeline(post);
    post.fragment = "ssao_blur.frag";
    occlusion_blur_ = gpu_->CreateGraphicsPipeline(post);
    post.fragment = "reflect_make.frag";
    post.colors = {kRefMapFormat};
    reflect_make_ = gpu_->CreateGraphicsPipeline(post);
    post.fragment = "reflect_blend.frag";
    post.colors = {kHdrFormat};
    reflect_blend_ = gpu_->CreateGraphicsPipeline(post);
    post.colors = {kHdrFormat, kHdrFormat};
    post.fragment = "reflect_layer.frag";
    reflect_layer_pipeline_ = gpu_->CreateGraphicsPipeline(post);
    post.fragment = "reflect_temporal.frag";
    reflect_temporal_pipeline_ = gpu_->CreateGraphicsPipeline(post);
    post.colors = {kHdrFormat};
    post.fragment = "vfx_composite.frag";
    vfx_composite_ = gpu_->CreateGraphicsPipeline(post);
    post.fragment = "vfx_depth_down.frag";
    post.colors = {kNearFarDepthFormat};
    post.depth = kDepthFormat;
    post.depth_test = true;
    post.depth_write = true;
    post.depth_compare = gpu::CompareOp::Always;
    vfx_depth_down_ = gpu_->CreateGraphicsPipeline(post);

    const gpu::PipelineHandle all[] = {gbuffer_, decal_, shadow_pipeline_, probe_, probe_resolve_, light_, compose_, forward_, forward_emissive_, forward_glow_, luminance_, bright_,
                              reflect_colour_, kawase_, bloom_add_, kawase_sum_, gaussian_, tonemap_, fxaa_, dof_ratio_, dof_down_, dof_blur_, dof_blend_,
                              dof_layers_, dof_gather_, dof_merge_, mb_velocity_,
                              mb_tile_pipeline_, mb_bake_pipeline_, mb_mcguire_, mb_composite_, mb_decode_, velocity_pipeline_, fsblur_, banding_, screen_fx_,
                              debug_, occlusion_, occlusion_blur_, reflect_make_, reflect_blend_, vfx_composite_, vfx_depth_down_, mirror_temporal_pipeline_,
                              reflect_layer_pipeline_, reflect_temporal_pipeline_, subliminal_string_, backdrop_, date_resolve_, date_resolve_out_};
    for (gpu::PipelineHandle p : all) {
        if (!p) {
            LogError("scene renderer: pipeline creation failed");
            return false;
        }
    }
    LogInfo("startup: upscaler input passes");
    return CreateUpscalePipelines();
}

gpu::Format SceneRenderer::BloomFormat() const {
    return hdr_post_ ? gpu::Format::R16G16B16A16Sfloat : kBloomFormat;
}

void SceneRenderer::SetHdrPost(bool on) {
    if (on == hdr_post_) {
        return;
    }
    gpu_->WaitIdle();
    hdr_post_ = on;
    ldr_format_ = on ? gpu::Format::R16G16B16A16Sfloat : gpu::Format::R8G8B8A8Unorm;
    DestroyPipelines();
    if (!CreatePipelines()) {
        LogError("graphics: the {} post chain's pipelines could not be made", on ? "HDR" : "SDR");
    }
    history_valid_ = false;
    date_history_valid_ = false;
    LogInfo("graphics: post chain for {} output ({} targets)", on ? "an HDR" : "an SDR", on ? "half float" : "8-bit");
}

void SceneRenderer::DestroyPipelines() {
    gpu::PipelineHandle* all[] = {&gbuffer_, &decal_, &shadow_pipeline_, &probe_, &probe_resolve_, &light_, &compose_, &forward_, &forward_emissive_, &forward_glow_, &luminance_, &bright_,
                         &reflect_colour_, &kawase_, &bloom_add_, &kawase_sum_, &gaussian_, &tonemap_, &fxaa_, &dof_ratio_, &dof_down_, &dof_blur_, &dof_blend_,
                         &dof_layers_, &dof_gather_, &dof_merge_,
                         &mb_velocity_, &mb_tile_pipeline_, &mb_bake_pipeline_, &mb_mcguire_, &mb_composite_, &mb_decode_, &velocity_pipeline_, &fsblur_,
                         &banding_, &screen_fx_, &debug_, &occlusion_, &occlusion_blur_, &reflect_make_, &reflect_blend_, &vfx_composite_, &vfx_depth_down_,
                         &mirror_temporal_pipeline_, &reflect_layer_pipeline_, &reflect_temporal_pipeline_, &subliminal_string_, &backdrop_,
                         &date_resolve_, &date_resolve_out_};
    for (gpu::PipelineHandle* p : all) {
        if (*p) {
            gpu_->DestroyPipeline(*p);
            *p = {};
        }
    }
    DestroyUpscalePipelines();
}

void SceneRenderer::Shutdown() {
    if (!renderer_) {
        return;
    }
    gpu_->WaitIdle();
    for (FrameSlot& slot : slots_) ReadUpscaleTimestamps(slot);
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
                                "velocity,ssao,rt_ao,probes,lights,sss,device_mb,frame,rt_classify,rt_gi,rt_glossy,rt_resolve,pt_direct,pt_indirect,pt_denoise,"
                                "pt_layers\n");
                for (size_t i = 0; i < n; ++i) {
                    const FrameTiming& t = timing_ring_[(timing_count_ - n + i) % timing_ring_.size()];
                    std::fprintf(f, "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.1f,%llu,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n", t.gpu_ms, t.pass_ms[0],
                                 t.pass_ms[1], t.pass_ms[2], t.pass_ms[3], t.pass_ms[4], t.pass_ms[5], t.part_ms[0], t.part_ms[1], t.part_ms[2],
                                 t.part_ms[3], t.part_ms[4], t.part_ms[5], t.light_ms[0], t.light_ms[1], t.light_ms[2],
                                 t.light_ms[3], t.light_ms[4], t.light_ms[5], t.device_mb, static_cast<unsigned long long>(t.frame), t.rt_ms[0],
                                 t.rt_ms[1], t.rt_ms[2], t.rt_ms[3], t.pt_ms[0], t.pt_ms[1], t.pt_ms[2], t.pt_ms[3]);
                }
                std::fclose(f);
            }
        }
    }
    if (const char* csv = std::getenv("PT_FRAME_CSV"); csv && !frame_trace_.empty()) {
        if (FILE* f = std::fopen(csv, "w")) {
            std::fprintf(f, "frame,cpu_ms,interval_ms,lights,shadow_views,draws,pipelines,pipeline_ms\n");
            for (size_t i = 0; i < frame_trace_.size(); ++i) {
                const FrameTrace& t = frame_trace_[i];
                std::fprintf(f, "%zu,%.4f,%.4f,%u,%u,%u,%u,%.3f\n", i, t.cpu_ms, t.interval_ms, t.lights, t.shadow_views, t.draws, t.pipelines,
                             t.pipeline_ms);
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
    DestroyFullPipelines();
    DestroyPathPipelines();
    for (gpu::PipelineHandle* p : {&light_rt_, &light_contact_, &reflect_make_rt_, &reflect_blend_rt_, &reflect_layer_rt_, &rt_ao_trace_, &rt_ao_filter_, &probe_ao_}) {
        if (*p) {
            gpu_->DestroyPipeline(*p);
            *p = {};
        }
    }
    DestroyPipelines();
    DestroyTargets();
    gpu_->DestroyBuffer(dump_compose_);
    gpu_->DestroyBuffer(dump_forward_);
    gpu_->DestroyBuffer(dump_scene_);
    gpu_->DestroyBuffer(dump_mirror_);
    gpu_->DestroyImage(shadow_.image);
    gpu::ImageResource* images[] = {&lut1_, &lut2_, &material_tex_, &dither_, &color_lut_, &color_lut_prev_, &noise_, &mask_, &white_};
    for (gpu::ImageResource* image : images) {
        gpu_->DestroyImage(*image);
    }
    for (FrameSlot& slot : slots_) {
        gpu_->DestroyBuffer(slot.frame_data);
        gpu_->DestroyBuffer(slot.skin);
        gpu_->DestroyBuffer(slot.luminance);
        gpu_->DestroyBuffer(slot.dispatch);
        gpu_->DestroyQueryPool(slot.queries);
        gpu_->DestroyQueryPool(slot.upscale_queries);
        if (slot.neural_queries) gpu_->DestroyQueryPool(slot.neural_queries);
        if (slot.light_queries) gpu_->DestroyQueryPool(slot.light_queries);
    }
    gpu_->DestroyPipelineLayout(layout_);
    gpu_->DestroyBindingPool(pool_);
    gpu_->DestroyBindingLayout(frame_layout_);
    if (dispatch_pool_) gpu_->DestroyBindingPool(dispatch_pool_);
    if (dispatch_layout_) gpu_->DestroyBindingLayout(dispatch_layout_);
    for (gpu::SamplerHandle s : samplers_) {
        gpu_->DestroySampler(s);
    }
    delete frame_;
    frame_ = nullptr;
    renderer_ = nullptr;
}

bool SceneRenderer::CreateTarget(RenderTarget& target, gpu::Format format, gpu::Extent2D extent, gpu::ImageUsageFlags usage, gpu::AspectFlags aspect) {
    target = RenderTarget{};
    target.aspect = aspect;
    gpu::ImageDesc desc;
    desc.format = format;
    desc.extent = {std::max(extent.width, 1u), std::max(extent.height, 1u), 1};
    desc.usage = usage;
    desc.aspect = aspect;
    return gpu_->CreateImage(desc, target.image);
}

void SceneRenderer::DestroyTargets() {
    RenderTarget* targets[] = {&albedo_, &normal_, &material_, &depth_, &flare_depth_, &diffuse_, &specular_, &probe_acc_, &hdr_, &bloom_[0], &bloom_[1], &bloom_[2],
                               &flare_, &ldr_[0], &ldr_[1], &history_, &mirror_, &mirror_capture_cache_[0], &mirror_capture_cache_[1], &ao_[0], &ao_[1], &refmap_, &hdr_copy_, &particles_,
                               &particles_near_, &particles_far_, &near_far_depth_, &near_far_depth_attachment_, &dof_half_,
                               &dof_quarter_[0], &dof_quarter_[1], &dof_eighth_[0], &dof_eighth_[1], &velocity_, &object_velocity_,
                               &mb_tile_[0], &mb_tile_[1], &mb_tile_[2], &mb_tile_[3], &mb_tile_[4], &mb_neighbour_, &mb_bake_,
                               &mb_blur_[0], &mb_blur_[1], &mb_original_bake_, &mb_original_blur_[0], &mb_original_blur_[1],
                               &mirror_history_, &mirror_temporal_, &reflect_layer_, &reflect_offset_,
                               &reflect_history_[0], &reflect_history_[1], &nr_in_, &nr_out_};
    for (RenderTarget* t : targets) {
        gpu_->DestroyImage(t->image);
        *t = RenderTarget{};
    }
    nr_feature_ready_ = false;
    nr_extent_ = {};
    gpu_->DestroyImage(refmap_color_.image);
    refmap_color_ = RenderTarget{};
    sss_.DestroyTargets();
    for (RenderTarget& t : rt_ao_images_) {
        gpu_->DestroyImage(t.image);
        t = RenderTarget{};
    }
    rt_ao_history_ = false;
    DestroyFullTargets();
    DestroyPathTargets();
    mirror_history_valid_ = false;
    mirror_history_size_ = 0;
    reflect_history_valid_ = false;
    DestroyUpscaleTargets();
    extent_ = {0, 0};
    output_extent_ = {0, 0};
}

bool SceneRenderer::EnsureTargets(gpu::Extent2D extent, gpu::Extent2D output, bool upscaling) {
    if (extent.width == 0 || extent.height == 0 || output.width == 0 || output.height == 0) {
        return false;
    }
    if (extent.width == extent_.width && extent.height == extent_.height && output.width == output_extent_.width &&
        output.height == output_extent_.height && upscaling == targets_upscaled_ && ldr_[0].image.format == ldr_format_) {
        return true;
    }
    gpu_->WaitIdle();
    DestroyTargets();
    const vk::AllocTag tag("scene targets");
    mirror_stale_ = {};
    mirror_stale_cache_[0] = {};
    mirror_stale_cache_[1] = {};
    mirror_cache_index_ = mirror_render_cache_index_ = 0;
    const gpu::ImageUsageFlags color = gpu::ImageUsage::ColorAttachment | gpu::ImageUsage::Sampled;
    const gpu::ImageUsageFlags dumped = color | gpu::ImageUsage::TransferSrc;
    const uint32_t bloom_width = std::max(1u, static_cast<uint32_t>(std::lround(270.0 * output.width / std::max(output.height, 1u))));
    const gpu::Extent2D quarter{bloom_width, 270u};
    const gpu::AspectFlags c = gpu::Aspect::Color;
    bool ok = CreateTarget(albedo_, kAlbedoFormat, extent, dumped, c) && CreateTarget(normal_, kNormalFormat, extent, dumped, c) &&
              CreateTarget(material_, kMaterialFormat, extent, dumped, c) &&
              CreateTarget(depth_, kDepthFormat, extent,
                           gpu::ImageUsage::DepthStencilAttachment | gpu::ImageUsage::Sampled | gpu::ImageUsage::TransferSrc,
                           gpu::Aspect::Depth) &&
              CreateTarget(flare_depth_, kDepthFormat, extent,
                           gpu::ImageUsage::DepthStencilAttachment | gpu::ImageUsage::Sampled | gpu::ImageUsage::TransferDst,
                           gpu::Aspect::Depth) &&
              CreateTarget(diffuse_, kLightFormat, extent, dumped, c) && CreateTarget(specular_, kLightFormat, extent, dumped, c) &&
              CreateTarget(probe_acc_, kLightFormat, extent, dumped, c) &&
              CreateTarget(hdr_, kHdrFormat, extent, color | gpu::ImageUsage::TransferSrc | gpu::ImageUsage::Storage, c) &&
              CreateTarget(hdr_copy_, kHdrFormat, extent, color | gpu::ImageUsage::TransferDst, c) &&
              CreateTarget(nr_in_, kHdrFormat, extent, color | gpu::ImageUsage::TransferSrc, c) &&
              CreateTarget(nr_out_, kHdrFormat, extent, color | gpu::ImageUsage::Storage | gpu::ImageUsage::TransferDst, c) &&
              CreateTarget(particles_, kHdrFormat, extent, dumped, c) &&
              CreateTarget(particles_near_, kHdrFormat, {(extent.width + 1) / 2, (extent.height + 1) / 2}, dumped, c) &&
              CreateTarget(particles_far_, kHdrFormat, {(extent.width + 1) / 2, (extent.height + 1) / 2}, dumped, c) &&
              CreateTarget(near_far_depth_, kNearFarDepthFormat, {(extent.width + 1) / 2, (extent.height + 1) / 2}, dumped, c) &&
              CreateTarget(near_far_depth_attachment_, kDepthFormat, {(extent.width + 1) / 2, (extent.height + 1) / 2},
                           gpu::ImageUsage::DepthStencilAttachment | gpu::ImageUsage::Sampled | gpu::ImageUsage::TransferSrc,
                           gpu::Aspect::Depth) &&
              CreateTarget(refmap_, kRefMapFormat, extent, color, c) &&
              CreateTarget(mirror_, kHdrFormat, extent, color | gpu::ImageUsage::TransferDst | gpu::ImageUsage::TransferSrc, c) &&
              CreateTarget(mirror_capture_cache_[0], kHdrFormat, {std::min(128u, extent.width), std::min(128u, extent.height)},
                           color | gpu::ImageUsage::TransferDst | gpu::ImageUsage::TransferSrc, c) &&
              CreateTarget(mirror_capture_cache_[1], kHdrFormat, {std::min(512u, extent.width), std::min(512u, extent.height)},
                           color | gpu::ImageUsage::TransferDst | gpu::ImageUsage::TransferSrc, c) &&
              CreateTarget(bloom_[0], BloomFormat(), quarter, color, c) && CreateTarget(bloom_[1], BloomFormat(), quarter, color, c) &&
              CreateTarget(bloom_[2], BloomFormat(), quarter, color, c) && CreateTarget(flare_, kPostFormat, output, color, c) &&
              CreateTarget(ldr_[0], ldr_format_, output, color | gpu::ImageUsage::TransferSrc, c) &&
              CreateTarget(ldr_[1], ldr_format_, output, color | gpu::ImageUsage::TransferSrc, c) &&
              CreateTarget(history_, ldr_format_, output, color | gpu::ImageUsage::TransferDst, c) &&
              CreateTarget(ao_[0], kAoFormat, extent, color, c) && CreateTarget(ao_[1], kAoFormat, extent, color | gpu::ImageUsage::TransferSrc, c);
    const gpu::Extent2D shift1{std::max(output.width >> 1, 1u), std::max(output.height >> 1, 1u)};
    const gpu::Extent2D shift2 = hdr_post_ ? shift1 : gpu::Extent2D{std::max(output.width >> 2, 1u), std::max(output.height >> 2, 1u)};
    const gpu::Extent2D shift3 = hdr_post_ ? shift1 : gpu::Extent2D{std::max(output.width >> 3, 1u), std::max(output.height >> 3, 1u)};
    ok = ok && CreateTarget(dof_half_, kPostFormat, shift1, color, c) && CreateTarget(dof_quarter_[0], kPostFormat, shift2, color, c) &&
         CreateTarget(dof_quarter_[1], kPostFormat, shift2, color, c) && CreateTarget(dof_eighth_[0], kPostFormat, shift3, color, c) &&
         CreateTarget(dof_eighth_[1], kPostFormat, shift3, color, c) && CreateTarget(velocity_, kPostFormat, output, color, c) &&
         CreateTarget(object_velocity_, kPostFormat, extent, color, c) && CreateTarget(mb_blur_[0], kPostFormat, output, color, c) &&
         CreateTarget(mb_blur_[1], kPostFormat, output, color, c) &&
         CreateTarget(mb_original_bake_, kPostFormat, shift1, color, c) &&
         CreateTarget(mb_original_blur_[0], kPostFormat, shift1, color, c) &&
         CreateTarget(mb_original_blur_[1], kPostFormat, shift1, color, c);
    for (uint32_t i = 0; i < 5 && ok; ++i) {
        const uint32_t round = (2u << i) - 1u;
        ok = CreateTarget(mb_tile_[i], kPostFormat, {(output.width + round) >> (i + 1), (output.height + round) >> (i + 1)}, color, c);
    }
    ok = ok && CreateTarget(mb_neighbour_, kPostFormat, mb_tile_[4].Extent(), color, c) &&
         CreateTarget(mb_bake_, kPostFormat, output, color, c);
    ok = ok && (!upscaling || CreateUpscaleTargets(extent, output));
    ok = ok && (!upscaling ||
                (CreateTarget(mirror_history_, kHdrFormat, extent, color | gpu::ImageUsage::TransferDst, c) &&
                 CreateTarget(mirror_temporal_, kHdrFormat, extent, color | gpu::ImageUsage::TransferSrc, c) &&
                 CreateTarget(reflect_layer_, kHdrFormat, extent, color, c) && CreateTarget(reflect_offset_, kHdrFormat, extent, color, c) &&
                 CreateTarget(reflect_history_[0], kHdrFormat, extent, color, c) &&
                 CreateTarget(reflect_history_[1], kHdrFormat, extent, color, c)));
    ok = ok && (!rt_ || CreateTarget(refmap_color_, kRefMapFormat, extent, color, c));
    if (ok && sss_ready_ && !sss_.CreateTargets(extent)) {
        LogWarn("scene renderer: subsurface scattering off at {}x{}: out of video memory", extent.width, extent.height);
    }
    if (!ok) {
        LogError("scene renderer: cannot create render targets {}x{}", extent.width, extent.height);
        return false;
    }
    gpu_->Submit([&](gpu::CommandList& cmd) {
        RenderTarget* colors[] = {&albedo_, &normal_, &material_, &diffuse_, &specular_, &probe_acc_, &hdr_, &mirror_, &mirror_capture_cache_[0], &mirror_capture_cache_[1], &bloom_[0], &bloom_[1], &bloom_[2],
                                  &flare_, &ldr_[0], &ldr_[1], &history_, &ao_[0], &ao_[1], &refmap_, &hdr_copy_, &particles_,
                                  &particles_near_, &particles_far_, &near_far_depth_, &dof_half_,
                                  &dof_quarter_[0], &dof_quarter_[1], &dof_eighth_[0], &dof_eighth_[1], &velocity_, &object_velocity_, &mb_tile_[0],
                                  &mb_tile_[1], &mb_tile_[2], &mb_tile_[3], &mb_tile_[4], &mb_neighbour_, &mb_bake_, &mb_blur_[0],
                                  &mb_blur_[1], &mb_original_bake_, &mb_original_blur_[0], &mb_original_blur_[1], &nr_in_, &nr_out_};
        for (RenderTarget* t : colors) {
            cmd.UseTargets({{t, gpu::Layout::ColorAttachment}});
            cmd.BeginPass(t->Extent(), {{t, true, {}}});
            cmd.EndPass();
            cmd.UseTargets({{t, gpu::Layout::ShaderReadOnly}});
        }
        cmd.UseTargets({{&depth_, gpu::Layout::DepthAttachment}});
        cmd.BeginPass(extent, {}, &depth_, false, true);
        cmd.EndPass();
        cmd.UseTargets({{&depth_, gpu::Layout::DepthReadOnly}});
        cmd.UseTargets({{&flare_depth_, gpu::Layout::DepthAttachment}});
        cmd.BeginPass(extent, {}, &flare_depth_, false, true);
        cmd.EndPass();
        cmd.UseTargets({{&flare_depth_, gpu::Layout::DepthReadOnly}});
        cmd.UseTargets({{&near_far_depth_attachment_, gpu::Layout::DepthAttachment}});
        cmd.BeginPass(near_far_depth_attachment_.Extent(), {}, &near_far_depth_attachment_, false, true);
        cmd.EndPass();
        cmd.UseTargets({{&near_far_depth_attachment_, gpu::Layout::DepthReadOnly}});
        RenderTarget* upscale_colors[] = {&motion_, &reactive_, &opaque_, &post_hdr_, &post_depth_, &post_object_velocity_, &refmap_color_,
                                         &mirror_history_, &mirror_temporal_, &reflect_layer_, &reflect_offset_, &reflect_history_[0],
                                         &reflect_history_[1]};
        for (RenderTarget* t : upscale_colors) {
            if (t->Valid()) {
                cmd.UseTargets({{t, gpu::Layout::ColorAttachment}});
                cmd.BeginPass(t->Extent(), {{t, true, {}}});
                cmd.EndPass();
                cmd.UseTargets({{t, gpu::Layout::ShaderReadOnly}});
            }
        }
        RenderTarget* upscale_storage[] = {&upscaled_, &exposure_image_};
        for (RenderTarget* t : upscale_storage) {
            if (t->Valid()) {
                cmd.UseTargets({{t, gpu::Layout::TransferDst}});
                cmd.ClearColorImage(t->image, gpu::Layout::TransferDst, gpu::ClearColor{}, 1, 1);
                cmd.UseTargets({{t, gpu::Layout::ShaderReadOnly}});
            }
        }
    });
    extent_ = extent;
    output_extent_ = output;
    targets_upscaled_ = upscaling;
    history_valid_ = false;
    date_history_valid_ = false;
    WriteImageDescriptors();
    return true;
}

void SceneRenderer::WriteImageDescriptors() {
    gpu::ImageBinding images[gpu::kImageSlots];
    for (gpu::ImageBinding& info : images) {
        info = {white_.view, gpu::Layout::ShaderReadOnly, {}, &white_};
    }
    auto set_target = [&](uint32_t slot, const RenderTarget& t) {
        if (t.Valid()) {
            images[slot].view = t.image.view;
            images[slot].image = &t.image;
            images[slot].layout =
                gpu::AspectFlags(t.aspect) == gpu::Aspect::Depth ? gpu::Layout::DepthReadOnly : gpu::Layout::ShaderReadOnly;
        }
    };
    auto set_image = [&](uint32_t slot, const gpu::ImageResource& image) {
        if (image.view) {
            images[slot].view = image.view;
            images[slot].image = &image;
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
    set_target(gpu::kImgMbOriginalBake, mb_original_bake_);
    set_target(gpu::kImgMbOriginalBlurA, mb_original_blur_[0]);
    set_target(gpu::kImgMbOriginalBlurB, mb_original_blur_[1]);
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
    set_target(gpu::kImgNeuralIn, nr_in_);
    set_target(gpu::kImgNeuralOut, nr_out_);
    set_target(gpu::kImgParticles, particles_);
    set_target(gpu::kImgParticlesNear, particles_near_);
    set_target(gpu::kImgParticlesFar, particles_far_);
    set_target(gpu::kImgNearFarDepth, near_far_depth_);
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
    const bool d3d12 = gpu_->Info().backend == gpu::Backend::D3D12;
    const gpu::ImageBinding lut2_info{lut2_.view, gpu::Layout::ShaderReadOnly, {}, &lut2_};
    WriteUpscaleDescriptors(images, false);
    gpu::ImageBinding post_images[gpu::kImageSlots];
    std::copy(std::begin(images), std::end(images), std::begin(post_images));
    WriteUpscaleDescriptors(post_images, true);
    for (int set_index = 0; set_index < 2 * static_cast<int>(Renderer::kFramesInFlight); ++set_index) {
        FrameSlot& slot = slots_[set_index / 2];
        const bool post = (set_index & 1) != 0;
        const gpu::BindingSetHandle set = post ? slot.post_set : slot.set;
        const gpu::BufferBinding frame_info{slot.frame_data.buffer};
        const gpu::BufferBinding skin_info{slot.skin.buffer};
        const gpu::BufferBinding luminance_info{slot.luminance.buffer};
        gpu::BindingWrite writes[7];
        writes[0] = {set, 0, 0, gpu::BindingType::StorageBuffer, {}, {&frame_info, 1}};
        writes[1] = {set, 1, 0, gpu::BindingType::StorageBuffer, {}, {&skin_info, 1}};
        writes[2] = {set, 2, 0, gpu::BindingType::SampledImage, {post ? post_images : images, gpu::kImageSlots}};
        writes[3] = {set, 3, 0, gpu::BindingType::Sampler, {}, {}, {samplers_, d3d12 ? size_t(gpu::kSmpShadow) : size_t(gpu::kSmpCount)}};
        writes[4] = {set, 4, 0, gpu::BindingType::SampledImage, {&lut2_info, 1}};
        writes[5] = {set, 5, 0, gpu::BindingType::StorageBuffer, {}, {&luminance_info, 1}};
        writes[6] = {set, 6, 0, gpu::BindingType::Sampler, {}, {}, {&samplers_[gpu::kSmpShadow], 1}};
        gpu_->WriteBindings({writes, d3d12 ? 7u : 6u});
    }
}

bool SceneRenderer::UploadImage(gpu::ImageResource& image, gpu::Format format, gpu::Extent3D extent, const void* data, size_t size) {
    const vk::AllocTag tag("scene images");
    bool created = false;
    if (!image.image || image.format != format || image.extent.width != extent.width || image.extent.height != extent.height ||
        image.extent.depth != extent.depth) {
        if (image.image) {
            gpu_->WaitIdle();
            gpu_->DestroyImage(image);
        }
        if (!gpu_->CreateImage({format, extent, gpu::ImageUsage::Sampled | gpu::ImageUsage::TransferDst}, image)) {
            return false;
        }
        created = true;
    }
    gpu::BufferResource staging;
    if (!MakeBuffer(staging, size, gpu::BufferUsage::TransferSrc, true)) {
        if (created) gpu_->DestroyImage(image);
        return false;
    }
    std::memcpy(staging.mapped, data, size);
    gpu_->FlushMapped(staging, 0, size);
    gpu_->Submit([&](gpu::CommandList& cmd) {
        cmd.Barrier({image.image, gpu::Aspect::Color, gpu::PipelineStage::AllCommands, 0, gpu::Layout::Undefined, gpu::PipelineStage::Transfer,
                     gpu::Bits(gpu::Access::TransferWrite), gpu::Layout::TransferDst});
        gpu::BufferImageCopy region;
        region.image = {gpu::Aspect::Color, 0, 0, 1};
        region.extent = extent;
        cmd.CopyBufferToImage(staging, image, gpu::Layout::TransferDst, {&region, 1});
        cmd.Barrier({image.image, gpu::Aspect::Color, gpu::PipelineStage::Transfer, gpu::Bits(gpu::Access::TransferWrite),
                     gpu::Layout::TransferDst, gpu::PipelineStage::AllCommands, gpu::Bits(gpu::Access::ShaderSampledRead),
                     gpu::Layout::ShaderReadOnly});
    });
    gpu_->DestroyBuffer(staging);
    return true;
}

bool SceneRenderer::UploadImageMips(gpu::ImageResource& image, gpu::Format format, gpu::Extent3D extent, const std::vector<std::vector<uint8_t>>& mips) {
    const vk::AllocTag tag("scene images");
    const uint32_t levels = static_cast<uint32_t>(mips.size());
    if (image.image) {
        gpu_->WaitIdle();
        gpu_->DestroyImage(image);
    }
    if (levels == 0 || !gpu_->CreateImage({format, extent, gpu::ImageUsage::Sampled | gpu::ImageUsage::TransferDst, levels}, image)) {
        return false;
    }
    size_t total = 0;
    for (const std::vector<uint8_t>& mip : mips) {
        total += mip.size();
    }
    gpu::BufferResource staging;
    if (!MakeBuffer(staging, total, gpu::BufferUsage::TransferSrc, true)) {
        gpu_->DestroyImage(image);
        return false;
    }
    std::vector<gpu::BufferImageCopy> regions;
    size_t offset = 0;
    for (uint32_t level = 0; level < levels; ++level) {
        std::memcpy(static_cast<uint8_t*>(staging.mapped) + offset, mips[level].data(), mips[level].size());
        gpu::BufferImageCopy region;
        region.buffer_offset = offset;
        region.image = {gpu::Aspect::Color, level, 0, 1};
        region.extent = {std::max(1u, extent.width >> level), std::max(1u, extent.height >> level), 1};
        regions.push_back(region);
        offset += mips[level].size();
    }
    gpu_->FlushMapped(staging, 0, total);
    gpu_->Submit([&](gpu::CommandList& cmd) {
        cmd.Barrier({image.image, gpu::Aspect::Color, gpu::PipelineStage::AllCommands, 0, gpu::Layout::Undefined, gpu::PipelineStage::Transfer,
                     gpu::Bits(gpu::Access::TransferWrite), gpu::Layout::TransferDst, 0, levels, 0, 1});
        cmd.CopyBufferToImage(staging, image, gpu::Layout::TransferDst, regions);
        cmd.Barrier({image.image, gpu::Aspect::Color, gpu::PipelineStage::Transfer, gpu::Bits(gpu::Access::TransferWrite),
                     gpu::Layout::TransferDst, gpu::PipelineStage::AllCommands, gpu::Bits(gpu::Access::ShaderSampledRead),
                     gpu::Layout::ShaderReadOnly, 0, levels, 0, 1});
    });
    gpu_->DestroyBuffer(staging);
    return true;
}

void SceneRenderer::CreateBuiltinResources() {
    const uint8_t white[4] = {255, 255, 255, 255};
    UploadImage(white_, gpu::Format::R8G8B8A8Unorm, {1, 1, 1}, white, 4);
    UploadImage(noise_, gpu::Format::R8G8B8A8Unorm, {1, 1, 1}, white, 4);
    UploadImage(mask_, gpu::Format::R8G8B8A8Unorm, {1, 1, 1}, white, 4);

    std::vector<uint8_t> lut1(64 * 64);
    for (int j = 0; j < 64; ++j) {
        for (int i = 0; i < 64; ++i) {
            const double c = i / 63.0;
            const double f0 = j / 63.0;
            lut1[j * 64 + i] = static_cast<uint8_t>(std::floor(255.0 * (f0 + (1.0 - f0) * std::pow(1.0 - c, 5.0))));
        }
    }
    UploadImage(lut1_, gpu::Format::R8Unorm, {64, 64, 1}, lut1.data(), lut1.size());

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
    UploadImage(lut2_, gpu::Format::R8G8Unorm, {64, 16, 16}, lut2.data(), lut2.size());

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
    UploadImage(dither_, gpu::Format::R8G8B8A8Unorm, {8, 8, 1}, dither.data(), dither.size());

    std::vector<uint8_t> material(256 * 2 * 4, 0);
    for (int i = 0; i < 256; ++i) {
        uint8_t* row0 = &material[i * 4];
        uint8_t* row1 = &material[(256 + i) * 4];
        row0[0] = row0[1] = row0[2] = 255;
        row1[0] = 10;
    }
    UploadImage(material_tex_, gpu::Format::R8G8B8A8Unorm, {256, 2, 1}, material.data(), material.size());

    ScreenSettings identity;
    std::vector<uint8_t> lut;
    BuildColorLut(std::string(), identity, lut);
    UploadImage(color_lut_, gpu::Format::R8G8B8A8Unorm, {256, 16, 1}, lut.data(), lut.size());
    UploadImage(color_lut_prev_, gpu::Format::R8G8B8A8Unorm, {256, 16, 1}, lut.data(), lut.size());
}

bool SceneRenderer::LoadResources(Vfs& vfs) {
    vfs_ = &vfs;
    auto load = [&](const char* path, gpu::ImageResource& target, bool rgba_only) {
        FtexTexture ftex;
        if (!LoadFtex(vfs.Textures(), path, ftex) || ftex.mips.empty() || ftex.mips[0].empty()) {
            LogWarn("scene renderer: resource {} missing", path);
            return false;
        }
        const gpu::Format format = ftex.Format();
        if (rgba_only && format != gpu::Format::B8G8R8A8Unorm && format != gpu::Format::B8G8R8A8Srgb) {
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
    {
        FtexTexture ftex;
        const char* path = "/Assets/sh/effect/vfx_pic/fx_flr23_iy_alp_clp";
        if (LoadFtex(vfs.Textures(), path, ftex) && !ftex.mips.empty() && !ftex.mips[0].empty()) {
            std::vector<std::vector<uint8_t>> mips;
            for (uint32_t level = 0; level < ftex.mip_count && !ftex.mips[level].empty(); ++level) {
                mips.push_back(ftex.mips[level]);
            }
            UploadImageMips(mask_, ftex.Format(), {ftex.width, ftex.height, 1}, mips);
            LogInfo("scene renderer: resource {} {}x{} format {} with {} levels", path, ftex.width, ftex.height, static_cast<int>(ftex.Format()),
                    mips.size());
        } else {
            LogWarn("scene renderer: resource {} missing", path);
        }
    }
    color_lut_key_.clear();
    if (extent_.width > 0) {
        gpu_->WaitIdle();
        WriteImageDescriptors();
    }
    return true;
}

bool SceneRenderer::BuildColorLut(const std::string& path, const ScreenSettings& screen, std::vector<uint8_t>& out, std::vector<float>* fine) {
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
                    const float graded = Hermite(std::clamp(v[c], 0.0f, 1.0f), screen.start_slope, screen.end_slope);
                    p[c] = ToByte(c < 3 ? graded + screen.black_lift[c] * (1.0f - graded) : graded);
                }
            }
        }
    }
    if (fine) {
        fine->assign(size_t{64} * 64 * 64 * 4, 0.0f);
        for (int g = 0; g < 64; ++g) {
            for (int b = 0; b < 64; ++b) {
                for (int r = 0; r < 64; ++r) {
                    const glm::vec4 v = sample(glm::vec3(r, g, b) * (15.0f / 63.0f) * screen.color_scale);
                    float* p = &(*fine)[(static_cast<size_t>(g) * 4096 + b * 64 + r) * 4];
                    for (int c = 0; c < 4; ++c) {
                        p[c] = Hermite(std::clamp(v[c], 0.0f, 1.0f), screen.start_slope, screen.end_slope);
                    }
                }
            }
        }
    }
    return !filter.empty();
}

void SceneRenderer::UpdateColorLut(const ScreenSettings& screen) {
    const glm::vec3 lift = hdr_post_ ? glm::vec3(0.0f) : screen.black_lift;
    const std::string key = std::format("{}|{:.4f}|{:.4f}|{:.4f}|{:.4f}|{:.4f}|{:.4f}|{:.4f}|{:.4f}", screen.lut_path, screen.color_scale.x, screen.color_scale.y,
                                        screen.color_scale.z, screen.start_slope, screen.end_slope, lift.x, lift.y, lift.z);
    if (color_lut_fine_ != hdr_post_) {
        color_lut_key_.clear();
        color_lut_fine_ = hdr_post_;
    } else if (key == color_lut_key_) {
        return;
    }
    std::vector<uint8_t> lut;
    std::vector<float> fine;
    BuildColorLut(screen.lut_path, screen, lut, hdr_post_ ? &fine : nullptr);
    gpu_->WaitIdle();
    std::swap(color_lut_, color_lut_prev_);
    const auto upload = [&](gpu::ImageResource& image) {
        if (hdr_post_) {
            UploadImage(image, gpu::Format::R32G32B32A32Sfloat, {4096, 64, 1}, fine.data(), fine.size() * sizeof(float));
        } else {
            UploadImage(image, gpu::Format::R8G8B8A8Unorm, {256, 16, 1}, lut.data(), lut.size());
        }
    };
    upload(color_lut_);
    if (color_lut_key_.empty()) {
        upload(color_lut_prev_);
    }
    color_lut_prev_key_ = color_lut_key_;
    color_lut_key_ = key;
    if (extent_.width > 0) {
        WriteImageDescriptors();
    }
}

std::unique_ptr<GpuMesh> SceneRenderer::Upload(const MeshData& data) {
    const vk::AllocTag tag("meshes");
    auto mesh = std::make_unique<GpuMesh>();
    mesh->name = data.name;
    mesh->submeshes = data.submeshes;
    mesh->groups = data.groups;
    mesh->bounds_min = data.bounds_min;
    mesh->bounds_max = data.bounds_max;
    for (const SubMesh& sub : data.submeshes) {
        mesh->skinned = mesh->skinned || sub.skinned;
    }
    const uint64_t vertex_bytes = data.vertices.size() * sizeof(Vertex);
    const uint64_t index_bytes = data.indices.size() * sizeof(uint32_t);
    if (vertex_bytes == 0 || index_bytes == 0) {
        return nullptr;
    }
    const gpu::BufferUsageFlags rt_usage =
        gpu_->Info().ray_query ? gpu::BufferUsage::ShaderDeviceAddress | gpu::BufferUsage::AccelerationStructureBuildInput : gpu::BufferUsageFlags{};
    if (!MakeBuffer(mesh->vertices, vertex_bytes, gpu::BufferUsage::VertexBuffer | gpu::BufferUsage::TransferDst | rt_usage, false) ||
        !MakeBuffer(mesh->indices, index_bytes, gpu::BufferUsage::IndexBuffer | gpu::BufferUsage::TransferDst | rt_usage, false)) {
        gpu_->DestroyBuffer(mesh->vertices);
        gpu_->DestroyBuffer(mesh->indices);
        return nullptr;
    }
    if (!gpu::UploadBuffer(*gpu_, mesh->vertices, data.vertices.data(), vertex_bytes) ||
        !gpu::UploadBuffer(*gpu_, mesh->indices, data.indices.data(), index_bytes)) {
        gpu_->DestroyBuffer(mesh->vertices);
        gpu_->DestroyBuffer(mesh->indices);
        return nullptr;
    }
    return mesh;
}

void SceneRenderer::Destroy(GpuMesh& mesh) {
    gpu_->WaitIdle();
    if (rt_) {
        rt_->Forget(mesh);
    }
    gpu_->DestroyBuffer(mesh.vertices);
    gpu_->DestroyBuffer(mesh.indices);
}

vk::Context& SceneRenderer::Context() {
    return renderer_->Context();
}

bool SceneRenderer::RayTracingSupported(std::string& reason) const {
    if (gpu_ && gpu_->Info().backend != gpu::Backend::Vulkan) {
        if (!gpu_->Info().ray_query) {
            reason = "no DirectX Raytracing 1.1 on this GPU";
            return false;
        }
        if (!D3D12RayTracingAvailable()) {
            reason = "ray tracing is not available on DirectX 12 in this build";
            return false;
        }
        reason.clear();
        return true;
    }
    const vk::Context& ctx = renderer_->Context();
    if (!ctx.ray_query_supported) {
        reason = ctx.ray_query_missing.empty() ? std::string("no Vulkan ray queries") : ctx.ray_query_missing + " missing";
        return false;
    }
    reason.clear();
    return true;
}

float SceneRenderer::ExposureCompensationFor(float ev, const ExposureSettings& s) const {
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
    return comp;
}

float SceneRenderer::ExposureFor(float ev, const ExposureSettings& s) const {
    return std::exp2(ev + s.compensation + ExposureCompensationFor(ev, s));
}

void SceneRenderer::ReadTimestamps(FrameSlot& slot) {
    uint64_t stamps[kTimestamps] = {};
    if (!gpu_->ReadTimestamps(slot.queries, 0, kTimestamps, stamps)) {
        return;
    }
    const double period = gpu_->Info().timestamp_period_ns * 1.0e-6;
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
    stats_.light_ms[0] = span(3, 12);
    stats_.light_ms[1] = span(12, 13);
    stats_.light_ms[2] = std::max(0.0f, span(13, 14) - span(17, 20));
    stats_.light_ms[3] = span(14, 15);
    stats_.light_ms[4] = span(15, 16);
    stats_.light_ms[5] = span(16, 4);
    stats_.gpu_ms = span(0, 6);
    for (uint32_t i = 0; i < 3; ++i) {
        stats_.rt_ms[i] = span(17 + i, 18 + i);
    }
    stats_.rt_ms[3] = span(21, 22) + span(23, 24);
    stats_.pt_ms[0] = span(18, 25);
    stats_.pt_ms[1] = span(25, 26);
    stats_.pt_ms[2] = span(26, 27);
    stats_.pt_ms[3] = span(27, 28);
}

float SceneRenderer::MeterLuminance(const glm::vec2* texels, uint32_t count) {
    constexpr double kCap = 32.0;
    double all = 0.0;
    double covered = 0.0;
    uint32_t n_covered = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const float x = texels[i].x;
        const double l = std::isfinite(x) ? std::min(std::log(static_cast<double>(std::max(x, 0.0f)) + 2.0), kCap) : std::log(2.0);
        all += l;
        if (texels[i].y > 0.5f) {
            covered += l;
            ++n_covered;
        }
    }
    if (count == 0 || n_covered == 0) {
        return -1.0f;
    }
    const double mean = n_covered < count ? covered / n_covered : all / count;
    const float measured = static_cast<float>(std::exp(mean) - 2.0);
    return std::isfinite(measured) ? std::max(measured, 0.0f) : -1.0f;
}

float SceneRenderer::Meter(const glm::vec2* texels, uint32_t count) const {
    return hdr_post_ ? render::MeterWeighted(texels, count) : MeterLuminance(texels, count);
}

void SceneRenderer::ReadMeasurements(FrameSlot& slot, float dt, const ExposureSettings& settings) {
    const bool advance_exposure = std::isfinite(dt) && dt > 0.0f;
    constexpr double kExposureFrame = 1.0 / 30.0;
    if (advance_exposure) {
        exposure_time_ += dt;
    }
    const double elapsed = exposure_time_ - exposure_updated_at_;
    const bool exposure_tick = advance_exposure && elapsed + 1.0e-7 >= kExposureFrame;
    if (exposure_tick) {
        exposure_feedback_time_ = exposure_tick_times_[exposure_tick_index_];
        exposure_tick_times_[exposure_tick_index_] = exposure_time_;
        exposure_tick_index_ = (exposure_tick_index_ + 1) % exposure_tick_times_.size();
        exposure_updated_at_ += std::floor((elapsed + 1.0e-7) / kExposureFrame) * kExposureFrame;
        exposure_camera_dt_ = exposure_camera_dt_ * (59.0 / 60.0) +
                              static_cast<double>(exposure_previous_dt_ * (1.0f / 60.0f));
        if (!exposure_clock_started_) {
            const uint64_t wrapped_delta = uint64_t{0} - uint64_t{1};
            exposure_previous_dt_ = static_cast<float>(static_cast<double>(wrapped_delta) * 1.0e-6);
            exposure_clock_started_ = true;
        } else {
            exposure_previous_dt_ = std::max(dt, static_cast<float>(kExposureFrame));
        }
    }
    if (slot.timed) {
        ReadTimestamps(slot);
    }
    if (slot.light_timed && !slot.light_names.empty()) {
        static std::map<std::string, std::pair<double, uint32_t>> sums;
        static uint32_t frames = 0;
        const uint32_t n = static_cast<uint32_t>(slot.light_names.size());
        std::vector<uint64_t> stamps(2 * n);
        if (gpu_->ReadTimestamps(slot.light_queries, 0, 2 * n, stamps.data())) {
            const double period = gpu_->Info().timestamp_period_ns * 1.0e-6;
            for (uint32_t i = 0; i < n; ++i) {
                auto& [ms, count] = sums[slot.light_names[i]];
                ms += stamps[2 * i + 1] >= stamps[2 * i] ? static_cast<double>(stamps[2 * i + 1] - stamps[2 * i]) * period : 0.0;
                ++count;
            }
            if (++frames % 240 == 0) {
                std::vector<std::pair<double, std::string>> order;
                double total = 0.0;
                for (const auto& [name, v] : sums) {
                    order.emplace_back(v.first / frames, std::format("{} ({} frames)", name, v.second));
                    total += v.first / frames;
                }
                std::sort(order.rbegin(), order.rend());
                std::string text;
                for (size_t i = 0; i < std::min<size_t>(order.size(), 24); ++i) text += std::format("; {:.3f} {}", order[i].first, order[i].second);
                LogInfo("light timing: {:.3f} ms a frame over {} lights{}", total, sums.size(), text);
                sums.clear();
                frames = 0;
            }
        }
        slot.light_timed = false;
    }
    if (slot.reflection_sampled) {
        gpu_->InvalidateMapped(slot.luminance, 0, ~0ull);
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
    gpu_->InvalidateMapped(slot.luminance, 0, ~0ull);
    const float measured = Meter(static_cast<const glm::vec2*>(slot.luminance.mapped), slot.luminance_groups);
    slot.measured = false;
    if (measured < 0.0f) {
        return;
    }
    stats_.luminance = measured;
    if (!advance_exposure) {
        return;
    }
    if (!adaptation_valid_ || !toggles.adaptation || !lighting_ || !lighting_->valid) {
        exposure_samples_.clear();
        return;
    }
    if (hdr_post_) {
        ev_ = std::clamp(ev_, settings.min_ev, settings.max_ev);
        const float raw = measured / std::max(slot.exposure_used, 1.0e-12f);
        if (raw > 0.0f && std::isfinite(raw)) {
            const float target = std::clamp(std::log2(settings.key / raw), settings.min_ev, settings.max_ev);
            ev_ = render::AdaptEv(ev_, target, dt, settings.speed);
            if (static const bool ev_trace = std::getenv("PT_EV_TRACE") != nullptr; ev_trace) {
                LogInfo("ev trace: measured {:.5f} raw {:.5f} target {:.3f} ev {:.3f} dt {:.4f}", measured, raw, target, ev_, dt);
            }
        }
        return;
    }
    exposure_samples_.push_back({slot.exposure_sample_time, measured});
    while (exposure_samples_.size() > 1 && exposure_samples_[1].time <= exposure_feedback_time_ + 1.0e-7) {
        exposure_samples_.pop_front();
    }
    if (!exposure_tick) {
        return;
    }
    const float feedback = exposure_samples_.front().luminance;
    ev_ = std::clamp(ev_, settings.min_ev, settings.max_ev);
    const float adapted = feedback * std::exp2(-(settings.compensation + ExposureCompensationFor(ev_, settings)));
    const float target = std::clamp(ev_ + 0.5f * (std::log2(settings.key + 0.001f) - std::log2(adapted + 0.001f)), settings.min_ev, settings.max_ev);
    const float diff = target - ev_;
    float step = diff * (1.0f - std::pow(0.98f, 60.0f * static_cast<float>(exposure_camera_dt_))) * settings.speed * std::min(1.0f, std::abs(diff));
    if (std::abs(step) > std::abs(diff)) {
        step = diff;
    }
    if (std::isfinite(step)) {
        ev_ += step;
    }
    if (static const bool ev_trace = std::getenv("PT_EV_TRACE") != nullptr; ev_trace) {
        LogInfo("ev trace: measured {:.5f} adapted {:.5f} target {:.3f} ev {:.3f} dt {:.4f} camera_dt {:.6f} time {:.6f} sample_time {:.6f}", feedback, adapted, target, ev_, dt, exposure_camera_dt_, exposure_time_, exposure_samples_.front().time);
    }
}

namespace {

uint32_t DumpTexelBytes(gpu::Format format) {
    const uint32_t bytes = gpu::Describe(format).bytes;
    return bytes ? bytes : 4;
}

}

void SceneRenderer::RecordDumpCopy(gpu::CommandList& cmd, RenderTarget& target, gpu::BufferResource& buffer) {
    const gpu::Extent3D e = target.image.extent;
    const uint64_t size = uint64_t(e.width) * e.height * DumpTexelBytes(target.image.format);
    if (buffer.size < size) {
        gpu_->DestroyBuffer(buffer);
        if (!MakeBuffer(buffer, size, gpu::BufferUsage::TransferDst, true)) {
            return;
        }
    }
    const gpu::Layout back = target.layout;
    cmd.UseTargets({{&target, gpu::Layout::TransferSrc}});
    gpu::BufferImageCopy region;
    region.image = {target.aspect, 0, 0, 1};
    region.extent = {e.width, e.height, 1};
    cmd.CopyImageToBuffer(target.image, gpu::Layout::TransferSrc, buffer, {&region, 1});
    cmd.UseTargets({{&target, back}});
}

void SceneRenderer::DumpDraws(const std::vector<DrawItem>& items, const std::string& path) {
    gpu_->WaitIdle();
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::vector<const GpuMesh*> meshes;
    std::vector<std::pair<const GpuMesh*, uint32_t>> ids;
    auto find = [&](const GpuMesh* m) -> int64_t {
        for (const auto& [mesh, id] : ids) {
            if (mesh == m) return id;
        }
        return -1;
    };
    for (const DrawItem& d : items) {
        if (!d.mesh || find(d.mesh) >= 0) continue;
        ids.emplace_back(d.mesh, static_cast<uint32_t>(meshes.size()));
        meshes.push_back(d.mesh);
    }
    const uint32_t count = static_cast<uint32_t>(meshes.size());
    std::fwrite(&count, 4, 1, f);
    for (const GpuMesh* m : meshes) {
        gpu::BufferResource vb, ib;
        const bool ok = m->vertices.Valid() && m->indices.Valid() && MakeBuffer(vb, m->vertices.size, gpu::BufferUsage::TransferDst, true) && MakeBuffer(ib, m->indices.size, gpu::BufferUsage::TransferDst, true);
        if (ok) {
            gpu_->Submit([&](gpu::CommandList& cmd) {
                cmd.CopyBuffer(m->vertices, 0, vb, 0, m->vertices.size);
                cmd.CopyBuffer(m->indices, 0, ib, 0, m->indices.size);
            });
            gpu_->InvalidateMapped(vb, 0, ~0ull);
            gpu_->InvalidateMapped(ib, 0, ~0ull);
        }
        const uint32_t name_length = static_cast<uint32_t>(m->name.size());
        const uint32_t nv = ok ? static_cast<uint32_t>(m->vertices.size / sizeof(Vertex)) : 0u;
        const uint32_t ni = ok ? static_cast<uint32_t>(m->indices.size / 4) : 0u;
        const uint32_t ns = static_cast<uint32_t>(m->submeshes.size());
        std::fwrite(&name_length, 4, 1, f);
        std::fwrite(m->name.data(), 1, name_length, f);
        std::fwrite(&nv, 4, 1, f);
        std::fwrite(&ni, 4, 1, f);
        std::fwrite(&ns, 4, 1, f);
        if (ok) {
            const Vertex* v = static_cast<const Vertex*>(vb.mapped);
            std::vector<float> packed(size_t(nv) * 5);
            for (uint32_t i = 0; i < nv; ++i) {
                packed[size_t(i) * 5 + 0] = v[i].position.x;
                packed[size_t(i) * 5 + 1] = v[i].position.y;
                packed[size_t(i) * 5 + 2] = v[i].position.z;
                packed[size_t(i) * 5 + 3] = v[i].uv0.x;
                packed[size_t(i) * 5 + 4] = v[i].uv0.y;
            }
            std::fwrite(packed.data(), 4, packed.size(), f);
            std::fwrite(ib.mapped, 4, ni, f);
        }
        for (const SubMesh& s : m->submeshes) {
            const int32_t row[8] = {static_cast<int32_t>(s.first_index), static_cast<int32_t>(s.index_count), s.vertex_offset, static_cast<int32_t>(s.material), static_cast<int32_t>(s.pass),
                                    s.double_sided ? 1 : 0, s.skinned ? 1 : 0, static_cast<int32_t>(s.group)};
            std::fwrite(row, 4, 8, f);
        }
        if (vb.Valid()) gpu_->DestroyBuffer(vb);
        if (ib.Valid()) gpu_->DestroyBuffer(ib);
    }
    uint32_t drawn = 0;
    for (const DrawItem& d : items) drawn += d.mesh ? 1u : 0u;
    std::fwrite(&drawn, 4, 1, f);
    for (const DrawItem& d : items) {
        if (!d.mesh) continue;
        const uint32_t id = static_cast<uint32_t>(find(d.mesh));
        std::fwrite(&id, 4, 1, f);
        std::fwrite(&d.transform[0][0], 4, 16, f);
        std::fwrite(&d.source, 8, 1, f);
        const int32_t row[4] = {d.material_override, d.skin.empty() ? 0 : 1, d.view_attached ? 1 : 0, d.hidden_views};
        std::fwrite(row, 4, 4, f);
        uint8_t hidden[32] = {};
        for (size_t i = 0; i < kMaxHiddenMeshes; ++i) {
            if (d.hidden_meshes[i]) hidden[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
        }
        std::fwrite(hidden, 1, 32, f);
        std::fwrite(&d.tint[0], 4, 4, f);
    }
    std::fclose(f);
    LogInfo("draw dump: {} meshes, {} draws in {}", count, drawn, path);
}

bool SceneRenderer::DumpTargets(const std::string& prefix) {
    dump_requested_ = false;
    gpu_->WaitIdle();
    const char* selected_names = std::getenv("PT_TARGET_DUMP_NAMES");
    auto selected = [&](std::string_view name) {
        if (!selected_names || !*selected_names) {
            return true;
        }
        std::string_view remaining(selected_names);
        while (!remaining.empty()) {
            const size_t comma = remaining.find(',');
            if (remaining.substr(0, comma) == name) {
                return true;
            }
            if (comma == std::string_view::npos) {
                break;
            }
            remaining.remove_prefix(comma + 1);
        }
        return false;
    };
    std::ofstream index(prefix + ".targets.txt");
    auto write = [&](const char* name, const void* data, gpu::Extent3D e, gpu::Format format) {
        const size_t bytes = size_t(e.width) * e.height * DumpTexelBytes(format);
        std::ofstream out(prefix + "." + name + ".bin", std::ios::binary);
        out.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
        index << name << " " << e.width << " " << e.height << " " << static_cast<int>(format) << "\n";
    };
    RenderTarget* targets[] = {&albedo_, &normal_, &material_, &depth_, &diffuse_, &specular_, &reactive_, &motion_, &ao_[1], &particles_,
                               &particles_near_, &particles_far_, &near_far_depth_, &near_far_depth_attachment_, &refmap_, &reflect_layer_, &hdr_, &flare_,
                               &fsr_rr_input_[0], &fsr_rr_input_[1], &fsr_rr_input_[2], &fsr_rr_input_[3],
                               &fsr_rr_depth_, &fsr_rr_motion_, &fsr_rr_normal_, &rr_normal_roughness_, &rr_hit_distance_};
    const char* names[] = {"albedo", "normal", "material", "depth", "diffuse", "specular", "reactive", "motion", "occlusion", "particles",
                           "particles_near", "particles_far", "near_far_depth", "near_far_depth_attachment", "refmap", "reflection", "hdr", "flare",
                           "fsr_rr_direct_diffuse", "fsr_rr_direct_specular", "fsr_rr_indirect_diffuse", "fsr_rr_indirect_specular",
                           "fsr_rr_depth", "fsr_rr_motion", "fsr_rr_normal", "rr_normal_roughness", "rr_hit_distance"};
    for (size_t i = 0; i < std::size(targets); ++i) {
        RenderTarget& target = *targets[i];
        if (!target.Valid() || !selected(names[i])) {
            continue;
        }
        gpu::BufferResource buffer;
        const gpu::Extent3D e = target.image.extent;
        if (!MakeBuffer(buffer, uint64_t(e.width) * e.height * DumpTexelBytes(target.image.format), gpu::BufferUsage::TransferDst,
                              true)) {
            continue;
        }
        gpu_->Submit([&](gpu::CommandList& cmd) { RecordDumpCopy(cmd, target, buffer); });
        gpu_->InvalidateMapped(buffer, 0, ~0ull);
        write(names[i], buffer.mapped, e, target.image.format);
        gpu_->DestroyBuffer(buffer);
    }
    static const char* const kFullNames[RayTracing::kFullImages] = {
        "rt_unshadowed", "rt_shadowed", "rt_ratio0", "rt_ratio1", "rt_depth0", "rt_depth1", "rt_ratio_spatial", "rt_gi_raw", "rt_gi_history0",
        "rt_gi_history1", "rt_gi", "rt_gi_pong", "rt_glossy_raw", "rt_glossy_history0", "rt_glossy_history1", "rt_glossy_ping", "rt_glossy"};
    for (uint32_t i = 0; i < RayTracing::kFullImages; ++i) {
        RenderTarget& target = rt_full_images_[i];
        if (!target.Valid() || !selected(kFullNames[i])) {
            continue;
        }
        gpu::BufferResource buffer;
        const gpu::Extent3D e = target.image.extent;
        if (!MakeBuffer(buffer, uint64_t(e.width) * e.height * DumpTexelBytes(target.image.format), gpu::BufferUsage::TransferDst,
                              true)) {
            continue;
        }
        gpu_->Submit([&](gpu::CommandList& cmd) { RecordDumpCopy(cmd, target, buffer); });
        gpu_->InvalidateMapped(buffer, 0, ~0ull);
        write(kFullNames[i], buffer.mapped, e, target.image.format);
        gpu_->DestroyBuffer(buffer);
    }
    static const char* const kPathNames32[RayTracing::kPathImages32] = {"pt_reservoir", "pt_reservoir_history0", "pt_reservoir_history1",
                                                                        "pt_reference"};
    static const char* const kPathNames16[RayTracing::kPathImages16] = {"pt_spec_unshadowed", "pt_diffuse_moments0", "pt_diffuse_moments1",
                                                                        "pt_specular_moments0", "pt_specular_moments1", "pt_variance_ping",
                                                                        "pt_variance_pong", "pt_layer_geometry", "pt_layer_raw", "pt_layer_history0",
                                                                        "pt_layer_history1"};
    for (uint32_t i = 0; i < RayTracing::kPathImages32 + RayTracing::kPathImages16; ++i) {
        const bool wide = i < RayTracing::kPathImages32;
        RenderTarget& target = wide ? pt_images32_[i] : pt_images16_[i - RayTracing::kPathImages32];
        if (!target.Valid() || !selected(wide ? kPathNames32[i] : kPathNames16[i - RayTracing::kPathImages32])) {
            continue;
        }
        gpu::BufferResource buffer;
        const gpu::Extent3D e = target.image.extent;
        if (!MakeBuffer(buffer, uint64_t(e.width) * e.height * DumpTexelBytes(target.image.format), gpu::BufferUsage::TransferDst,
                              true)) {
            continue;
        }
        gpu_->Submit([&](gpu::CommandList& cmd) { RecordDumpCopy(cmd, target, buffer); });
        gpu_->InvalidateMapped(buffer, 0, ~0ull);
        write(wide ? kPathNames32[i] : kPathNames16[i - RayTracing::kPathImages32], buffer.mapped, e, target.image.format);
        gpu_->DestroyBuffer(buffer);
    }
    if (rt_ && RtFullAny() && light_count_ > 0) {
        const gpu::BufferResource& state = rt_->LightState(renderer_->FrameIndex());
        gpu::BufferResource buffer;
        const uint64_t bytes = sizeof(glm::vec4) * light_count_;
        if (state.buffer && MakeBuffer(buffer, bytes, gpu::BufferUsage::TransferDst, true)) {
            gpu_->Submit([&](gpu::CommandList& cmd) {
                cmd.CopyBuffer(state, 0, buffer, 0, bytes);
            });
            gpu_->InvalidateMapped(buffer, 0, ~0ull);
            const glm::vec4* values = static_cast<const glm::vec4*>(buffer.mapped);
            std::ofstream lights(prefix + ".rt_lights.txt");
            for (uint32_t i = 0; i < light_count_ && i < light_sources_.size(); ++i) {
                const gpu::Light& g = frame_->lights[i];
                lights << std::format("{} back {:.3f} blocked {:.3f} light {:.5f} factor {:.3f} strength {:.3f} bounce {:.2f} rays {} cast {} at {:.2f} {:.2f} {:.2f} {} "
                                      "axis {:.2f} {:.2f} {:.2f} inner {:.3f} cone {:.3f}\n",
                                      light_sources_[i]->name.empty() ? "(effect)" : light_sources_[i]->name, values[i].x, values[i].y,
                                      values[i].z, values[i].w, g.shadow_cone.z, g.shadow_cone.w, g.info.x >= 0 ? (g.info.x >> 16) & 0xFF : 0,
                                      light_sources_[i]->cast_shadow ? 1 : 0, g.position.x, g.position.y, g.position.z,
                                      g.position.w > 0.5f ? "spot" : "point", g.direction.x, g.direction.y, g.direction.z, g.range.y, g.cone.x);
                lights << std::format("gpu {} info {} {} {} {}", i, g.info.x, g.info.y, g.info.z, g.info.w);
                const auto dump_vector = [&](const char* name, const glm::vec4& v) {
                    lights << std::format(" {} {:.9g} {:.9g} {:.9g} {:.9g}", name, v.x, v.y, v.z, v.w);
                };
                dump_vector("position", g.position);
                dump_vector("color", g.color);
                dump_vector("range", g.range);
                dump_vector("scales", g.scales);
                dump_vector("direction", g.direction);
                dump_vector("cone", g.cone);
                dump_vector("shadow_cone", g.shadow_cone);
                for (int column = 0; column < 4; ++column) {
                    dump_vector("area_column", g.area[column]);
                    dump_vector("mask_column", g.mask[column]);
                }
                lights << '\n';
            }
            gpu_->DestroyBuffer(buffer);
        }
    }
    if (dump_recorded_ && hdr_.Valid()) {
        const std::pair<const char*, gpu::BufferResource*> copies[] = {{"compose", &dump_compose_}, {"forward", &dump_forward_}, {"scene", &dump_scene_}};
        for (const auto& [name, buffer] : copies) {
            if (buffer->mapped && selected(name)) {
                gpu_->InvalidateMapped(*buffer, 0, ~0ull);
                write(name, buffer->mapped, hdr_.image.extent, hdr_.image.format);
            }
        }
    }
    if (dump_mirror_.mapped && mirror_.Valid() && selected("mirror")) {
        gpu_->InvalidateMapped(dump_mirror_, 0, ~0ull);
        write("mirror", dump_mirror_.mapped, mirror_.image.extent, mirror_.image.format);
        const gpu::Extent3D e = mirror_.image.extent;
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
