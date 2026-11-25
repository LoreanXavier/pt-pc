#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <limits>
#include <string>

#include "engine/core/log.h"
#include "engine/fs/vfs.h"
#include "engine/render/pipeline_cache_store.h"
#include "engine/render/point_shadow_projection.h"
#include "engine/render/scene_renderer.h"
#include "engine/render/hdr_output.h"
#include "engine/vfx/vfx_system.h"

namespace pt {
namespace {

constexpr uint32_t kShadowAtlasSize = SceneRenderer::kShadowAtlasSize;
constexpr uint32_t kShadowTile = SceneRenderer::kShadowTile;
constexpr uint32_t kShadowTiles = kShadowAtlasSize / kShadowTile;

glm::mat4 ReverseZPerspective(float fov_y, float aspect, float near_plane) {
    const float f = 1.0f / std::tan(fov_y * 0.5f);
    glm::mat4 p(0.0f);
    p[0][0] = f / aspect;
    p[1][1] = -f;
    p[2][3] = -1.0f;
    p[3][2] = near_plane;
    return p;
}

glm::mat4 SpotShadowProjection(float fov_y, float near_plane, float far_plane) {
    glm::mat4 p = ReverseZPerspective(fov_y, 1.0f, near_plane);
    p[2][2] = near_plane / (far_plane - near_plane);
    p[3][2] = near_plane * far_plane / (far_plane - near_plane);
    return p;
}

glm::mat4 FoxView(const glm::mat4& gl_view) {
    return glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, -1.0f)) * gl_view;
}

bool MirrorLightSkips(const GpuMesh& mesh, int sub) {
    static const bool all_casters = std::getenv("PT_MIRROR_CASTERS_ALL") != nullptr;
    for (const char* name : {"coc0_main0_def", "bab0_main0_def", "shsb_clck001.fmdl", "shsb_lght005.fmdl", "handy_lens"}) {
        if (!all_casters && mesh.name.find(name) != std::string::npos) {
            return true;
        }
    }
    if (mesh.name.find("shsb_bath001.fmdl") == std::string::npos) {
        return false;
    }
    static const std::string list = [] {
        const char* v = std::getenv("PT_MIRROR_SKIP_SUB");
        return "," + std::string(v ? v : "2,10") + ",";
    }();
    return list.find("," + std::to_string(sub) + ",") != std::string::npos;
}

/* The original uses no polygon offset; its spot shadows move the receiver along the view's z by viewBias instead (0xDC07F0). */
bool ShadowLegacy() {
    static const bool legacy = [] {
        const char* s = std::getenv("PT_SHADOW_LEGACY");
        return s && std::string_view(s) == "1";
    }();
    return legacy;
}

void ExtractPlanes(const glm::mat4& vp, glm::vec4 planes[6]) {
    const glm::mat4 m = glm::transpose(vp);
    planes[0] = m[3] + m[0];
    planes[1] = m[3] - m[0];
    planes[2] = m[3] + m[1];
    planes[3] = m[3] - m[1];
    planes[4] = m[3] - m[2];
    planes[5] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    for (int i = 0; i < 5; ++i) {
        const float len = glm::length(glm::vec3(planes[i]));
        if (len > 0.0f) {
            planes[i] /= len;
        }
    }
}

struct OrientedBox {
    glm::vec3 center{0.0f};
    glm::vec3 axes[3] = {glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f)};
    glm::vec3 half{0.0f};
};

OrientedBox BoxFromMatrix(const glm::mat4& box) {
    OrientedBox o;
    o.center = glm::vec3(box[3]);
    for (int i = 0; i < 3; ++i) {
        const glm::vec3 c(box[i]);
        o.half[i] = glm::length(c);
        o.axes[i] = o.half[i] > 1.0e-12f ? c / o.half[i] : o.axes[i];
    }
    return o;
}

bool BoxesOverlap(const OrientedBox& a, const OrientedBox& b) {
    float r[3][3];
    float abs_r[3][3];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r[i][j] = glm::dot(a.axes[i], b.axes[j]);
            abs_r[i][j] = std::abs(r[i][j]) + 1.0e-6f;
        }
    }
    const glm::vec3 d = b.center - a.center;
    const float t[3] = {glm::dot(d, a.axes[0]), glm::dot(d, a.axes[1]), glm::dot(d, a.axes[2])};
    for (int i = 0; i < 3; ++i) {
        if (std::abs(t[i]) > a.half[i] + b.half[0] * abs_r[i][0] + b.half[1] * abs_r[i][1] + b.half[2] * abs_r[i][2]) {
            return false;
        }
    }
    for (int j = 0; j < 3; ++j) {
        if (std::abs(t[0] * r[0][j] + t[1] * r[1][j] + t[2] * r[2][j]) >
            a.half[0] * abs_r[0][j] + a.half[1] * abs_r[1][j] + a.half[2] * abs_r[2][j] + b.half[j]) {
            return false;
        }
    }
    for (int i = 0; i < 3; ++i) {
        const int i1 = (i + 1) % 3;
        const int i2 = (i + 2) % 3;
        for (int j = 0; j < 3; ++j) {
            const int j1 = (j + 1) % 3;
            const int j2 = (j + 2) % 3;
            const float ra = a.half[i1] * abs_r[i2][j] + a.half[i2] * abs_r[i1][j];
            const float rb = b.half[j1] * abs_r[i][j2] + b.half[j2] * abs_r[i][j1];
            if (std::abs(t[i2] * r[i1][j] - t[i1] * r[i2][j]) > ra + rb) {
                return false;
            }
        }
    }
    return true;
}

bool PointInBox(const glm::vec3& p, const OrientedBox& b) {
    const glm::vec3 d = p - b.center;
    float sq = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float out = std::max(std::abs(glm::dot(d, b.axes[i])) - b.half[i], 0.0f);
        sq += out * out;
    }
    return sq < 1.0e-6f;
}

glm::vec3 LightLodScales(const SceneLight& l, const glm::vec3& at, const glm::vec3& eye, const glm::vec3& forward, float p00) {
    float level = -1.0f;
    if (l.lod.x != 0.0f || l.lod.y != 0.0f) {
        level = l.cast_shadow ? 0.0f : 1.0f;
        const float depth = std::max(glm::dot(at - eye, forward), 1.0e-10f);
        const float size = 100.0f * p00 * l.lod.z / depth;
        if (size < l.lod.x) {
            const float span = l.lod.y - l.lod.x;
            const float t = (size - l.lod.x) / (std::abs(span) > 1.0e-6f ? span : -1.0e-6f);
            const float t2 = t * t;
            const float a = l.lod.w > 0.5f ? 0.1f : 1.0f / 3.0f;
            const float b = l.lod.w > 0.5f ? 0.55f : 2.0f / 3.0f;
            const float fine = t2 < a ? t2 / a : (t2 < b ? (t2 - a) / (b - a) + 1.0f : (t2 - b) / (1.0f - b) + 2.0f);
            level = std::min(std::max(fine, level), 3.0f);
        }
    }
    if (level < 0.0f) {
        return glm::vec3(1.0f, 1.0f, 1.0f);
    }
    return glm::vec3(std::clamp(2.0f - level, 0.0f, 1.0f), std::clamp(3.0f - level, 0.0f, 1.0f), std::clamp(1.0f - level, 0.0f, 1.0f));
}

glm::vec3 AnyPerpendicular(const glm::vec3& d) {
    return std::abs(d.y) < 0.95f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
}

bool BoxOccluded(const glm::vec3& lo, const glm::vec3& hi, const glm::vec3& eye, const SceneOccluder& o) {
    const glm::vec3 n = glm::cross(o.points[1] - o.points[0], o.points[2] - o.points[0]);
    float side = glm::dot(n, eye - o.points[0]);
    if (std::abs(side) < 1.0e-6f || (o.one_sided && side < 0.0f)) {
        return false;
    }
    glm::vec4 planes[8];
    planes[0] = glm::vec4(side > 0.0f ? -n : n, 0.0f);
    planes[0].w = -glm::dot(glm::vec3(planes[0]), o.points[0]);
    glm::vec3 centre(0.0f);
    for (uint32_t i = 0; i < o.count; ++i) {
        centre += o.points[i] / static_cast<float>(o.count);
    }
    for (uint32_t i = 0; i < o.count; ++i) {
        glm::vec3 m = glm::cross(o.points[i] - eye, o.points[(i + 1) % o.count] - eye);
        m = glm::dot(m, centre - eye) < 0.0f ? -m : m;
        planes[i + 1] = glm::vec4(m, -glm::dot(m, eye));
    }
    for (uint32_t i = 0; i <= o.count; ++i) {
        const glm::vec3 p = glm::vec3(planes[i]);
        const glm::vec3 worst(p.x > 0.0f ? lo.x : hi.x, p.y > 0.0f ? lo.y : hi.y, p.z > 0.0f ? lo.z : hi.z);
        if (glm::dot(p, worst) + planes[i].w < 0.0f) {
            return false;
        }
    }
    return true;
}

bool SphereInPlanes(const glm::vec3& c, float r, const glm::vec4* planes) {
    for (int i = 0; i < 5; ++i) {
        if (glm::dot(glm::vec3(planes[i]), c) + planes[i].w < -r) {
            return false;
        }
    }
    return true;
}

bool BoxInPlanes(const glm::vec3& c, const glm::vec3& h, const glm::vec4* planes) {
    for (int i = 0; i < 4; ++i) {
        const glm::vec3 n = glm::vec3(planes[i]);
        if (glm::dot(n, c) + planes[i].w < -glm::dot(glm::abs(n), h)) {
            return false;
        }
    }
    return true;
}

constexpr float kPtFittingReach = 0.3f;

constexpr float kReflectionLightReach = 40.0f;

LightBox LightGridBox(const SceneLight& l) {
    LightBox box{l.position - glm::vec3(l.outer_range), l.position + glm::vec3(l.outer_range)};
    if (l.type != LightType::Spot) {
        return box;
    }
    const float u = std::clamp(std::acos(std::clamp(l.cos_outer, -1.0f, 1.0f)), 1.0e-10f, glm::half_pi<float>());
    const float r = l.outer_range / std::cos(u * 0.5f);
    const glm::vec3 z = glm::normalize(l.direction);
    const glm::vec3 y = glm::normalize(l.up);
    const glm::vec3 x = glm::cross(y, z);
    box = {l.position, l.position};
    const auto add = [&](const glm::vec3& p) {
        box.lo = glm::min(box.lo, p);
        box.hi = glm::max(box.hi, p);
    };
    add(l.position + r * z);
    constexpr float t = 0.41421356f;
    constexpr glm::vec2 kCorners[8] = {{1.0f, t}, {t, 1.0f}, {-t, 1.0f}, {-1.0f, t}, {-1.0f, -t}, {-t, -1.0f}, {t, -1.0f}, {1.0f, -t}};
    for (const glm::vec2& c : kCorners) {
        add(l.position + r * std::sin(u) * (c.x * x + c.y * y) + r * std::cos(u) * z);
    }
    if (l.has_area) {
        const glm::mat3 m(l.area_world);
        const glm::vec3 half = glm::abs(m[0]) + glm::abs(m[1]) + glm::abs(m[2]);
        box.lo = glm::max(box.lo, glm::vec3(l.area_world[3]) - half);
        box.hi = glm::min(box.hi, glm::vec3(l.area_world[3]) + half);
    }
    return box;
}

float LightScore(const SceneLight& l, const glm::vec3& center, float radius, float& cone) {
    const glm::vec3 to = center - l.position;
    const float dist = glm::length(to);
    cone = 1.0f;
    if (l.type == LightType::Spot) {
        const float cos_a = dist > 1.0e-6f ? glm::dot(glm::normalize(l.direction), to / dist) : 1.0f;
        cone = std::pow(std::clamp((cos_a - l.cos_outer) * l.inv_cone_range, 0.0f, 1.0f), 2.7182817f);
    }
    const float d = std::max(dist, l.inner_range);
    const float reach = l.outer_range + radius;
    const float falloff = 1.0f / std::max(d * d, 1.0e-12f) - d * d / std::max(reach * reach * reach * reach, 1.0e-12f);
    return glm::length(l.intensity) * cone * falloff;
}

}

static uint32_t TargetTexelBytes(gpu::Format format) {
    switch (format) {
    case gpu::Format::R16G16B16A16Sfloat:
        return 8;
    case gpu::Format::R32G32B32A32Sfloat:
        return 16;
    default:
        return 4;
    }
}

static float HalfToFloat(uint16_t bits) {    const uint32_t sign = static_cast<uint32_t>(bits & 0x8000u) << 16;
    const uint32_t exponent = (bits >> 10) & 0x1Fu;
    const uint32_t mantissa = bits & 0x3FFu;
    uint32_t value = 0;
    if (exponent == 0) {
        if (mantissa != 0) {
            uint32_t m = mantissa;
            uint32_t e = 113;
            while ((m & 0x400u) == 0) {
                m <<= 1;
                --e;
            }
            value = sign | (e << 23) | ((m & 0x3FFu) << 13);
        } else {
            value = sign;
        }
    } else if (exponent == 0x1Fu) {
        value = sign | 0x7F800000u | (mantissa << 13);
    } else {
        value = sign | ((exponent + 112u) << 23) | (mantissa << 13);
    }
    float out = 0.0f;
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

bool SceneRenderer::Visible(const Draw& draw, const glm::vec4* planes) const {
    return !planes || SphereInPlanes(draw.center, draw.radius, planes);
}

bool SceneRenderer::Visible(const Draw& draw, const ViewSetup& view) const {
    if ((draw.hidden_views & view.view_bit) != 0 || !Visible(draw, view.planes)) return false;
    if (mesh_occlusion_ && view.view_bit == 1 && !main_occluders_.volumes.empty() && draw.mesh) {
        glm::vec3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
        for (int c = 0; c < 8; ++c) {
            const glm::vec3 corner((c & 1) ? draw.mesh->bounds_max.x : draw.mesh->bounds_min.x, (c & 2) ? draw.mesh->bounds_max.y : draw.mesh->bounds_min.y,
                                   (c & 4) ? draw.mesh->bounds_max.z : draw.mesh->bounds_min.z);
            const glm::vec3 w = glm::vec3(draw.transform * glm::vec4(corner, 1.0f));
            lo = glm::min(lo, w);
            hi = glm::max(hi, w);
        }
        if (lightcull::FindOccludingVolume(lo, hi, main_occluders_) >= 0) return false;
    }
    return true;
}

void SceneRenderer::PrepareFrame(const Camera& camera, const std::vector<DrawItem>& items, const SceneLighting& lighting) {
    camera_ = camera;
    static const char* mesh_occlusion_env = std::getenv("PT_MESH_OCCLUSION");
    mesh_occlusion_ = mesh_occlusion_env ? std::string_view(mesh_occlusion_env) == "1" : lighting.valid && lighting.screen.mesh_occlusion;
    view_count_ = 0;
    light_count_ = 0;
    probe_count_ = 0;
    draws_.clear();
    skin_matrices_.clear();
    light_sources_.clear();
    light_boxes_.clear();
    const float aspect = static_cast<float>(output_extent_.width) / static_cast<float>(output_extent_.height);
    const glm::mat4 unjittered = camera.Projection(aspect);
    glm::mat4 projection = unjittered;
    if (up_.enabled) {
        projection[2][0] -= up_.jitter_ndc.x;
        projection[2][1] -= up_.jitter_ndc.y;
    }
    static const bool date_resolve_off = [] {
        const char* v = std::getenv("PT_DATE_RESOLVE");
        return v && std::string_view(v) == "0";
    }();
    const bool date_was_active = date_active_;
    date_active_ = date_resolve_on_ && !date_resolve_off && vr_eye_ < 0 && date_resolve_ && date_resolve_out_ &&
                   mb_blur_[0].Valid() && mb_blur_[1].Valid() && !(toggles.motion_blur && lighting.screen.motion_blur && lighting.valid);
    if (!date_active_ || !date_was_active) {
        date_history_valid_ = false;
    }
    date_jitter_ndc_ = glm::vec2(0.0f);
    if (date_active_) {
        if (!up_.enabled) {
            const glm::vec2 pixels = JitterOffset(static_cast<int32_t>(frame_counter_ % 16u), 16);
            date_jitter_ndc_ = 2.0f * pixels / glm::vec2(static_cast<float>(extent_.width), static_cast<float>(extent_.height));
            projection[2][0] -= date_jitter_ndc_.x;
            projection[2][1] -= date_jitter_ndc_.y;
        }
        const glm::mat4 view_projection = unjittered * camera.View();
        date_still_now_ = date_still_ && date_history_valid_ && view_projection == date_previous_view_projection_;
        date_previous_view_projection_ = view_projection;
    }
    const glm::mat4 gl_view = camera.View();
    const float time = static_cast<float>(frame_counter_) / 60.0f;

    auto add_view = [&](const glm::mat4& proj, const glm::mat4& gview, const glm::vec3& eye, const glm::vec4& clip) {
        gpu::View& v = frame_->views[view_count_];
        v.view_projection = proj * gview;
        v.view = FoxView(gview);
        v.inv_view = glm::inverse(v.view);
        v.projection_param = glm::vec4(1.0f / proj[0][0], 1.0f / proj[1][1], camera.near_plane, 0.0f);
        v.exposure = glm::vec4(exposure_, 1.0f / std::max(exposure_, 1.0e-12f), ev_, time);
        v.viewport = glm::vec4(extent_.width, extent_.height, 1.0f / extent_.width, 1.0f / extent_.height);
        v.eye = glm::vec4(eye, 0.0f);
        v.clip_plane = clip;
        v.shadow = glm::vec4(0.0f);
        static const float shadow_rotate = std::getenv("PT_SHADOW_ROTATE") ? 1.0f : 0.0f;
        v.jitter = up_.enabled ? glm::vec4(up_.jitter_ndc, up_.mip_bias, shadow_rotate) : glm::vec4(0.0f);
        static const glm::vec2 shader_jitter = [] {
            glm::vec2 s(1.0f);
            if (const char* e = std::getenv("PT_JITTER_SHADER_SCALE")) std::sscanf(e, "%f %f", &s.x, &s.y);
            return s;
        }();
        v.jitter.x *= shader_jitter.x;
        v.jitter.y *= shader_jitter.y;
        if (date_active_ && !up_.enabled) {
            v.jitter = glm::vec4(date_jitter_ndc_, 0.0f, 0.0f);
        }
        v.jitter.z += graphics.texture_detail==0?1.0f:graphics.texture_detail==2?-.5f:0.0f;
        v.temporal = glm::vec4(0.0f);
        v.dominant_light = glm::vec4(glm::mat3(v.view) * glm::vec3(0.0f, 1.0f, 0.0f), 1.0f);
        return view_count_++;
    };
    main_view_ = ViewSetup{};
    main_view_.index = add_view(projection, gl_view, camera.position, glm::vec4(0.0f));
    main_view_.view = gl_view;
    main_view_.projection = projection;
    main_view_.eye = camera.position;
    ExtractPlanes(frame_->views[main_view_.index].view_projection, main_view_.planes);
    const glm::vec4 dominant_light(glm::mat3(frame_->views[main_view_.index].view) * glm::vec3(dominant_.out), dominant_.out.w);
    frame_->views[main_view_.index].dominant_light = dominant_light;
    if (static const bool log = std::getenv("PT_DOMINANT_LOG") != nullptr; log) {
        LogInfo("dominant light: view space ({:.4f} {:.4f} {:.4f} {:.4f})", dominant_light.x, dominant_light.y, dominant_light.z, dominant_light.w);
    }
    static const bool fixed_dither = std::getenv("PT_UPSCALE_FIXED_DITHER") != nullptr;
    if ((up_.enabled || date_active_) && !fixed_dither) {
        static constexpr glm::vec2 kTaps[4] = {{1.0f, 1.0f}, {-1.0f, -1.0f}, {1.0f, -1.0f}, {-1.0f, 1.0f}};
        frame_->views[main_view_.index].temporal =
            glm::vec4(static_cast<float>((frame_counter_ * 39u) & 63u), kTaps[frame_counter_ & 3u], 1.0f);
    }
    motion_view_ = add_view(projection, gl_view, camera.position, glm::vec4(0.0f));
    frame_->views[motion_view_].dominant_light = dominant_light;
    const glm::mat4 current_motion = unjittered * gl_view;
    frame_->views[motion_view_].view_projection = graphics.camera_motion_blur ? previous_view_projection_ : current_motion;
    frame_->views[motion_view_].inv_view = current_motion;
    post_view_ = main_view_.index;
    if (up_.enabled) {
        post_view_ = add_view(unjittered, gl_view, camera.position, glm::vec4(0.0f));
        gpu::View& post = frame_->views[post_view_];
        post.viewport = glm::vec4(output_extent_.width, output_extent_.height, 1.0f / output_extent_.width, 1.0f / output_extent_.height);
        post.jitter = glm::vec4(0.0f);
        post.dominant_light = dominant_light;
    }

    DrawPairKeys pair_keys;
    for (const DrawItem& item : items) {
        if (!item.mesh) {
            continue;
        }
        const GpuMesh& mesh = *item.mesh;
        const glm::vec3 local_center = (mesh.bounds_min + mesh.bounds_max) * 0.5f;
        float local_radius = glm::length(mesh.bounds_max - mesh.bounds_min) * 0.5f;
        uint32_t skin_base = gpu::kInvalid;
        if (!item.skin.empty() && mesh.skinned && skin_matrices_.size() + item.skin.size() <= gpu::kMaxSkinMatrices) {
            skin_base = static_cast<uint32_t>(skin_matrices_.size());
            skin_matrices_.insert(skin_matrices_.end(), item.skin.begin(), item.skin.end());
            local_radius = local_radius * 1.5f + 0.5f;
        }
        const uint64_t key = pair_keys.Next(item);
        MotionHistory& next = motion_history_next_[key];
        next.transform = item.transform;
        next.skin.assign(item.skin.begin(), item.skin.end());
        uint32_t previous_model = gpu::kInvalid;
        uint32_t previous_skin = gpu::kInvalid;
        if (auto it = motion_history_.find(key); motion_history_valid_ && it != motion_history_.end()) {
            const MotionHistory& h = it->second;
            const bool skinned = skin_base != gpu::kInvalid && h.skin.size() == item.skin.size();
            const bool posed = skinned && std::memcmp(h.skin.data(), item.skin.data(), h.skin.size() * sizeof(glm::mat4)) != 0;
            const bool moved = h.transform != item.transform && glm::distance(glm::vec3(h.transform[3]), glm::vec3(item.transform[3])) < 1.0f;
            const size_t need = 1 + (posed ? h.skin.size() : 0);
            if ((posed || moved) && skin_matrices_.size() + need <= gpu::kMaxSkinMatrices) {
                previous_model = static_cast<uint32_t>(skin_matrices_.size());
                skin_matrices_.push_back(h.transform);
                if (posed) {
                    previous_skin = static_cast<uint32_t>(skin_matrices_.size());
                    skin_matrices_.insert(skin_matrices_.end(), h.skin.begin(), h.skin.end());
                } else if (skinned) {
                    previous_skin = skin_base;
                }
            }
        }
        const float scale = std::max({glm::length(glm::vec3(item.transform[0])), glm::length(glm::vec3(item.transform[1])),
                                      glm::length(glm::vec3(item.transform[2]))});
        const glm::vec3 center = glm::vec3(item.transform * glm::vec4(local_center, 1.0f));
        const bool masked = item.hidden_meshes.any();
        if (static const char* exclude = std::getenv("PT_DRAW_EXCLUDE"); exclude && mesh.name.find(exclude) != std::string::npos) {
            continue;
        }
        for (size_t i = 0; i < mesh.submeshes.size(); ++i) {
            const SubMesh& sub = mesh.submeshes[i];
            if (masked && i < kMaxHiddenMeshes && item.hidden_meshes[i]) {
                continue;
            }
            static const std::vector<std::pair<std::string, size_t>> exclude_meshes = [] {
                std::vector<std::pair<std::string, size_t>> out;
                if (const char* v = std::getenv("PT_DRAW_EXCLUDE_MESH")) {
                    std::string s(v);
                    size_t start = 0;
                    while (start < s.size()) {
                        const size_t end = std::min(s.find(',', start), s.size());
                        const std::string part = s.substr(start, end - start);
                        const size_t colon = part.rfind(':');
                        if (colon != std::string::npos) {
                            out.emplace_back(part.substr(0, colon), static_cast<size_t>(std::strtoul(part.c_str() + colon + 1, nullptr, 10)));
                        }
                        start = end + 1;
                    }
                }
                return out;
            }();
            bool excluded_mesh = false;
            for (const auto& [text, index] : exclude_meshes) {
                excluded_mesh = excluded_mesh || (index == i && mesh.name.find(text) != std::string::npos);
            }
            if (excluded_mesh) {
                continue;
            }
            Draw d;
            d.light_fitting = item.light_fitting;
            d.mesh = item.mesh;
            d.sub = &sub;
            d.transform = item.transform;
            if (i < 2 && mesh.name == "/Assets/sh/environ/object/shsb/house/shsb_hous001/scenes/shsb_hous001_vrtn001.fmdl") {
                if (!resumed_threshold_artifact) d.transform[3].y -= 1.0e-4f;
                static int logged_threshold = -1;
                const int state = resumed_threshold_artifact ? 1 : 0;
                if (logged_threshold != state) {
                    LogInfo("geometry: f100 threshold {}", state ? "native overlap restored after loading the f100 save" : "clean first-play floor separation (0.1 mm)");
                    logged_threshold = state;
                }
            }
            d.tint = item.tint;
            d.material = item.material_override >= 0 ? static_cast<uint32_t>(item.material_override) : sub.material;
            d.skin_base = skin_base;
            d.previous_model = previous_model;
            d.previous_skin = previous_skin;
            d.center = center;
            d.radius = local_radius * scale;
            d.hidden_views = item.hidden_views;
            d.handy_shadowless = item.handy_shadowless;
            d.view_shadowless = item.view_attached && item.handy_shadowless;
            draws_.push_back(d);
        }
    }
    motion_history_.swap(motion_history_next_);
    motion_history_next_.clear();
    if (dump_requested_) {
        for (const Draw& d : draws_) {
            if (glm::distance(d.center, camera.position) < d.radius + 0.3f) {
                LogInfo("near draw: {} submesh {} material {} pass {} kind {} hidden {:#x} centre ({:.2f} {:.2f} {:.2f}) radius {:.2f} distance {:.2f}",
                        d.mesh->name, static_cast<int>(d.sub - d.mesh->submeshes.data()), d.material, static_cast<int>(d.sub->pass), static_cast<int>(d.sub->kind),
                        d.hidden_views, d.center.x, d.center.y, d.center.z, d.radius, glm::distance(d.center, camera.position));
            }
        }
    }
    if (lighting.mirror_capture) {
        for (Draw& d : draws_) {
            for (const SceneMirror& mirror : lighting.mirrors) {
                if (d.mesh == mirror.mesh && d.transform == mirror.transform) {
                    d.hidden_views |= 2u;
                }
            }
        }
    }

    mirror_cache_index_ = lighting.mirror_high ? 1u : 0u;
    mirror_stale_ = mirror_stale_cache_[mirror_cache_index_];
    mirror_active_ = false;
    mirror_stale_active_ = false;
    static const bool mirror_temporal_off = std::getenv("PT_MIRROR_TEMPORAL_OFF") != nullptr;
    mirror_temporal_enabled_ = up_.enabled && !mirror_temporal_off;
    mirrors_.clear();
    static const bool trace_active = std::getenv("PT_MIRROR_TRACE") != nullptr;
    if (trace_active) {
        static bool last_active = false;
        static size_t last_count = 0;
        if (lighting.mirror_capture != last_active || lighting.mirrors.size() != last_count) {
            last_active = lighting.mirror_capture;
            last_count = lighting.mirrors.size();
            LogInfo("mirror trace: capture wanted {} mirrors {} toggles.mirrors {} cam ({:.3f} {:.3f} {:.3f})", lighting.mirror_capture,
                    lighting.mirrors.size(), toggles.mirrors, camera.position.x, camera.position.y, camera.position.z);
        }
    }
    if (toggles.mirrors && lighting.mirror_capture && !rt_path_) {
        const SceneMirror* nearest = nullptr;
        float nearest_distance = 0.0f;
        for (const SceneMirror& mirror : lighting.mirrors) {
            const float distance = glm::length(glm::vec3(mirror.transform[3]) - camera.position);
            if (mirror.mesh && (!nearest || distance < nearest_distance)) {
                nearest = &mirror;
                nearest_distance = distance;
            }
        }
        for (const SceneMirror& mirror : lighting.mirrors) {
            if (&mirror != nearest) {
                continue;
            }
            const glm::vec3 extent = mirror.mesh->bounds_max - mirror.mesh->bounds_min;
            int axis = 0;
            if (extent.y < extent[axis]) {
                axis = 1;
            }
            if (extent.z < extent[axis]) {
                axis = 2;
            }
            glm::vec3 local_normal(0.0f);
            local_normal[axis] = 1.0f;
            const glm::vec3 local_center = (mirror.mesh->bounds_min + mirror.mesh->bounds_max) * 0.5f;
            const glm::vec3 center = glm::vec3(mirror.transform * glm::vec4(local_center, 1.0f));
            glm::vec3 normal = glm::normalize(glm::mat3(glm::transpose(glm::inverse(mirror.transform))) * local_normal);
            if (glm::dot(normal, camera.position - center) < 0.0f) {
                normal = -normal;
            }
            const float distance = glm::length(camera.position - center);
            const float radius = glm::length(extent) * 0.5f;
            const float facing = glm::dot(normal, camera.position - center);
            static const bool trace_mirror = std::getenv("PT_MIRROR_TRACE") != nullptr;
            if (trace_mirror) {
                LogInfo("mirror trace: candidate at ({:.3f} {:.3f} {:.3f}) normal ({:.3f} {:.3f} {:.3f}) distance {:.3f} radius {:.3f} facing {:.4f} "
                        "in_planes {} capture {}",
                        center.x, center.y, center.z, normal.x, normal.y, normal.z, distance, radius, facing,
                        SphereInPlanes(center, radius, main_view_.planes), distance <= 12.0f && SphereInPlanes(center, radius, main_view_.planes) &&
                                                                          facing >= 0.01f);
            }
            if (distance > 12.0f || !SphereInPlanes(center, radius, main_view_.planes) || facing < 0.01f) {
                continue;
            }
            const glm::vec3 origin = glm::vec3(mirror.transform[3]);
            const float side = glm::dot(normal, camera.position - origin);
            glm::vec3 eye = camera.position - 2.0f * side * normal;
            float eye_distance = glm::length(eye - origin);
            const float clamp_distance = lighting.mirror_high ? 5.5f : 0.5f;
            if (eye_distance > clamp_distance) {
                eye = origin + (eye - origin) * (clamp_distance / eye_distance);
                eye_distance = clamp_distance;
            }
            float corner_radius = 0.0f;
            const int ua = (axis + 1) % 3;
            const int va = (axis + 2) % 3;
            for (int corner = 0; corner < 4; ++corner) {
                glm::vec3 local = local_center;
                local[ua] = (corner & 1) ? mirror.mesh->bounds_max[ua] : mirror.mesh->bounds_min[ua];
                local[va] = (corner & 2) ? mirror.mesh->bounds_max[va] : mirror.mesh->bounds_min[va];
                corner_radius = std::max(corner_radius, glm::length(glm::vec3(mirror.transform * glm::vec4(local, 1.0f)) - center));
            }
            const glm::vec3 look = glm::normalize(origin - eye);
            const glm::vec3 up = std::abs(look.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
            const glm::mat4 capture_view = glm::lookAt(eye, origin, up);
            const float f = eye_distance / std::max(corner_radius, 1.0e-3f);
            glm::mat4 capture_projection(0.0f);
            capture_projection[0][0] = f;
            capture_projection[1][1] = -f;
            capture_projection[2][3] = -1.0f;
            capture_projection[3][2] = eye_distance;
            const uint32_t size = std::min({lighting.mirror_high ? 512u : 128u, extent_.width, extent_.height});
            mirror_current_view_projection_ = capture_projection * capture_view;
            mirror_stale_ = MirrorStale{true, mirror.mesh, mirror.transform, capture_projection, capture_view, eye, eye_distance, size};
            mirror_stale_cache_[mirror_cache_index_] = mirror_stale_;
            glm::vec2 capture_jitter(0.0f);
            if (mirror_temporal_enabled_) {
                capture_jitter = 2.0f * up_.jitter_pixels / static_cast<float>(size);
                capture_projection[2][0] -= capture_jitter.x;
                capture_projection[2][1] -= capture_jitter.y;
            }
            mirror_view_ = ViewSetup{};
            mirror_view_.index = add_view(capture_projection, capture_view, eye, glm::vec4(0.0f));
            gpu::View& capture = frame_->views[mirror_view_.index];
            capture.viewport = glm::vec4(static_cast<float>(size), static_cast<float>(size), 1.0f / size, 1.0f / size);
            capture.jitter = glm::vec4(capture_jitter, 0.0f, 0.0f);
            if (mirror_temporal_enabled_) {
                capture.temporal = frame_->views[main_view_.index].temporal;
            }
            capture.projection_param.z = eye_distance;
            mirror_view_.area = gpu::Extent2D{size, size};
            mirror_view_.view_bit = 2;
            mirror_view_.view = capture_view;
            mirror_view_.projection = capture_projection;
            mirror_view_.eye = eye;
            frame_->mirror = glm::vec4(static_cast<float>(mirror_view_.index), static_cast<float>(size), 0.0f, 0.0f);
            ExtractPlanes(frame_->views[mirror_view_.index].view_projection, mirror_view_.planes);
            mirror_active_ = true;
            mirrors_.push_back(&mirror);
            if (trace_active) {
                LogInfo("mirror trace: capture on view {} eye ({:.3f} {:.3f} {:.3f}) distance {:.3f} corner {:.3f} size {}", mirror_view_.index, eye.x,
                        eye.y, eye.z, eye_distance, corner_radius, size);
            }
            break;
        }
    }
    if (!mirror_active_ && toggles.mirrors && mirror_stale_.valid && view_count_ < gpu::kMaxViews) {
        const uint32_t index = add_view(mirror_stale_.projection, mirror_stale_.view, mirror_stale_.eye, glm::vec4(0.0f));
        gpu::View& stale = frame_->views[index];
        const float size = static_cast<float>(mirror_stale_.size);
        stale.viewport = glm::vec4(size, size, 1.0f / size, 1.0f / size);
        stale.jitter = glm::vec4(0.0f);
        stale.projection_param.z = mirror_stale_.near_distance;
        frame_->mirror = glm::vec4(static_cast<float>(index), size, 0.0f, 0.0f);
        mirror_stale_active_ = true;
        if (trace_active) {
            static bool last_stale = false;
            if (!last_stale) {
                LogInfo("mirror trace: no capture, the mirror keeps the last one (view {} size {})", index, mirror_stale_.size);
            }
            last_stale = true;
        }
    }

    static const bool legacy_cull = [] {
        const char* s = std::getenv("PT_LIGHT_CULL_LEGACY");
        return s && std::string_view(s) == "1";
    }();
    legacy_light_cull_ = legacy_cull;
    static const char* trace_cull_env = std::getenv("PT_LIGHT_CULL_TRACE");
    static const bool trace_cull = trace_cull_env != nullptr;
    static const bool trace_cull_every = trace_cull && std::string_view(trace_cull_env) == "every";
    const bool trace_cull_frame = trace_cull && (trace_cull_every || frame_counter_ % 60 == 0);
    static const bool light_changes = std::getenv("PT_LIGHT_CHANGES") != nullptr;
    main_occluders_ = {};
    mirror_occluders_ = {};
    if (!legacy_cull) {
        const lightcull::OccluderLimits& limits = lightcull::LimitsFromEnv();
        main_cull_view_projection_ = unjittered * gl_view;
        main_occluders_ = lightcull::BuildOccluderVolumes(lighting.occluders, main_cull_view_projection_, camera.position, limits);
        if (mirror_active_) {
            mirror_occluders_ = lightcull::BuildOccluderVolumes(lighting.occluders, mirror_current_view_projection_, mirror_view_.eye, limits);
        }
        if (trace_cull_frame) {
            const auto describe = [](const lightcull::OccluderSet& set, size_t source) {
                for (const lightcull::OccluderVolume& v : set.volumes) {
                    if (v.source == source) {
                        return std::format("kept (area {:.3f} distance {:.2f} planes {})", v.area, v.distance, v.plane_count);
                    }
                }
                return std::string(lightcull::VerdictName(set.verdicts[source]));
            };
            for (size_t i = 0; i < lighting.occluders.size(); ++i) {
                const glm::vec3 q = lighting.occluders[i].points[0];
                LogInfo("light cull: occluder {} at ({:.2f} {:.2f} {:.2f}) {}{}", i, q.x, q.y, q.z, describe(main_occluders_, i),
                        mirror_active_ ? " mirror view " + describe(mirror_occluders_, i) : std::string());
            }
        }
    }
    for (const SceneLight& l : lighting.lights) {
        if (static const char* light_only = std::getenv("PT_LIGHT_ONLY"); light_only && l.name.find(light_only) == std::string::npos) {
            continue;
        }
        if (static const char* light_skip = std::getenv("PT_FRAME_LIGHT_SKIP"); light_skip && l.name.find(light_skip) != std::string::npos) {
            continue;
        }
        if (light_count_ >= gpu::kMaxLights) {
            break;
        }
        const LightBox box = LightGridBox(l);
        const bool explain = trace_cull_frame || light_changes;
        std::string drop;
        if (legacy_cull) {
            const bool in_planes = BoxInPlanes((box.lo + box.hi) * 0.5f, (box.hi - box.lo) * 0.5f, main_view_.planes);
            if (!in_planes && !mirror_active_) {
                drop = "box outside the view planes";
            } else if (!mirror_active_) {
                for (size_t oi = 0; oi < lighting.occluders.size(); ++oi) {
                    if (BoxOccluded(box.lo, box.hi, camera.position, lighting.occluders[oi])) {
                        const glm::vec3 q = lighting.occluders[oi].points[0];
                        drop = explain ? std::format("occluder {} at ({:.2f} {:.2f} {:.2f}) hides box ({:.2f} {:.2f} {:.2f})-({:.2f} {:.2f} {:.2f})", oi, q.x,
                                                     q.y, q.z, box.lo.x, box.lo.y, box.lo.z, box.hi.x, box.hi.y, box.hi.z)
                                       : std::string("an occluder hides its box");
                        break;
                    }
                }
            }
        } else {
            const auto view_drop = [&](const ViewSetup& view, const lightcull::OccluderSet& occluders, const char* name) -> std::string {
                if (!BoxInCullView(view, box)) {
                    if (explain) {
                        return std::format("{} ({:.2f} {:.2f} {:.2f})-({:.2f} {:.2f} {:.2f}) outside the {} view (type {} range {:.2f} dimmer {:.2f} cos {:.3f} dir ({:.2f} {:.2f} {:.2f}) up ({:.2f} {:.2f} {:.2f}) area {})",
                                           lightcull::GridFromEnv().valid ? "grid box" : "box", box.lo.x, box.lo.y, box.lo.z, box.hi.x, box.hi.y, box.hi.z, name,
                                           static_cast<int>(l.type), l.outer_range, l.dimmer, l.cos_outer, l.direction.x, l.direction.y, l.direction.z, l.up.x, l.up.y, l.up.z, l.has_area);
                    }
                    return std::format("{} outside the {} view", lightcull::GridFromEnv().valid ? "grid box" : "box", name);
                }
                const int hider = lightcull::FindOccludingVolume(box.lo, box.hi, occluders);
                if (hider < 0) {
                    return std::string();
                }
                if (!explain) {
                    return std::string("an occluder hides its box");
                }
                const lightcull::OccluderVolume& v = occluders.volumes[hider];
                const glm::vec3 q = lighting.occluders[v.source].points[0];
                return std::format("{} view occluder {} at ({:.2f} {:.2f} {:.2f}) (area {:.3f} distance {:.2f}) hides box ({:.2f} {:.2f} {:.2f})-({:.2f} {:.2f} "
                                   "{:.2f})",
                                   name, v.source, q.x, q.y, q.z, v.area, v.distance, box.lo.x, box.lo.y, box.lo.z, box.hi.x, box.hi.y, box.hi.z);
            };
            drop = view_drop(main_view_, main_occluders_, "camera");
            static const bool no_cull = std::getenv("PT_LIGHT_NO_VIEW_CULL") != nullptr;
            if (!drop.empty() && (no_cull || rt_reflections_)) {
                const glm::vec3 nearest = glm::clamp(camera.position, box.lo, box.hi);
                if (no_cull || glm::distance(nearest, camera.position) < kReflectionLightReach) drop.clear();
            }
            if (!drop.empty() && mirror_active_) {
                const std::string mirror_drop = view_drop(mirror_view_, mirror_occluders_, "mirror");
                drop = mirror_drop.empty() ? std::string() : drop + "; " + mirror_drop;
            }
        }
        const glm::vec3 cull_rank = l.has_rank_point ? l.rank_point : (box.lo + box.hi) * 0.5f;
        const glm::vec3 cull_lod = LightLodScales(l, cull_rank, camera.position, camera.Forward(), projection[0][0]);
        const float cull_gate = glm::pi<float>() * (l.intensity.x + l.intensity.y + l.intensity.z) * exposure_;
        if (trace_cull_frame) {
            std::string reason = drop;
            if (reason.empty() && cull_lod.y <= 0.0f) {
                reason = "lod.y is zero";
            } else if (reason.empty() && cull_gate < 0.01f) {
                reason = "intensity times exposure below 0.01";
            }
            LogInfo("light cull: {} {} pos ({:.2f} {:.2f} {:.2f}) rank ({:.2f} {:.2f} {:.2f}) dist {:.2f} lod.y {:.4f} gate {:.4f} shadow {:.3f}{}",
                    reason.empty() ? "kept" : "DROPPED", l.name, l.position.x, l.position.y, l.position.z, cull_rank.x, cull_rank.y, cull_rank.z,
                    glm::distance(camera.position, cull_rank), cull_lod.y, cull_gate, l.shadow_strength * cull_lod.z,
                    reason.empty() ? std::string() : std::format(" ({})", reason));
        }
        if (!drop.empty()) {
            if (light_changes) cull_reasons_[l.name] = drop;
            continue;
        }
        const glm::vec3& rank = cull_rank;
        const glm::vec3& lod = cull_lod;
        if (static const bool log_light_lod = std::getenv("PT_LIGHT_LOD") != nullptr; log_light_lod && frame_counter_ % 120 == 0) {
            LogInfo("light lod: {} scales ({:.4f} {:.4f} {:.4f}) shadow {:.4f} rank ({:.3f} {:.3f} {:.3f})",
                    l.name, lod.x, lod.y, lod.z, l.shadow_strength * lod.z, rank.x, rank.y, rank.z);
        }
        if (lod.y <= 0.0f) {
            if (light_changes) cull_reasons_[l.name] = "lod.y is zero";
            continue;
        }
        if (glm::pi<float>() * (l.intensity.x + l.intensity.y + l.intensity.z) * exposure_ < 0.01f) {
            if (light_changes) cull_reasons_[l.name] = "intensity times exposure below 0.01";
            continue;
        }
        gpu::Light& g = frame_->lights[light_count_++];
        g = gpu::Light{};
        const bool spot = l.type == LightType::Spot;
        g.position = glm::vec4(l.position, spot ? 1.0f : 0.0f);
        g.color = glm::vec4(l.intensity, l.source_radius);
        g.direction = glm::vec4(glm::normalize(l.direction), l.shadow_bias);
        const float outer = l.outer_range + l.dimmer;
        g.range = glm::vec4(l.dimmer, l.inner_range, 1.0f / std::max(outer * outer * outer * outer, 1.0e-12f),
                            ShadowLegacy() ? 0.0f : l.view_bias);
        g.cone = spot ? glm::vec4(l.cos_outer, l.inv_cone_range, l.cone_exponent, l.dimmer) : glm::vec4(0.0f, 0.0f, 0.0f, -1.0f);
        g.shadow_cone = glm::vec4(l.shadow_cos_outer, l.shadow_inv_cone_range, 0.0f, 0.0f);
        const SceneLight* stair_area = nullptr;
        if (rt_path_ && l.path_stair_ceiling && !l.masked) {
            for (const SceneLight& area : lighting.lights) {
                if (area.stair_area && area.has_area &&
                    (!stair_area || glm::distance(area.position, l.position) < glm::distance(stair_area->position, l.position))) {
                    stair_area = &area;
                }
            }
        }
        const bool stair_ceiling = stair_area != nullptr;
        static bool stair_ceiling_logged = false;
        if (stair_ceiling && !stair_ceiling_logged) {
            LogInfo("path lighting: chandelier stair-ceiling shadow exclusion active");
            stair_ceiling_logged = true;
        }
        const uint32_t flags = (l.cast_shadow ? gpu::kLightCastsShadow : 0u) | (l.id == kHandyLightId ? gpu::kLightNoFitting : 0u) |
                               (stair_ceiling ? gpu::kLightStairCeiling : 0u) | (l.fill ? gpu::kLightFill : 0u) |
                               (l.mask_soft ? gpu::kLightMaskSoft : 0u);
        g.scales = glm::vec4(l.specular_scale * lod.x, l.diffuse_scale * lod.y, l.shadow_strength * lod.z, static_cast<float>(flags));
        const bool own_mask = l.masked && l.mask_texture >= 0;
        g.info = glm::ivec4(-1, l.masked && !own_mask ? static_cast<int>(gpu::kResMask) : -1,
                            l.has_area ? static_cast<int>(l.area_clip_mode) : 0, own_mask ? l.mask_texture + 1 : 0);
        g.area = l.has_area ? l.area_to_box : glm::mat4(0.0f);
        if (stair_ceiling) {
            g.mask = stair_area->area_to_box;
        }
        if (l.masked) {
            const glm::vec3 dir = glm::normalize(l.direction);
            const glm::vec3 up = std::abs(glm::dot(dir, glm::normalize(l.up))) < 0.95f ? glm::normalize(l.up) : AnyPerpendicular(dir);
            glm::mat4 flip(1.0f);
            flip[0][0] = -1.0f;
            g.mask = flip * ReverseZPerspective(std::max(l.mask_fov, 0.01f), 1.0f, 0.005f) * glm::lookAt(l.position, l.position + dir, up);
        }
        light_sources_.push_back(&l);
        light_boxes_.push_back(box);
    }

    probe_order_.clear();
    for (uint32_t i = 0; i < lighting.probes.size() && probe_order_.size() < gpu::kMaxProbes; ++i) {
        probe_order_.push_back(i);
    }
    std::stable_sort(probe_order_.begin(), probe_order_.end(),
                     [&](uint32_t a, uint32_t b) { return lighting.probes[a].priority > lighting.probes[b].priority; });
    for (uint32_t index : probe_order_) {
        const SceneProbe& p = lighting.probes[index];
        gpu::Probe& g = frame_->probes[probe_count_++];
        g.box = p.world_to_box;
        g.positive = glm::vec4(p.positive_scale, p.weight);
        g.negative = glm::vec4(p.negative_scale, 0.0f);
        for (int i = 0; i < 9; ++i) {
            g.sh[i] = glm::vec4(p.sh[i], 0.0f);
        }
    }
    if (static const char* probe_at = std::getenv("PT_PROBE_AT"); probe_at) {
        glm::vec3 at(0.0f);
        glm::vec3 n(0.0f, 1.0f, 0.0f);
        std::sscanf(probe_at, "%f %f %f %f %f %f", &at.x, &at.y, &at.z, &n.x, &n.y, &n.z);
        n = glm::normalize(n);
        const glm::vec3 pn(-n.x, -n.z, n.y);
        glm::vec3 blend(0.0f);
        for (uint32_t i = 0; i < probe_count_; ++i) {
            const SceneProbe& p = lighting.probes[probe_order_[i]];
            const glm::vec3 q = glm::vec3(p.world_to_box * glm::vec4(at, 1.0f));
            const glm::vec3 f = glm::min(glm::clamp((1.0f - q) * p.positive_scale, 0.0f, 1.0f), glm::clamp((1.0f + q) * p.negative_scale, 0.0f, 1.0f));
            const float w = p.weight * f.x * f.y * f.z;
            if (w <= 0.0f) {
                continue;
            }
            const glm::vec3 e = p.sh[0] + p.sh[1] * pn.y + p.sh[2] * pn.z + p.sh[3] * pn.x + p.sh[4] * (pn.x * pn.y) + p.sh[5] * (pn.y * pn.z) +
                                p.sh[6] * (3.0f * pn.z * pn.z - 1.0f) + p.sh[7] * (pn.x * pn.z) + p.sh[8] * (pn.x * pn.x - pn.y * pn.y);
            blend = glm::max(glm::vec3(0.0f), w * e) + blend * std::max(0.0f, 1.0f - w);
            const glm::vec3 c(p.box_world[3]);
            LogInfo("probe at: #{} {} priority {} box centre ({:.2f} {:.2f} {:.2f}) q ({:.2f} {:.2f} {:.2f}) weight {:.3f} irradiance ({:.4f} {:.4f} {:.4f})",
                    i, p.name, p.priority, c.x, c.y, c.z, q.x, q.y, q.z, w, e.x, e.y, e.z);
        }
        LogInfo("probe at: blend ({:.4f} {:.4f} {:.4f})", blend.x, blend.y, blend.z);
    }
    BuildShadowViews(lighting, camera.position);
    SetupFullLights();
    if (static const bool light_changes = std::getenv("PT_LIGHT_CHANGES") != nullptr; light_changes) {
        std::map<std::string, std::pair<bool, float>> now;
        for (uint32_t i = 0; i < light_count_; ++i) {
            now[light_sources_[i]->name] = {(light_shadow_views_[i] & 1) != 0, frame_->lights[i].scales.y};
        }
        for (const auto& [name, state] : now) {
            auto before = light_change_state_.find(name);
            if (before == light_change_state_.end()) {
                LogInfo("light change: frame {} eye ({:.2f} {:.2f} {:.2f}) {} ENTERS shadow {} diffuse {:.3f}", frame_counter_, camera.position.x,
                        camera.position.y, camera.position.z, name, state.first, state.second);
            } else if (before->second.first != state.first) {
                const auto why = shadow_reasons_.find(name);
                LogInfo("light change: frame {} eye ({:.2f} {:.2f} {:.2f}) {} shadow {} -> {} diffuse {:.3f}{}", frame_counter_, camera.position.x,
                        camera.position.y, camera.position.z, name, before->second.first, state.first, state.second,
                        why != shadow_reasons_.end() ? ": " + why->second : std::string());
            } else if (std::abs(before->second.second - state.second) > 0.25f) {
                LogInfo("light change: frame {} eye ({:.2f} {:.2f} {:.2f}) {} diffuse {:.3f} -> {:.3f}", frame_counter_, camera.position.x,
                        camera.position.y, camera.position.z, name, before->second.second, state.second);
            }
        }
        for (const auto& [name, state] : light_change_state_) {
            if (!now.contains(name)) {
                const auto reason = cull_reasons_.find(name);
                LogInfo("light change: frame {} eye ({:.2f} {:.2f} {:.2f}) {} LEAVES (shadow {} diffuse {:.3f}): {}", frame_counter_, camera.position.x,
                        camera.position.y, camera.position.z, name, state.first, state.second,
                        reason != cull_reasons_.end() ? reason->second : std::string("not in the scene's lights"));
            }
        }
        light_change_state_ = std::move(now);
        cull_reasons_.clear();
    }
    stats_.draws = static_cast<uint32_t>(draws_.size());
    stats_.lights = light_count_;
    stats_.probes = probe_count_;
    stats_.shadow_views = static_cast<uint32_t>(shadow_views_.size());
}

bool SceneRenderer::BoxInCullView(const ViewSetup& view, const LightBox& box) const {
    if (const lightcull::LightGrid& grid = lightcull::GridFromEnv(); grid.valid) {
        return lightcull::GridBoxInFrustum(box.lo, box.hi, view.view_bit == 1 ? main_cull_view_projection_ : mirror_current_view_projection_, grid);
    }
    return BoxInPlanes((box.lo + box.hi) * 0.5f, (box.hi - box.lo) * 0.5f, view.planes);
}

void SceneRenderer::BuildShadowViews(const SceneLighting& lighting, const glm::vec3& eye) {
    shadow_views_.clear();
    shadow_reasons_.clear();
    light_upload_count_ = light_count_;
    light_shadow_views_.assign(light_count_, 0);
    mirror_shadow_lights_.assign(light_count_, gpu::kInvalid);
    if (!toggles.shadows) {
        return;
    }
    struct Candidate {
        uint32_t light = 0;
        float score = 0.0f;
        uint8_t priority = 0;
        uint8_t view_bit = 1;
    };
    light_shadow_views_.assign(light_count_, 0);
    mirror_shadow_lights_.assign(light_count_, gpu::kInvalid);
    light_upload_count_ = light_count_;
    std::vector<Candidate> ranked;
    const auto occluded = [&](const ViewSetup& view, const LightBox& box) {
        if (legacy_light_cull_) {
            return std::any_of(lighting_->occluders.begin(), lighting_->occluders.end(),
                               [&](const SceneOccluder& o) { return BoxOccluded(box.lo, box.hi, view.eye, o); });
        }
        return lightcull::FindOccludingVolume(box.lo, box.hi, view.view_bit == 1 ? main_occluders_ : mirror_occluders_) >= 0;
    };
    auto select = [&](const ViewSetup& view, uint32_t normal_limit, uint32_t total_limit) {
        std::vector<Candidate> order;
        for (uint32_t i = 0; i < light_count_; ++i) {
            const SceneLight& src = *light_sources_[i];
            const LightBox& box = light_boxes_[i];
            const char* why = !src.cast_shadow                                ? "no castShadow"
                              : frame_->lights[i].scales.z <= 0.0f             ? "shadow LOD scale 0"
                              : (src.hidden_views & view.view_bit) != 0        ? "hidden in the view"
                              : !(legacy_light_cull_ ? BoxInPlanes((box.lo + box.hi) * 0.5f, (box.hi - box.lo) * 0.5f, view.planes) : BoxInCullView(view, box))
                                  ? "box outside the view planes"
                              : occluded(view, box) ? "an occluder hides its box"
                                                    : nullptr;
            if (why) {
                if (view.view_bit == 1) shadow_reasons_[src.name] = why;
                continue;
            }
            const glm::vec3 d = (src.has_rank_point ? src.rank_point : (box.lo + box.hi) * 0.5f) - view.eye;
            const float factor = src.type == LightType::Spot ? 0.63661977f : 1.0f;
            float score = (src.intensity.x + src.intensity.y + src.intensity.z) * factor / std::max(glm::dot(d, d), 1.0e-12f);
            if (legacy_light_cull_ && view.view_bit == 1 && previous_shadowed_.contains(src.name)) {
                score *= 1.25f;
            }
            order.push_back({i, score, src.priority, src.id == kMirrorLightId ? uint8_t{2} : view.view_bit});
        }
        std::stable_sort(order.begin(), order.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
        std::stable_sort(order.begin(), order.end(), [](const Candidate& a, const Candidate& b) { return a.priority < b.priority; });
        if (view.view_bit == 1) ranked = order;
        std::vector<Candidate> selected;
        uint32_t normal = 0;
        for (const Candidate& c : order) {
            if (selected.size() >= total_limit) {
                if (view.view_bit == 1) shadow_reasons_[light_sources_[c.light]->name] = "over the total limit";
                continue;
            }
            if (c.priority < 0x80) {
                if (normal >= normal_limit) {
                    if (view.view_bit == 1) shadow_reasons_[light_sources_[c.light]->name] = std::format("over the normal limit (score {:.4f})", c.score);
                    continue;
                }
                ++normal;
            }
            selected.push_back(c);
        }
        return selected;
    };
    const uint32_t normal_limit = lighting.screen.wide_shadow_limit ? 10u : 3u;
    std::vector<Candidate> candidates = legacy_light_cull_ ? select(main_view_, 4, 12) : select(main_view_, normal_limit, normal_limit + 5);
    if (normal_limit != shown_shadow_limit_) {
        shown_shadow_limit_ = normal_limit;
        LogInfo("shadow selection: main view limits {} and {} ({} of {} candidates shadowed)", normal_limit, normal_limit + 5,
                candidates.size(), ranked.size());
    }
    previous_shadowed_.clear();
    for (const Candidate& c : candidates) {
        light_shadow_views_[c.light] |= 1;
        previous_shadowed_.insert(light_sources_[c.light]->name);
    }
    if (mirror_active_) {
        const auto mirror_candidates = select(mirror_view_, 2, 4);
        for (Candidate c : mirror_candidates) {
            if (light_upload_count_ >= gpu::kMaxLights) break;
            const uint32_t original = c.light;
            const uint32_t copy = light_upload_count_++;
            frame_->lights[copy] = frame_->lights[original];
            mirror_shadow_lights_[original] = copy;
            light_sources_.push_back(light_sources_[original]);
            light_boxes_.push_back(light_boxes_[original]);
            c.light = copy;
            candidates.push_back(c);
        }
    }
    if (static const bool trace_selection = std::getenv("PT_SHADOW_SELECTION") != nullptr; trace_selection) {
        static uint32_t trace_frame = 0;
        static const int trace_every = std::max(1, std::atoi(std::getenv("PT_SHADOW_SELECTION")));
        if (++trace_frame % (trace_every > 1 ? trace_every : 120) == 0) {
            LogInfo("shadow selection: eye ({:.3f} {:.3f} {:.3f}) RT {}", eye.x, eye.y, eye.z, rt_active_);
            for (const Candidate& c : ranked) {
                const bool selected = std::any_of(candidates.begin(), candidates.end(), [&](const Candidate& s) { return s.light == c.light; });
                LogInfo("shadow selection: {} priority {} score {:.5f} strength {:.3f} selected {}", light_sources_[c.light]->name,
                        c.priority, c.score, frame_->lights[c.light].scales.z, selected);
            }
        }
    }
    if (rt_active_) {
        const uint32_t samples = raytracing.soft_shadows ? (2u << std::clamp(graphics.ray_quality,0,2)) : 1u;
        for (const Candidate& c : candidates) {
            const uint32_t casters = light_sources_[c.light]->id == kMirrorLightId ? 32u : c.view_bit;
            frame_->lights[c.light].info.x = static_cast<int>(casters | (rt_cull_ << 8) | (samples << 16));
        }
        return;
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    const uint32_t shadow_tile = shadow_.Extent().width / kShadowTiles;
    const uint32_t shadow_size = shadow_.Extent().width;
    bool used[kShadowTiles][kShadowTiles] = {};
    auto allocate = [&](uint32_t width, uint32_t height, uint32_t& tx, uint32_t& ty) {
        for (uint32_t y = 0; y + height <= kShadowTiles; y += height) {
            for (uint32_t x = 0; x + width <= kShadowTiles; x += width) {
                bool free = true;
                for (uint32_t j = 0; j < height; ++j) {
                    for (uint32_t k = 0; k < width; ++k) {
                        free = free && !used[y + j][x + k];
                    }
                }
                if (free) {
                    for (uint32_t j = 0; j < height; ++j) {
                        for (uint32_t k = 0; k < width; ++k) {
                            used[y + j][x + k] = true;
                        }
                    }
                    tx = x;
                    ty = y;
                    return true;
                }
            }
        }
        return false;
    };
    for (const Candidate& c : candidates) {
        if (view_count_ + 2 > gpu::kMaxViews) {
            break;
        }
        gpu::Light& g = frame_->lights[c.light];
        const SceneLight* src = light_sources_[c.light];
        const bool spot = g.position.w > 0.5f;
        uint32_t tx = 0;
        uint32_t ty = 0;
        if (!allocate(spot ? 1u : 2u, 1u, tx, ty)) {
            continue;
        }
        const float tile_uv = static_cast<float>(shadow_tile) / static_cast<float>(shadow_size);
        const glm::vec3 pos = glm::vec3(g.position);
        if (spot) {
            const glm::vec3 dir = glm::normalize(glm::vec3(g.direction));
            const glm::vec3 up = std::abs(glm::dot(dir, glm::normalize(src->up))) < 0.95f ? glm::normalize(src->up) : AnyPerpendicular(dir);
            const float half = std::atan(std::abs(std::tan(src->shadow_fov * 0.5f)));
            const float fov = std::clamp(2.0f * half, glm::radians(1.0f), glm::radians(179.0f));
            const float near_plane = std::max(src->inner_range, 1.0e-3f);
            const float far_plane = std::max(src->outer_range, 2.0f * near_plane);
            const glm::mat4 proj = SpotShadowProjection(fov, near_plane, far_plane);
            const glm::mat4 look = glm::lookAt(pos, pos + dir, up);
            gpu::View& v = frame_->views[view_count_];
            v = gpu::View{};
            v.view_projection = proj * look;
            v.view = look;
            v.projection_param = glm::vec4(1.0f, 1.0f, near_plane, 1.0f);
            v.eye = glm::vec4(pos, 0.0f);
            ShadowView sv;
            sv.view = view_count_++;
            sv.light = c.light;
            sv.casters = c.view_bit;
            sv.rect = {{static_cast<int32_t>(tx * shadow_tile), static_cast<int32_t>(ty * shadow_tile)}, {shadow_tile, shadow_tile}};
            sv.center = pos;
            sv.radius = src->outer_range;
            ExtractPlanes(v.view_projection, sv.planes);
            sv.frustum = true;
            sv.bias_constant = ShadowLegacy() ? -1.5f : 0.0f;
            sv.bias_slope = ShadowLegacy() ? -2.0f : 0.0f;
            sv.cull = gpu::CullMode::Front;
            shadow_views_.push_back(sv);
            const float half_texel = ShadowLegacy() ? 0.0f : 1.0f / static_cast<float>(shadow_tile);
            g.shadow = glm::translate(glm::mat4(1.0f), glm::vec3(half_texel, half_texel, 0.0f)) * v.view_projection;
            g.shadow_rect = glm::vec4(tx * tile_uv, ty * tile_uv, tile_uv, tile_uv);
        } else {
            const float r = src->outer_range;
            const float k = PointShadowRange(r);
            const glm::mat4 light_space = PointShadowView(pos);
            for (int side = 0; side < 2; ++side) {
                gpu::View& v = frame_->views[view_count_];
                v = gpu::View{};
                v.view = light_space;
                v.projection_param = glm::vec4(1.0f, 1.0f, 0.05f, 2.0f);
                v.eye = glm::vec4(pos, side == 0 ? -1.0f : 1.0f);
                v.shadow = glm::vec4(1.0f / k, r - k, 0.0f, 0.0f);
                ShadowView sv;
                sv.view = view_count_++;
                sv.light = c.light;
                sv.casters = c.view_bit;
                sv.rect = {{static_cast<int32_t>((tx + side) * shadow_tile), static_cast<int32_t>(ty * shadow_tile)}, {shadow_tile, shadow_tile}};
                sv.center = pos;
                sv.radius = r;
                sv.frustum = false;
                sv.bias_constant = ShadowLegacy() ? -1.5f : 0.0f;
                sv.bias_slope = ShadowLegacy() ? -2.0f : 0.0f;
                sv.cull = PointShadowCullFront(side != 0) ? gpu::CullMode::Front : gpu::CullMode::Back;
                shadow_views_.push_back(sv);
            }
            g.shadow = light_space;
            g.cone = glm::vec4(r - k, 1.0f / std::max(r, 1.0e-4f), 1.0f / std::max(k, 1.0e-4f), -1.0f);
            g.shadow_rect = glm::vec4(tx * tile_uv, ty * tile_uv, 2.0f * tile_uv, tile_uv);
        }
        g.info.x = static_cast<int>(tx + ty * kShadowTiles);
    }
}

void SceneRenderer::SetTppFog(const TppAtmosphereSettings& tpp) {
    const float log_exposure = std::log2(std::max(exposure_, 1.0e-12f));
    float offset = tpp.exposure_offset_values[0];
    if (log_exposure <= tpp.exposure_offset_targets[0]) {
        offset = tpp.exposure_offset_values[2];
        for (int i = 0; i < 2; ++i) {
            const float a = tpp.exposure_offset_targets[i];
            const float b = tpp.exposure_offset_targets[i + 1];
            if (log_exposure <= a && log_exposure > b) {
                const float k = (log_exposure - a) / (b - a);
                offset = tpp.exposure_offset_values[i] + (tpp.exposure_offset_values[i + 1] - tpp.exposure_offset_values[i]) * k;
            }
        }
    }
    const float e = exposure_ * std::exp2(offset);
    frame_->fog[0] = glm::vec4(tpp.fog_density, tpp.fog_falloff, tpp.fog_near, std::max(tpp.fog_far, 10.0f));
    frame_->fog[1] = glm::vec4(tpp.fog_self * e, 1.0f);
    frame_->fog[2] = glm::vec4(tpp.fog_mie, tpp.fog_mie_anisotropy);
    frame_->fog[3] = glm::vec4(tpp.fog_rayleigh, e * tpp.dir_gain * glm::length(tpp.dir_color));
    frame_->fog[4] = glm::vec4(0.0f);
    frame_->fog[5] = glm::vec4(0.0f);
    frame_->fog[6] = glm::vec4(0.0f);
    frame_->fog[7] = glm::vec4(tpp.light_dir, 0.0f);
}

void SceneRenderer::UploadFrame(FrameSlot& slot) {
    uint8_t* base = static_cast<uint8_t*>(slot.frame_data.mapped);
    std::memcpy(base + offsetof(gpu::FrameData, fog), frame_->fog, sizeof(frame_->fog));
    std::memcpy(base + offsetof(gpu::FrameData, mirror), &frame_->mirror, sizeof(frame_->mirror));
    std::memcpy(base + offsetof(gpu::FrameData, display), &frame_->display, sizeof(frame_->display));
    std::memcpy(base + offsetof(gpu::FrameData, grade), &frame_->grade, sizeof(frame_->grade));
    std::memcpy(base + offsetof(gpu::FrameData, views), frame_->views, sizeof(gpu::View) * view_count_);
    std::memcpy(base + offsetof(gpu::FrameData, lights), frame_->lights, sizeof(gpu::Light) * light_upload_count_);
    std::memcpy(base + offsetof(gpu::FrameData, probes), frame_->probes, sizeof(gpu::Probe) * probe_count_);
    gpu_->FlushMapped(slot.frame_data, 0, ~0ull);
    if (!skin_matrices_.empty()) {
        std::memcpy(slot.skin.mapped, skin_matrices_.data(), sizeof(glm::mat4) * skin_matrices_.size());
        gpu_->FlushMapped(slot.skin, 0, sizeof(glm::mat4) * skin_matrices_.size());
    }
}

void SceneRenderer::BindSets(gpu::CommandList& cmd, gpu::BindPoint point) {
    const FrameSlot& slot = slots_[renderer_->FrameIndex()];
    gpu::BindingSetHandle sets[2] = {textures_->BindingSet(), post_bindings_ ? slot.post_set : slot.set};
    cmd.BindSets(point, layout_, 0, sets);
}

void SceneRenderer::DrawMesh(gpu::CommandList& cmd, const Draw& draw, uint32_t view, uint32_t flags, bool mirrored, bool no_cull,
                             const glm::vec4* aux, gpu::CullMode cull, bool shadow_pass) {
    const SubMesh& sub = *draw.sub;
    const bool double_sided = shadow_pass ? sub.shadow_double_sided : sub.double_sided;
    cmd.SetCullMode(double_sided || no_cull ? gpu::CullMode::None : cull);
    cmd.SetFrontFace(mirrored ? gpu::FrontFace::Clockwise : gpu::FrontFace::CounterClockwise);
    cmd.BindVertexBuffer(draw.mesh->vertices.buffer, 0);
    cmd.BindIndexBuffer(draw.mesh->indices.buffer, 0, gpu::IndexType::Uint32);
    gpu::DrawPush push;
    push.model = draw.transform;
    push.ids = glm::uvec4(view, draw.material, flags, sub.skinned ? draw.skin_base : gpu::kInvalid);
    push.tint = draw.tint;
    if (aux) {
        push.aux[0] = aux[0];
        push.aux[1] = aux[1];
    }
    PushConstants(cmd, &push, sizeof(push));
    cmd.DrawIndexed(sub.index_count, 1, sub.first_index, sub.vertex_offset, 0);
}

void SceneRenderer::PushConstants(gpu::CommandList& cmd, const void* data, uint32_t size) {
    cmd.PushConstants(layout_, gpu::ShaderStage::Vertex | gpu::ShaderStage::Fragment | gpu::ShaderStage::Compute, 0, size, data);
}

void SceneRenderer::Fullscreen(gpu::CommandList& cmd, gpu::PipelineHandle pipeline, const gpu::PassPush& push) {
    cmd.BindPipeline(gpu::BindPoint::Graphics, pipeline);
    PushConstants(cmd, &push, sizeof(push));
    cmd.Draw(3, 1, 0, 0);
}

void SceneRenderer::RecordRayTracing(gpu::CommandList& cmd) {
    uint64_t& built = rt_built_frame_[renderer_->FrameIndex() % Renderer::kFramesInFlight];
    auto untraced = [&] {
        if (rt_->Valid(renderer_->FrameIndex())) return;
        rt_active_ = rt_reflections_ = rt_ao_active_ = rt_contact_active_ = false;
        rt_full_shadows_ = rt_gi_active_ = rt_glossy_active_ = rt_path_ = false;
    };
    if (built == frame_counter_) {
        untraced();
        return;
    }
    built = frame_counter_;
    static const bool skinned = [] {
        const char* env = std::getenv("PT_RT_SKINNED");
        return !env || std::atoi(env) != 0;
    }();
    rt_casters_.clear();
    for (const Draw& d : draws_) {
        if (!skinned && d.sub->skinned && d.skin_base != gpu::kInvalid) {
            continue;
        }
        uint32_t mask = 0;
        if (rt_path_ && (d.hidden_views & 3u) == 1u && d.sub->kind != gpu::kKindShadowOnly &&
            ((d.sub->pass == RenderPass::Opaque && d.sub->kind == gpu::kKindDeferred) || d.sub->pass == RenderPass::Unlit ||
             d.sub->pass == RenderPass::Transparent)) {
            mask |= 2u;
        }
        if (d.sub->shadow) {
            mask |= ((d.hidden_views & 1u) == 0 ? 1u : 0u) | ((d.hidden_views & 2u) == 0 ? 2u : 0u);
            if ((d.hidden_views & 2u) == 0 && !MirrorLightSkips(*d.mesh, static_cast<int>(d.sub - d.mesh->submeshes.data()))) {
                mask |= 32u;
            }
        }
        if (static const char* exclude = std::getenv("PT_SHADOW_EXCLUDE"); exclude && d.mesh->name.find(exclude) != std::string::npos) {
            mask &= ~3u;
        }
        if (static const bool skin_shadow_only = std::getenv("PT_SHADOW_SKIN_ONLY") != nullptr; skin_shadow_only && !d.sub->skinned) {
            mask &= ~3u;
        }
        if (static const bool no_skin_shadow = std::getenv("PT_SHADOW_NO_SKIN") != nullptr; no_skin_shadow && d.sub->skinned) {
            mask &= ~3u;
        }
        const bool deferred = d.sub->pass == RenderPass::Opaque && d.sub->kind == gpu::kKindDeferred;
        const bool forward = (d.sub->pass == RenderPass::Unlit || d.sub->pass == RenderPass::Transparent) && d.sub->kind != gpu::kKindShadowOnly;
        const bool surface = (deferred || forward) && (d.hidden_views & 1u) == 0;
        static const bool layers_opaque = std::getenv("PT_RT_REFLECT_LAYERS_OPAQUE") != nullptr;
        const bool layer = forward && (d.sub->kind == gpu::kKindConstant || d.sub->kind == gpu::kKindGlass);
        const bool reflected = rt_reflections_ || rt_glossy_active_ || rt_path_;
        const bool mirror_layer = (d.hidden_views & 3u) == 1u && layer;
        if (reflected && (surface || mirror_layer) && layer && !layers_opaque) {
            mask |= 64u;
        } else if (reflected && surface) {
            mask |= 4u;
        }
        const bool see_through = d.sub->kind == gpu::kKindConstant || d.sub->kind == gpu::kKindGlass || d.sub->kind == gpu::kKindSky;
        static const bool glow_casters = [] {
            const char* value = std::getenv("PT_RT_GLOW_CASTERS");
            return !value || std::atoi(value) != 0;
        }();
        const bool glow_caster = glow_casters && d.sub->kind == gpu::kKindConstant && d.sub->shadow;
        const bool full_occluder = RtFullAny() && (deferred || (d.sub->shadow && !see_through) || glow_caster) && (d.hidden_views & 1u) == 0;
        if (deferred && (d.hidden_views & 1u) == 0 && (rt_ao_active_ || rt_gi_active_ || rt_path_)) {
            mask |= 8u;
        }
        bool ray_fitting = false;
        if ((deferred && (d.hidden_views & 1u) == 0 && rt_contact_active_) || full_occluder) {
            bool fitting = false;
            if (d.radius < 0.5f) {
                for (const SceneLight* l : light_sources_) {
                    if (glm::distance(l->position, d.center) < d.radius + 0.02f) {
                        fitting = true;
                        break;
                    }
                }
            }
            static const bool path_fittings = [] {
                const char* value = std::getenv("PT_PATH_FITTING");
                return !value || std::atoi(value) != 0;
            }();
            if (!rt_path_ && rt_full_shadows_ && full_occluder) {
                ray_fitting = fitting || d.light_fitting != 0;
            }
            if (rt_path_ && path_fittings && full_occluder) {
                ray_fitting = d.light_fitting != 0;
                if (d.radius < 0.5f) {
                    for (const SceneLight* l : light_sources_) {
                        if (glm::distance(l->position, d.center) < d.radius + kPtFittingReach) {
                            ray_fitting = true;
                            break;
                        }
                    }
                }
            }
            const bool path_layer = rt_path_ && d.sub->kind == gpu::kKindParallax;
            if ((!fitting || ray_fitting) && !path_layer && (!glow_caster || ray_fitting)) {
                mask |= (deferred && (d.hidden_views & 1u) == 0 && rt_contact_active_) ? 16u : 0u;
                mask |= full_occluder ? 128u : 0u;
            }
        }
        if (static const char* exclude = std::getenv("PT_PATH_OCCLUDER_EXCLUDE"); exclude && d.mesh->name.find(exclude) != std::string::npos) {
            mask &= ~128u;
        }
        if (mask == 0) {
            continue;
        }
        RtCaster c;
        c.mesh = d.mesh;
        c.submesh = static_cast<uint32_t>(d.sub - d.mesh->submeshes.data());
        c.transform = d.transform;
        c.material = d.material;
        c.skin_base = d.sub->skinned ? d.skin_base : gpu::kInvalid;
        c.mask = static_cast<uint8_t>(mask);
        c.alpha_test = (textures_->MaterialFlags(d.material) & gpu::kMatAlphaTest) != 0;
        c.double_sided = d.sub->shadow ? d.sub->shadow_double_sided : d.sub->double_sided;
        c.fitting = ray_fitting && (mask & 128u) != 0;
        c.mirror_hidden = (d.hidden_views & 2u) != 0;
        c.light_fitting = c.fitting ? d.light_fitting : 0;
        c.tint = glm::vec3(d.tint);
        rt_casters_.push_back(c);
    }
    if (static const bool log_fittings = std::getenv("PT_RT_FITTING_LOG") != nullptr; log_fittings && frame_counter_ % 120 == 0) {
        for (const SceneLight* l : light_sources_) {
            for (const Draw& d : draws_) {
                if (glm::distance(l->position, d.center) < d.radius + 0.6f && d.radius < 1.0f) {
                    LogInfo("ray tracing: light {} ({:.2f} {:.2f} {:.2f}) near {}#{} radius {:.2f} centre ({:.2f} {:.2f} {:.2f}) shadow {} kind {} "
                            "material flags {:#x} fitting {}",
                            l->name.empty() ? "(effect)" : l->name, l->position.x, l->position.y, l->position.z, d.mesh->name,
                            static_cast<int>(d.sub - d.mesh->submeshes.data()), d.radius, d.center.x, d.center.y, d.center.z, d.sub->shadow,
                            static_cast<int>(d.sub->kind), textures_->MaterialFlags(d.material),
                            d.radius < 0.5f && glm::distance(l->position, d.center) < d.radius + 0.02f);
                }
            }
        }
    }
    CollectEffects();
    rt_->SetEffects(rt_effect_vertices_, rt_effect_indices_, rt_path_ ? 16u : 64u);
    BindSets(cmd, gpu::BindPoint::Compute);
    if (!rt_->Build(cmd, renderer_->FrameIndex(), rt_casters_)) {
        untraced();
    }
    static const bool log_frames = std::getenv("PT_RT_LOG") != nullptr;
    if (log_frames) {
        const RtStats& s = rt_->Stats();
        std::string skinned_meshes;
        for (const RtCaster& c : rt_casters_) {
            if (c.skin_base != gpu::kInvalid) {
                skinned_meshes += std::format(" {}#{}(mask {})", c.mesh->name, c.submesh, c.mask);
            }
        }
        LogInfo("ray tracing: frame {}: {} instances, {} skinned ({} vertices), {} effect triangles, {} structures built, {} static BLAS so far;{}",
                frame_counter_, s.instances, s.skinned_instances, s.skinned_vertices, s.effect_triangles, s.built_this_frame, s.static_blas,
                skinned_meshes);
    }
}

void SceneRenderer::CollectEffects() {
    rt_effect_vertices_.clear();
    rt_effect_indices_.clear();
    static const bool enabled = [] {
        const char* value = std::getenv("PT_RT_EFFECTS");
        return !value || std::atoi(value) != 0;
    }();
    const vfx::RenderList* list = enabled && vfx_list && (rt_reflections_ || rt_glossy_active_ || rt_path_) ? vfx_list() : nullptr;
    if (!list) {
        return;
    }
    constexpr size_t kMaxQuads = 8192;
    constexpr uint32_t kSkip = vfx::kQuadLiquid | vfx::kQuadLiquidHnm | vfx::kQuadRain | vfx::kQuadScreen | vfx::kQuadFlare;
    const float exposure = std::max(exposure_, 1.0e-12f);
    const float ev = -std::log2(exposure);
    size_t quads = 0;
    for (const vfx::Draw& d : list->draws) {
        if (d.layer != vfx::Layer::World || d.refract || d.opaque || (d.blend != vfx::BlendMode::Alpha && d.blend != vfx::BlendMode::Add)) {
            continue;
        }
        for (uint32_t i = d.first; i < d.first + d.count && i < list->quads.size() && quads < kMaxQuads; ++i) {
            const vfx::Quad& q = list->quads[i];
            const uint32_t flags = q.info.y;
            if ((flags & kSkip) != 0u || q.info.x == vfx::kNoTexture || !(q.color.a > 0.0f)) {
                continue;
            }
            static const float kMinAlpha = [] { const char* v = std::getenv("PT_RT_EFFECT_MIN_ALPHA"); return v ? static_cast<float>(std::atof(v)) : 0.08f; }();
            static const float kMinArea = [] { const char* v = std::getenv("PT_RT_EFFECT_MIN_AREA"); return v ? static_cast<float>(std::atof(v)) : 2.5e-5f; }();
            if (d.blend == vfx::BlendMode::Alpha && q.color.a < kMinAlpha) {
                continue;
            }
            if (glm::length(glm::cross(glm::vec3(q.corner[1] - q.corner[0]), glm::vec3(q.corner[2] - q.corner[0]))) < kMinArea) {
                continue;
            }
            glm::vec3 rgb = glm::vec3(q.color);
            if ((flags & vfx::kQuadLit) != 0u) {
                const glm::vec3 centre = (glm::vec3(q.corner[0]) + glm::vec3(q.corner[1]) + glm::vec3(q.corner[2]) + glm::vec3(q.corner[3])) * 0.25f;
                glm::vec3 lights(0.0f);
                for (int l = 0; l < 3; ++l) {
                    const glm::vec4& c = q.light_color[l];
                    if (glm::dot(glm::vec3(c), glm::vec3(c)) < 1.0e-12f) continue;
                    const float distance = std::max(glm::length(glm::vec3(q.light_position[l]) - centre), 0.01f);
                    const float squared = distance * distance;
                    lights += glm::vec3(c) * std::max(0.0f, 1.0f / squared - squared * q.light_position[l].w);
                }
                rgb *= glm::vec3(q.light_factors) + lights * q.light_factors.w;
            }
            if ((flags & vfx::kQuadExposure) == 0u) {
                rgb /= exposure;
            }
            if ((flags & vfx::kQuadLuminance) != 0u) {
                const float t = std::clamp((ev - q.luminance.x) / std::max(q.luminance.y - q.luminance.x, 1.0e-4f), 0.0f, 1.0f);
                rgb *= std::max(q.luminance.z, 0.0f) + (std::max(q.luminance.w, 0.0f) - std::max(q.luminance.z, 0.0f)) * t;
            }
            const uint32_t mode = (d.blend == vfx::BlendMode::Add ? 1u : 0u) | ((flags & vfx::kQuadLit) != 0u ? 4u : 2u) |
                                  ((flags & vfx::kQuadClip) != 0u ? 8u : 0u);
            const bool triangle = (flags & vfx::kQuadTriangle) != 0u;
            const glm::vec3 normal = glm::normalize(glm::cross(glm::vec3(q.corner[1] - q.corner[0]), glm::vec3(q.corner[2] - q.corner[0])) +
                                                    glm::vec3(0.0f, 0.0f, 1.0e-9f));
            const uint32_t base = static_cast<uint32_t>(rt_effect_vertices_.size());
            for (uint32_t corner = 0; corner < (triangle ? 3u : 4u); ++corner) {
                Vertex v;
                v.position = glm::vec3(q.corner[corner]);
                v.normal = normal;
                v.uv0 = triangle ? (corner == 0 ? glm::vec2(q.uv.x, q.uv.y) : corner == 1 ? glm::vec2(q.uv.z, q.uv.w) : glm::vec2(q.uv_next.x, q.uv_next.y))
                                 : glm::vec2((corner & 1u) != 0u ? q.uv.z : q.uv.x, (corner & 2u) != 0u ? q.uv.w : q.uv.y);
                v.uv1 = glm::vec2(std::bit_cast<float>(q.info.x), std::bit_cast<float>(mode));
                v.color = glm::vec4(rgb, q.color.a);
                rt_effect_vertices_.push_back(v);
            }
            for (uint32_t index : {0u, 2u, 1u}) rt_effect_indices_.push_back(base + index);
            if (!triangle) {
                for (uint32_t index : {1u, 2u, 3u}) rt_effect_indices_.push_back(base + index);
            }
            ++quads;
        }
    }
    if (static const bool log_effects = std::getenv("PT_RT_LOG") != nullptr; log_effects && frame_counter_ % 120 == 0 && !rt_effect_vertices_.empty()) {
        glm::vec3 low(1.0e30f);
        glm::vec3 high(-1.0e30f);
        for (const Vertex& v : rt_effect_vertices_) {
            low = glm::min(low, v.position);
            high = glm::max(high, v.position);
        }
        LogInfo("ray tracing: frame {}: {} effect quads, {} triangles, bounds ({:.2f} {:.2f} {:.2f}) to ({:.2f} {:.2f} {:.2f})", frame_counter_, quads,
                rt_effect_indices_.size() / 3, low.x, low.y, low.z, high.x, high.y, high.z);
    }
}

void SceneRenderer::RecordShadows(gpu::CommandList& cmd) {
    if (rt_active_ || rt_reflections_ || rt_ao_active_ || rt_contact_active_ || RtFullAny()) {
        RecordRayTracing(cmd);
    }
    if (rt_active_) {
        return;
    }
    if (shadow_views_.empty()) {
        return;
    }
    uint32_t rows = 0;
    for (const ShadowView& sv : shadow_views_) {
        rows = std::max(rows, static_cast<uint32_t>(sv.rect.offset.y) + sv.rect.extent.height);
    }
    cmd.UseTargets({{&shadow_, gpu::Layout::DepthAttachment}});
    cmd.BeginPass({shadow_.Extent().width, rows}, {}, &shadow_, false, true);
    cmd.BindPipeline(gpu::BindPoint::Graphics, shadow_pipeline_);
    for (const ShadowView& sv : shadow_views_) {
        cmd.SetViewport(sv.rect);
        cmd.SetDepthBias(sv.bias_constant, 0.0f, sv.bias_slope);
        const uint8_t casters = sv.casters;
        uint32_t shadow_draws = 0, shadow_indices = 0;
        for (const Draw& d : draws_) {
            if (!d.sub->shadow || d.view_shadowless || (d.hidden_views & casters) != 0 || (d.handy_shadowless && light_sources_[sv.light]->id == kHandyLightId)) {
                continue;
            }
            if (static const char* exclude = std::getenv("PT_SHADOW_EXCLUDE"); exclude && d.mesh->name.find(exclude) != std::string::npos) {
                continue;
            }
            if (light_sources_[sv.light]->id == kMirrorLightId && MirrorLightSkips(*d.mesh, static_cast<int>(d.sub - d.mesh->submeshes.data()))) {
                continue;
            }
            if (static const bool skin_shadow_only = std::getenv("PT_SHADOW_SKIN_ONLY") != nullptr; skin_shadow_only && !d.sub->skinned) {
                continue;
            }
            if (static const bool no_skin_shadow = std::getenv("PT_SHADOW_NO_SKIN") != nullptr; no_skin_shadow && d.sub->skinned) {
                continue;
            }
            if (glm::length(d.center - sv.center) > sv.radius + d.radius) {
                continue;
            }
            if (sv.frustum && !SphereInPlanes(d.center, d.radius, sv.planes)) {
                continue;
            }
            ++shadow_draws; shadow_indices += d.sub->index_count;
            ++shadow_draws; shadow_indices += d.sub->index_count;
            if (static const bool dump = std::getenv("PT_SHADOW_PASS_DUMP") != nullptr; dump && light_sources_[sv.light]->id == kMirrorLightId) {
                LogInfo("shadow draw: {}#{} {} verts-radius {:.2f} centre ({:.2f} {:.2f} {:.2f})", d.mesh->name, static_cast<int>(d.sub - d.mesh->submeshes.data()), d.sub->index_count, d.radius, d.center.x, d.center.y, d.center.z);
            }
            DrawMesh(cmd, d, sv.view, 0, false, false, nullptr, sv.cull, true);
        }
        if (static const bool trace = std::getenv("PT_SHADOW_PASS_TRACE") != nullptr; trace) {
            const SceneLight& light = *light_sources_[sv.light];
            const glm::vec3 axis = glm::vec3(frame_->lights[sv.light].direction);
            LogInfo("shadow pass: {} casters {:#x} draws {} indices {} at ({:.6f} {:.6f} {:.6f}) axis ({:.8f} {:.8f} {:.8f}) near {:.6f} far {:.6f} fov {:.6f} bias {:.6f} view-bias {:.6f}",
                    light.name, sv.casters, shadow_draws, shadow_indices, sv.center.x, sv.center.y, sv.center.z,
                    axis.x, axis.y, axis.z, light.inner_range, light.outer_range, glm::degrees(light.shadow_fov), light.shadow_bias, light.view_bias);
        }
    }
    cmd.EndPass();
    cmd.UseTargets({{&shadow_, gpu::Layout::DepthReadOnly}});
}

void SceneRenderer::RecordGBuffer(gpu::CommandList& cmd, const ViewSetup& view) {
    cmd.UseTargets({{&albedo_, gpu::Layout::ColorAttachment},
                     {&normal_, gpu::Layout::ColorAttachment},
                     {&material_, gpu::Layout::ColorAttachment},
                     {&depth_, gpu::Layout::DepthAttachment}});
    cmd.BeginPass(ViewArea(view), {{&albedo_, true, {}}, {&normal_, true, {}}, {&material_, true, {}}}, &depth_, false, true);
    uint32_t debug_flags = 0;
    if (debug_view == DebugView::UV0) {
        debug_flags = 2;
    } else if (debug_view == DebugView::VertexColor) {
        debug_flags = 3;
    }
    if (static const bool lod = std::getenv("PT_DEBUG_LOD") != nullptr; lod) {
        debug_flags = 4;
    }
    const glm::vec4 aux[2] = {glm::vec4(0.0f), glm::vec4(0.0f, 0.0f, 0.0f, static_cast<float>(reflection_cube_))};
    for (int pass = 0; pass < 2; ++pass) {
        const bool path = rt_path_ && view.index == main_view_.index && pt_gbuffer_ && pt_decal_;
        cmd.BindPipeline(gpu::BindPoint::Graphics, pass == 0 ? (path ? pt_gbuffer_ : gbuffer_) : (path ? pt_decal_ : decal_));
        if (pass == 1) {
            static const bool legacy_bias = std::getenv("PT_DECAL_BIAS") != nullptr;
            cmd.SetDepthBias(legacy_bias ? 1.0f : 0.0f, 0.0f, legacy_bias ? 1.0f : 0.0f);
        }
        const RenderPass wanted = pass == 0 ? RenderPass::Opaque : RenderPass::Decal;
        decal_order_.clear();
        for (const Draw& d : draws_) {
            if (d.sub->pass == wanted && d.sub->kind == gpu::kKindDeferred && Visible(d, view)) {
                decal_order_.push_back(&d);
            }
        }
        static const bool by_layer = [] {
            const char* env = std::getenv("PT_DECAL_LAYERS");
            return !env || std::atoi(env) != 0;
        }();
        if (pass == 1 && by_layer) {
            std::stable_sort(decal_order_.begin(), decal_order_.end(), [](const Draw* a, const Draw* b) { return a->sub->layer < b->sub->layer; });
        }
        for (const Draw* d : decal_order_) {
            DrawMesh(cmd, *d, view.index, debug_flags, view.mirrored, false, aux);
        }
    }
    cmd.EndPass();
}

void SceneRenderer::RecordOcclusion(gpu::CommandList& cmd, const ViewSetup& view) {
    cmd.UseTargets({{&depth_, gpu::Layout::DepthReadOnly}, {&ao_[0], gpu::Layout::ColorAttachment}});
    cmd.BeginPass(extent_, {{&ao_[0], false, {}}});
    gpu::PassPush push;
    push.ids = glm::uvec4(view.index, 0, 0, 0);
    push.f0 = glm::vec4(3.0f, 1.0f, 0.0f, 0.0f);
    push.f1 = glm::vec4(1.0f, 4.0f, 1.0f, 0.25f);
    push.f2 = glm::vec4(1.0f / 5.0f, 0.85f, std::min(camera_.far_plane, 100.0f + 150.0f), 1.0f / 150.0f);
    Fullscreen(cmd, occlusion_, push);
    cmd.EndPass();
    cmd.UseTargets({{&ao_[0], gpu::Layout::ShaderReadOnly}, {&ao_[1], gpu::Layout::ColorAttachment}});
    cmd.BeginPass(extent_, {{&ao_[1], false, {}}});
    push.f0 = glm::vec4(0.45f, 0.0f, 0.0f, 0.0f);
    Fullscreen(cmd, occlusion_blur_, push);
    cmd.EndPass();
    cmd.UseTargets({{&ao_[1], gpu::Layout::ShaderReadOnly}});
}

bool SceneRenderer::EnsureAoTargets() {
    const vk::AllocTag tag("ray tracing");
    if (rt_ao_images_[0].Valid() && rt_ao_images_[0].Extent().width == extent_.width && rt_ao_images_[0].Extent().height == extent_.height) {
        return true;
    }
    if (rt_ao_failed_extent_.width == extent_.width && rt_ao_failed_extent_.height == extent_.height &&
        std::chrono::steady_clock::now() - rt_ao_failed_at_ < std::chrono::seconds(5)) {
        return false;
    }
    gpu_->WaitIdle();
    for (RenderTarget& t : rt_ao_images_) {
        gpu_->DestroyImage(t.image);
        t = RenderTarget{};
    }
    const gpu::Format formats[RayTracing::kAoImages] = {gpu::Format::R32Sfloat, gpu::Format::R16G16B16A16Sfloat, gpu::Format::R16G16B16A16Sfloat,
                                                     gpu::Format::R32Sfloat, gpu::Format::R32Sfloat};
    gpu::ImageViewHandle views[RayTracing::kAoImages]{};
    for (uint32_t i = 0; i < RayTracing::kAoImages; ++i) {
        if (!CreateTarget(rt_ao_images_[i], formats[i], extent_, gpu::ImageUsage::Storage | gpu::ImageUsage::TransferDst,
                          gpu::Aspect::Color)) {
            LogError("ray tracing: AO image creation failed at {}x{}", extent_.width, extent_.height);
            rt_ao_failed_extent_ = extent_;
            rt_ao_failed_at_ = std::chrono::steady_clock::now();
            for (RenderTarget& t : rt_ao_images_) {
                gpu_->DestroyImage(t.image);
                t = RenderTarget{};
            }
            return false;
        }
        views[i] = rt_ao_images_[i].image.view;
    }
    gpu_->Submit([&](gpu::CommandList& cmd) {
        for (uint32_t i = 0; i < RayTracing::kAoImages; ++i) {
            cmd.UseTargets({{&rt_ao_images_[i], gpu::Layout::General}});
            const gpu::ClearColor value = (i == 1 || i == 2) ? gpu::ClearColor{} : gpu::ClearColor{{1.0f, 1.0f, 1.0f, 1.0f}};
            cmd.ClearColorImage(rt_ao_images_[i].image, gpu::Layout::General, value, 1, 1);
        }
        cmd.UseTargets({{&rt_ao_images_[0], gpu::Layout::General}});
    });
    rt_->SetAoImages(views);
    rt_ao_history_ = false;
    LogInfo("ray tracing: ambient occlusion images {}x{}", extent_.width, extent_.height);
    return true;
}

void SceneRenderer::RecordRtAmbientOcclusion(gpu::CommandList& cmd, const ViewSetup& view) {
    const gpu::View& v = frame_->views[view.index];
    const bool cut = glm::distance(rt_ao_previous_eye_, view.eye) > kCameraCutDistance;
    const uint32_t previous = rt_ao_written_;
    gpu::PassPush push;
    push.ids = glm::uvec4(view.index, static_cast<uint32_t>(frame_counter_ & 0xFFFFFFu), std::getenv("PT_RT_AO_RAYS")?rt_ao_rays_:2u << std::clamp(graphics.ray_quality,0,2), 0);
    push.f0 = glm::vec4(rt_ao_reach_, rt_ao_frames_, 0.0f, 0.0f);
    push.m = rt_ao_previous_;
    BindSets(cmd, gpu::BindPoint::Compute);
    const gpu::BindingSetHandle rt_set = rt_->Set(renderer_->FrameIndex());
    cmd.BindSets(gpu::BindPoint::Compute, rt_->Layout(), 2, {&rt_set, 1});
    const uint32_t groups_x = (extent_.width + 7) / 8;
    const uint32_t groups_y = (extent_.height + 7) / 8;
    cmd.UseTargets({{&depth_, gpu::Layout::DepthReadOnly}, {&normal_, gpu::Layout::ShaderReadOnly}});
    cmd.BindPipeline(gpu::BindPoint::Compute, rt_ao_trace_);
    PushConstants(cmd, &push, sizeof(push));
    cmd.Dispatch(groups_x, groups_y, 1);
    cmd.BindPipeline(gpu::BindPoint::Compute, rt_ao_filter_);
    for (uint32_t pass = 0; pass < 3; ++pass) {
        cmd.UseTargets({{&rt_ao_images_[0], gpu::Layout::General}, {&rt_ao_images_[1], gpu::Layout::General},
                         {&rt_ao_images_[2], gpu::Layout::General}, {&rt_ao_images_[3], gpu::Layout::General}});
        push.ids.z = pass;
        push.ids.w = previous | (rt_ao_history_ && !cut ? 0u : 2u);
        PushConstants(cmd, &push, sizeof(push));
        cmd.Dispatch(groups_x, groups_y, 1);
    }
    cmd.UseTargets({{&rt_ao_images_[4], gpu::Layout::General}});
    BindSets(cmd, gpu::BindPoint::Graphics);
    rt_ao_written_ = previous ^ 1u;
    rt_ao_history_ = true;
    rt_ao_previous_ = v.view_projection;
    rt_ao_previous_eye_ = view.eye;
    rt_ao_ready_ = true;
}

glm::mat4 SceneRenderer::SpotVolume(const SceneLight& l, float radius, float& size) {
    size = 0.0f;
    if (!(l.inv_cone_range > 0.0f) || !(l.cone_exponent > 0.0f) || !(radius > 0.0f) || !std::isfinite(l.cos_outer)) {
        return glm::mat4(1.0f);
    }
    const float length_dir = glm::length(l.direction);
    if (!(length_dir > 1.0e-6f)) {
        return glm::mat4(1.0f);
    }
    const glm::vec3 axis = l.direction / length_dir;
    const glm::vec3 u = glm::normalize(glm::cross(axis, AnyPerpendicular(axis)));
    const glm::vec3 w = glm::cross(axis, u);
    constexpr float kMargin = 0.05f;
    const float c = std::clamp(l.cos_outer - 1.0e-3f, -1.0f, 1.0f);
    float z_min = -kMargin;
    float lateral = radius;
    if (c >= 0.0f) {
        lateral = std::min(radius, radius * std::sqrt(std::max(0.0f, 1.0f - c * c)) + kMargin);
    } else {
        z_min = std::max(-radius, radius * c - kMargin);
    }
    const float z_max = radius;
    const float half_z = 0.5f * (z_max - z_min);
    const glm::vec3 center = l.position + axis * (0.5f * (z_max + z_min));
    size = lateral * lateral * half_z;
    return glm::mat4(glm::vec4(u * lateral, 0.0f), glm::vec4(w * lateral, 0.0f), glm::vec4(axis * half_z, 0.0f), glm::vec4(center, 1.0f));
}

glm::vec2 SceneRenderer::LightDepthBounds(const gpu::Light& light, const gpu::View& view, const glm::mat4* volume) {
    const double outer = std::pow(1.0 / std::max(static_cast<double>(light.range.z), 1.0e-30), 0.25);
    const double reach = std::max(outer, outer - static_cast<double>(light.range.x));
    const double radius = reach * 1.02 + 0.05;
    const double center_z = static_cast<double>((view.view * glm::vec4(glm::vec3(light.position), 1.0f)).z);
    const double near_distance = static_cast<double>(view.projection_param.z);
    double far_z = center_z + radius;
    double near_z = center_z - radius;
    if (volume) {
        double lo_z = std::numeric_limits<double>::max();
        double hi_z = -std::numeric_limits<double>::max();
        for (int corner = 0; corner < 8; ++corner) {
            const glm::vec4 local((corner & 1) ? 1.0f : -1.0f, (corner & 2) ? 1.0f : -1.0f, (corner & 4) ? 1.0f : -1.0f, 1.0f);
            const double z = static_cast<double>((view.view * (*volume * local)).z);
            lo_z = std::min(lo_z, z);
            hi_z = std::max(hi_z, z);
        }
        const double margin = 0.01 + 1.0e-4 * std::max(std::abs(lo_z), std::abs(hi_z));
        if (std::isfinite(lo_z) && std::isfinite(hi_z)) {
            near_z = std::max(near_z, lo_z - margin);
            far_z = std::min(far_z, hi_z + margin);
        }
    }
    if (!(far_z > 0.0) || !(near_distance > 0.0) || !std::isfinite(far_z) || !std::isfinite(near_z) || near_z > far_z) {
        return {0.0f, 1.0f};
    }
    const double lo = near_distance / far_z * (1.0 - 1.0e-4);
    const double hi = near_z > 0.0 ? near_distance / near_z * (1.0 + 1.0e-4) : 1.0;
    return {static_cast<float>(std::clamp(lo, 0.0, 1.0)), static_cast<float>(std::clamp(hi, 0.0, 1.0))};
}

void SceneRenderer::RecordLighting(gpu::CommandList& cmd, const ViewSetup& view) {
    const gpu::CullMode cull = view.mirrored ? gpu::CullMode::Back : gpu::CullMode::Front;
    const bool path = rt_path_ && view.index == main_view_.index && lighting_->valid && rt_full_traced_;
    const bool probes = toggles.probes && probe_count_ > 0 && !path;
    const bool lights = toggles.lights && debug_view != DebugView::Ambient && !path;
    const gpu::Extent2D area = ViewArea(view);
    static const bool legacy_resolve = [] {
        const char* v = std::getenv("PT_SH_RESOLVE");
        return v && std::string_view(v) == "legacy";
    }();
    const bool ao = rt_ao_ready_ && view.index == main_view_.index;
    const bool shrink = !up_.enabled && !ao && !legacy_resolve;
    const gpu::Extent2D acc_area{shrink ? (area.width + 1) / 2 : area.width, shrink ? (area.height + 1) / 2 : area.height};
    auto draw_probes = [&](uint32_t shrink_flag) {
        if (ao) {
            cmd.BindPipeline(gpu::BindPoint::Graphics, probe_ao_);
            const gpu::BindingSetHandle rt_set = rt_->Set(renderer_->FrameIndex());
            cmd.BindSets(gpu::BindPoint::Graphics, rt_->Layout(), 2, {&rt_set, 1});
        } else {
            cmd.BindPipeline(gpu::BindPoint::Graphics, probe_);
        }
        cmd.SetCullMode(cull);
        for (uint32_t i = 0; i < probe_count_; ++i) {
            const SceneProbe& p = lighting_->probes[probe_order_[i]];
            gpu::PassPush push;
            push.ids = glm::uvec4(view.index, i, shrink_flag, 0);
            push.m = p.box_world;
            PushConstants(cmd, &push, sizeof(push));
            cmd.Draw(36, 1, 0, 0);
        }
    };
    if (probes) {
        cmd.UseTargets({{&normal_, gpu::Layout::ShaderReadOnly},
                         {&depth_, gpu::Layout::DepthReadOnly},
                         {&probe_acc_, gpu::Layout::ColorAttachment}});
        gpu::ClearColor transmittance{};
        transmittance.float32[3] = 1.0f;
        cmd.BeginPass(acc_area, {{&probe_acc_, true, transmittance}});
        draw_probes(shrink ? 1u : 0u);
        cmd.EndPass();
        cmd.UseTargets({{&probe_acc_, gpu::Layout::ShaderReadOnly}});
    }
    cmd.UseTargets({{&albedo_, gpu::Layout::ShaderReadOnly},
                     {&normal_, gpu::Layout::ShaderReadOnly},
                     {&material_, gpu::Layout::ShaderReadOnly},
                     {&depth_, gpu::Layout::DepthReadOnly},
                     {&diffuse_, gpu::Layout::ColorAttachment},
                     {&specular_, gpu::Layout::ColorAttachment}});
    cmd.BeginPass(area, {{&diffuse_, true, {}}, {&specular_, true, {}}}, &depth_, true, false);
    if (probes) {
        cmd.BindPipeline(gpu::BindPoint::Graphics, probe_resolve_);
        gpu::PassPush push;
        push.ids = glm::uvec4(legacy_resolve ? 2u : (shrink ? 1u : 0u), acc_area.width, acc_area.height, view.index);
        PushConstants(cmd, &push, sizeof(push));
        cmd.Draw(3, 1, 0, 0);
    }
    if (view.index == main_view_.index) {
        Stamp(cmd, 15);
    }
    const bool full = rt_full_shadows_ && view.index == main_view_.index && lighting_->valid && rt_full_traced_;
    if (full) {
        cmd.EndPass();
        cmd.UseTargets({{&rt_full_images_[0], gpu::Layout::ColorAttachment}, {&rt_full_images_[1], gpu::Layout::ColorAttachment}});
        cmd.BeginPass(area, {{&rt_full_images_[0], true, {}}, {&specular_, false, {}}, {&rt_full_images_[1], true, {}}}, &depth_, true, false);
    }
    const bool contact = rt_contact_active_ && view.index == main_view_.index && !full;
    FrameSlot& timing_slot = slots_[renderer_->FrameIndex()];
    const bool light_timing = timing_slot.light_queries && view.index == main_view_.index;
    if (light_timing) {
        cmd.ResetQueries(timing_slot.light_queries, 0, 2 * gpu::kMaxLights);
        timing_slot.light_names.clear();
    }
    if (lights && light_count_ > 0) {
        if (rt_active_ || contact || full) {
            cmd.BindPipeline(gpu::BindPoint::Graphics, full ? light_full_ : rt_active_ ? light_rt_ : light_contact_);
            const gpu::BindingSetHandle rt_set = rt_->Set(renderer_->FrameIndex());
            cmd.BindSets(gpu::BindPoint::Graphics, rt_->Layout(), 2, {&rt_set, 1});
        } else {
            cmd.BindPipeline(gpu::BindPoint::Graphics, light_);
        }
        cmd.SetCullMode(cull);
        static const char* const draw_only = std::getenv("PT_LIGHT_DRAW_ONLY");
        static const char* const draw_unshadowed = std::getenv("PT_LIGHT_DRAW_NO_SHADOW");
        for (uint32_t i = 0; i < light_count_; ++i) {
            const SceneLight& l = *light_sources_[i];
            if (draw_only && l.name.find(draw_only) == std::string::npos) {
                continue;
            }
            if ((l.hidden_views & view.view_bit) != 0) {
                continue;
            }
            const float radius = l.outer_range * 1.02f + 0.05f;
            glm::mat4 volume = glm::translate(glm::mat4(1.0f), l.position) * glm::scale(glm::mat4(1.0f), glm::vec3(radius));
            float volume_size = radius * radius * radius;
            const bool area_bounds = l.has_area && l.area_clip_mode == AreaClipMode::Box;
            bool drawn_area = false;
            if (l.has_area) {
                const glm::mat3 box(l.area_world);
                const glm::vec3 half(glm::length(box[0]), glm::length(box[1]), glm::length(box[2]));
                if (half.x * half.y * half.z < radius * radius * radius) {
                    volume = l.area_world;
                    volume_size = half.x * half.y * half.z;
                    drawn_area = true;
                }
            }
            static const bool spot_box = [] {
                const char* v = std::getenv("PT_LIGHT_SPOT_BOX");
                return !(v && v[0] == '0');
            }();
            static const bool alternate = [] {
                const char* v = std::getenv("PT_LIGHT_DEPTH_BOUNDS");
                return v && std::string_view(v) == "alternate";
            }();
            const bool former = alternate && (frame_counter_ & 1u);
            if (spot_box && !former && l.type == LightType::Spot && (!drawn_area || area_bounds)) {
                float size = 0.0f;
                const glm::mat4 cone_box = SpotVolume(l, radius, size);
                if (size > 0.0f && size < volume_size) {
                    volume = cone_box;
                    volume_size = size;
                    drawn_area = false;
                }
            }
            const bool volume_bounds = !drawn_area || area_bounds;
            gpu::PassPush push;
            const uint32_t mirror_light = mirror_shadow_lights_[i];
            const bool has_shadow = full ? frame_->lights[i].info.x >= 0
                                         : view.view_bit == 2 ? mirror_light != gpu::kInvalid : (light_shadow_views_[i] & view.view_bit) != 0;
            const uint32_t light_index = view.view_bit == 2 && has_shadow ? mirror_light : i;
            push.ids = glm::uvec4(view.index, light_index, toggles.shadows && has_shadow ? 1u : 0u, 0);
            if (draw_unshadowed && l.name.find(draw_unshadowed) != std::string::npos) {
                push.ids.z = 0;
            }
            push.f0.x = contact ? rt_contact_reach_ : 0.0f;
            push.f0.y = contact && rt_contact_legacy_ ? 1.0f : 0.0f;
            static const bool contact_centre = std::getenv("PT_RT_CONTACT_CENTRE") != nullptr;
            static const float contact_rays = [] {
                const char* value = std::getenv("PT_RT_CONTACT_RAYS");
                return value ? static_cast<float>(std::clamp(std::atoi(value), 1, 8)) : 2.0f;
            }();
            push.f0.z = contact && !contact_centre && rt_active_ && raytracing.soft_shadows ? contact_rays : 0.0f;
            if (full) {
                push.f0.w = 0.0015f;
            }
            push.m = volume;
            PushConstants(cmd, &push, sizeof(push));
            if (light_depth_bounds_) {
                const glm::vec2 bounds = former ? glm::vec2(0.0f, 1.0f)
                                                                              : LightDepthBounds(frame_->lights[light_index], frame_->views[view.index],
                                                                                                 volume_bounds ? &volume : nullptr);
                cmd.SetDepthBounds(bounds.x, bounds.y);
            }
            const uint32_t timed = static_cast<uint32_t>(timing_slot.light_names.size());
            if (light_timing) {
                const gpu::Light& g = frame_->lights[light_index];
                timing_slot.light_names.push_back(std::format("{} {} r {:.2f}{}{}", l.name.empty() ? "(effect)" : l.name,
                                                              l.type == LightType::Spot ? "spot" : "point", l.outer_range,
                                                              push.ids.z ? " shadow" : "", g.info.y >= 0 || g.info.w > 0 ? " mask" : ""));
                cmd.WriteTimestamp(timing_slot.light_queries, 2 * timed);
            }
            cmd.Draw(36, 1, 0, 0);
            if (light_timing) {
                cmd.WriteTimestamp(timing_slot.light_queries, 2 * timed + 1);
            }
        }
    }
    cmd.EndPass();
    if (light_timing) {
        timing_slot.light_timed = !timing_slot.light_names.empty();
    }
    if (view.index == main_view_.index) {
        Stamp(cmd, 16);
        Stamp(cmd, 21);
        if (RtFullAny() && lighting_->valid && rt_full_traced_) {
            RecordRtFullResolve(cmd, view);
        }
        Stamp(cmd, 22);
    }
    cmd.UseTargets({{&diffuse_, gpu::Layout::ShaderReadOnly}, {&specular_, gpu::Layout::ShaderReadOnly}});
}

void SceneRenderer::RecordLuminance(gpu::CommandList& cmd) {
    FrameSlot& slot = slots_[renderer_->FrameIndex()];
    const uint32_t gx = 2;
    const uint32_t gy = 2;
    cmd.BindPipeline(gpu::BindPoint::Compute, luminance_);
    BindSets(cmd, gpu::BindPoint::Compute);
    if (slot.dispatch_set) {
        const uint32_t groups[4] = {gx, gy, 1, 0};
        std::memcpy(slot.dispatch.mapped, groups, sizeof(groups));
        cmd.BindSets(gpu::BindPoint::Compute, layout_, 3, {&slot.dispatch_set, 1});
    }
    gpu::PassPush push;
    PushConstants(cmd, &push, sizeof(push));
    cmd.Dispatch(gx, gy, 1);
    cmd.GlobalBarrier(gpu::PipelineStage::ComputeShader, gpu::Bits(gpu::Access::ShaderStorageWrite), gpu::PipelineStage::Host, gpu::Bits(gpu::Access::HostRead));
    slot.luminance_groups = gpu::kMeterTexels;
}

void SceneRenderer::UpdateReflectionTexture(const std::string& path) {
    if (path == reflection_key_ || !vfs_) {
        return;
    }
    reflection_key_ = path;
    reflection_cube_ = 0;
    if (path.empty()) {
        return;
    }
    bool ok = false;
    const uint32_t index = textures_->LoadFox(vfs_->Textures(), path, &ok);
    const uint32_t slot = ok ? textures_->CubeSlot(index) : TextureManager::kNoCube;
    reflection_cube_ = slot == TextureManager::kNoCube ? 0 : slot;
    LogInfo("scene renderer: reflection texture {} {}", path, slot == TextureManager::kNoCube ? "is not a loadable cube map" : "loaded");
}

uint32_t SceneRenderer::ForwardLights(const Draw& draw, glm::vec4 aux[2], uint8_t view_bit) const {
    const glm::vec3 local_center = (draw.mesh->bounds_min + draw.mesh->bounds_max) * 0.5f;
    const glm::vec3 local_half = (draw.mesh->bounds_max - draw.mesh->bounds_min) * 0.5f;
    const glm::mat3 m(draw.transform);
    const glm::vec3 half = glm::abs(m[0]) * local_half.x + glm::abs(m[1]) * local_half.y + glm::abs(m[2]) * local_half.z;
    const glm::vec3 center = glm::vec3(draw.transform * glm::vec4(local_center, 1.0f));
    const glm::vec3 area_point = local_center + glm::vec3(draw.transform[3]);
    OrientedBox model_box;
    model_box.center = area_point;
    model_box.half = local_half;
    for (int i = 0; i < 3; ++i) {
        const float len = glm::length(m[i]);
        model_box.axes[i] = len > 1.0e-12f ? m[i] / len : model_box.axes[i];
    }
    const float radius = std::max({half.x, half.y, half.z});
    struct Candidate {
        uint32_t index = 0;
        float score = 0.0f;
        float cone = 1.0f;
    };
    std::vector<Candidate> candidates;
    for (uint32_t i = 0; i < light_count_ && i < 0xFFu; ++i) {
        const SceneLight& l = *light_sources_[i];
        const LightBox& box = light_boxes_[i];
        if ((l.hidden_views & view_bit) != 0 || glm::any(glm::greaterThan(box.lo, center + half)) ||
            glm::any(glm::lessThan(box.hi, center - half))) {
            continue;
        }
        if (l.has_area) {
            if (!BoxesOverlap(model_box, BoxFromMatrix(l.area_world))) {
                continue;
            }
        }
        float cone = 1.0f;
        const float score = LightScore(l, center, radius, cone);
        candidates.push_back({i, score, cone});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    uint32_t lights = 0xFFFFFFu;
    uint32_t cones = 0;
    for (uint32_t k = 0; k < 3 && k < candidates.size(); ++k) {
        lights = (lights & ~(0xFFu << (8 * k))) | (candidates[k].index << (8 * k));
        cones |= static_cast<uint32_t>(std::lround(candidates[k].cone * 1023.0f)) << (10 * k);
    }
    std::vector<const SceneProbe*> probes;
    for (const SceneProbe& p : lighting_->probes) {
        const glm::mat3 b(p.box_world);
        const glm::vec3 probe_half = glm::abs(b[0]) + glm::abs(b[1]) + glm::abs(b[2]);
        if (glm::all(glm::lessThanEqual(glm::abs(glm::vec3(p.box_world[3]) - center), probe_half + half))) {
            if (PointInBox(area_point, BoxFromMatrix(p.box_world))) {
                probes.push_back(&p);
            }
        }
    }
    std::stable_sort(probes.begin(), probes.end(), [](const SceneProbe* a, const SceneProbe* b) { return a->priority < b->priority; });
    float weights[3] = {0.0f, 0.0f, 0.0f};
    float left = 1.0f;
    float total = 0.0f;
    for (size_t k = 0; k < 3 && k < probes.size(); ++k) {
        const SceneProbe& p = *probes[k];
        const glm::vec3 q = glm::vec3(p.world_to_box * glm::vec4(center, 1.0f));
        const glm::vec3 f = glm::min(glm::clamp((1.0f - q) * p.positive_scale, 0.0f, 1.0f), glm::clamp((1.0f + q) * p.negative_scale, 0.0f, 1.0f));
        weights[k] = probes.size() == 1 ? 1.0f : left * p.weight * f.x * f.y * f.z;
        left -= weights[k];
        total += weights[k];
    }
    glm::vec3 sky(0.0f);
    glm::vec3 ground(0.0f);
    for (size_t k = 0; k < 3 && k < probes.size() && total >= 1.0e-5f; ++k) {
        const std::array<glm::vec3, 9>& sh = probes[k]->sh;
        sky += weights[k] / total * glm::max(sh[0] + sh[2] + 2.0f * sh[6], glm::vec3(0.0f));
        ground += weights[k] / total * glm::max(sh[0] - sh[2] + 2.0f * sh[6], glm::vec3(0.0f));
    }
    aux[0] = glm::vec4(0.5f * (sky - ground), std::bit_cast<float>(cones));
    aux[1] = glm::vec4(0.5f * (sky + ground), static_cast<float>(reflection_cube_));
    return lights;
}

void SceneRenderer::UpdateDominantLight(const Camera& camera, const SceneLighting& lighting, float dt) {
    const glm::vec3 p = camera.position + glm::vec3(0.0f, 0.0f, 2.0f);
    const glm::vec3 half = 0.9f * glm::abs(camera.Up()) + 0.25f * glm::abs(camera.Right());
    const glm::vec3 center = p + 0.25f * camera.Forward();
    const glm::vec3 lo = center - half;
    const glm::vec3 hi = center + half;
    const float radius = 0.5f * std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
    struct Candidate {
        const SceneLight* light = nullptr;
        float score = 0.0f;
        float strength = 0.0f;
    };
    std::vector<Candidate> candidates;
    for (const SceneLight& l : lighting.lights) {
        const LightBox box = LightGridBox(l);
        if (glm::any(glm::lessThan(box.hi, lo)) || glm::any(glm::greaterThan(box.lo, hi))) {
            continue;
        }
        float cone = 1.0f;
        const float score = LightScore(l, center, radius, cone);
        candidates.push_back({&l, score, glm::length(l.intensity) * cone / glm::pi<float>()});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    float best = 0.0f;
    glm::vec3 found(0.0f, -1.0f, 0.0f);
    const SceneLight* chosen = nullptr;
    for (size_t k = 0; k < 4 && k < candidates.size(); ++k) {
        const glm::vec3 to = p - candidates[k].light->position;
        if (candidates[k].strength > best && glm::dot(to, to) > 0.0f) {
            best = candidates[k].strength;
            found = glm::normalize(to);
            chosen = candidates[k].light;
        }
    }
    dominant_.Step(found, dt);
    static const bool log = std::getenv("PT_DOMINANT_LOG") != nullptr;
    if (log) {
        LogInfo("dominant light: frame {} eye ({:.3f} {:.3f} {:.3f}) found ({:.3f} {:.3f} {:.3f}) from {} ({} candidates) out ({:.3f} {:.3f} {:.3f} {:.3f})",
                frame_counter_, camera.position.x, camera.position.y, camera.position.z, found.x, found.y, found.z,
                chosen ? (chosen->name.empty() ? std::string("(effect light)") : chosen->name) : std::string("none"), candidates.size(),
                dominant_.out.x, dominant_.out.y, dominant_.out.z, dominant_.out.w);
        for (size_t k = 0; k < 6 && k < candidates.size(); ++k) {
            const SceneLight& l = *candidates[k].light;
            LogInfo("dominant light:   {} {} at ({:.3f} {:.3f} {:.3f}) score {:.5f} strength {:.4f}", k, l.name.empty() ? std::string("(effect light)") : l.name,
                    l.position.x, l.position.y, l.position.z, candidates[k].score, candidates[k].strength);
        }
    }
}

uint32_t SceneRenderer::MirrorLights(const Draw& draw, glm::vec4 aux[2]) const {
    const uint32_t block = ForwardLights(draw, aux, 1u);
    for (uint32_t i = 0; i < light_count_ && i < 0xFFu; ++i) {
        const SceneLight& l = *light_sources_[i];
        if (l.id != kHandyLightId) {
            continue;
        }
        const glm::vec3 n = glm::normalize(-glm::vec3(draw.transform[2]));
        const glm::vec3 a = glm::normalize(l.direction);
        glm::vec3 hit(0.0f);
        const float na = glm::dot(n, a);
        if (std::abs(na) > 1.0e-5f) {
            hit = l.position + a * (glm::dot(n, glm::vec3(draw.transform[3]) - l.position) / na);
        }
        const glm::vec3 local = glm::clamp(glm::vec3(glm::inverse(draw.transform) * glm::vec4(hit, 1.0f)), draw.mesh->bounds_min, draw.mesh->bounds_max);
        const glm::vec3 to = glm::vec3(draw.transform * glm::vec4(local, 1.0f)) - l.position;
        const float dist = glm::length(to);
        const float cos_a = dist > 1.0e-6f ? glm::dot(a, to / dist) : 1.0f;
        const float cone = l.type == LightType::Spot ? std::pow(std::clamp((cos_a - l.cos_outer) * l.inv_cone_range, 0.0f, 1.0f), 2.7182817f) : 1.0f;
        aux[0].w = std::bit_cast<float>(static_cast<uint32_t>(std::lround(cone * 1023.0f)));
        return 0xFFFF00u | i;
    }
    return block;
}

void SceneRenderer::RecordForward(gpu::CommandList& cmd, const ViewSetup& view, RenderTarget& output) {
    const bool main_view = &output == &hdr_;
    bool any = false;
    for (const Draw& d : draws_) {
        const RenderPass pass = d.sub->pass;
        any = any || ((pass == RenderPass::Unlit || pass == RenderPass::Transparent) && d.sub->kind != gpu::kKindShadowOnly && Visible(d, view));
    }
    if (!any) {
        return;
    }
    if (main_view) {
        cmd.EndPass();
        cmd.UseTargets({{&particles_, gpu::Layout::ColorAttachment}, {&depth_, gpu::Layout::DepthAttachment}});
        cmd.BeginPass(extent_, {{&particles_, true, {{0.0f, 0.0f, 0.0f, 1.0f}}}}, &depth_, false, false);
    }
    gpu::PipelineHandle bound;
    for (const Draw& d : draws_) {
        const RenderPass pass = d.sub->pass;
        if ((pass != RenderPass::Unlit && pass != RenderPass::Transparent) || d.sub->kind == gpu::kKindShadowOnly || !Visible(d, view)) {
            continue;
        }
        if (main_view && rt_path_ && rt_full_traced_ && (d.sub->kind == gpu::kKindGlass || d.sub->kind == gpu::kKindParallax)) {
            continue;
        }
        const bool emissive = d.sub->kind == gpu::kKindConstant || d.sub->kind == gpu::kKindSky;
        const bool soft_glow = emissive && (textures_->MaterialFlags(d.material) & gpu::kMatGlowClip) != 0;
        const gpu::PipelineHandle pipeline = soft_glow ? forward_glow_ : emissive ? forward_emissive_ : forward_;
        if (pipeline != bound) {
            cmd.BindPipeline(gpu::BindPoint::Graphics, pipeline);
            bound = pipeline;
        }
        uint32_t flags = main_view ? 0x80u : 0u;
        glm::vec4 aux[2]{};
        if (d.sub->kind == gpu::kKindGlass) {
            flags |= ForwardLights(d, aux, view.view_bit) << 8;
        }
        if (d.sub->kind == gpu::kKindParallax) {
            bool is_mirror = mirror_stale_active_ && mirror_stale_.mesh == d.mesh && mirror_stale_.transform == d.transform;
            for (const SceneMirror* m : mirrors_) {
                is_mirror = is_mirror || (m->mesh == d.mesh && m->transform == d.transform);
            }
            if (is_mirror) {
                if (!main_view) {
                    continue;
                }
                flags |= 1u;
            }
            flags |= MirrorLights(d, aux) << 8;
            static const int mirror_dirt = [] {
                const char* value = std::getenv("PT_MIRROR_DIRT");
                return value ? std::atoi(value) : -1;
            }();
            if (mirror_dirt >= 0) {
                flags |= 2u;
                if (mirror_dirt > 0) {
                    flags |= 4u;
                }
            }
            static const bool raw_reflection = std::getenv("PT_MIRROR_RAW_REFLECTION") != nullptr;
            if (raw_reflection) {
                flags |= 8u;
            }
            static const bool linear_dirt = std::getenv("PT_MIRROR_LINEAR_DIRT") != nullptr;
            if (linear_dirt) {
                flags |= 16u;
            }
            static const bool trace_mirror_draw = std::getenv("PT_MIRROR_TRACE") != nullptr;
            static const bool trace_mirror_every = trace_mirror_draw && std::string_view(std::getenv("PT_MIRROR_TRACE")) == "every";
            if (trace_mirror_draw && (trace_mirror_every || frame_counter_ % 240 == 0) && is_mirror) {
                const uint32_t block = flags >> 8;
                LogInfo("mirror trace: draw mirror {} mesh {} flags {:#x} block {:#x} aux0 ({:.4f} {:.4f} {:.4f} {:#x}) aux1 ({:.4f} {:.4f} {:.4f} {:.4f})",
                        main_view ? "main" : "capture", d.mesh->name, flags, block, aux[0].x, aux[0].y, aux[0].z, std::bit_cast<uint32_t>(aux[0].w),
                        aux[1].x, aux[1].y, aux[1].z, aux[1].w);
            }
        }
        DrawMesh(cmd, d, view.index, flags, view.mirrored, false, aux);
    }
    if (!main_view) {
        return;
    }
    cmd.EndPass();
    cmd.UseTargets({{&output, gpu::Layout::TransferSrc}, {&hdr_copy_, gpu::Layout::TransferDst}});
    gpu::ImageCopy copy;
    copy.src = {gpu::Aspect::Color, 0, 0, 1};
    copy.dst = copy.src;
    copy.extent = {extent_.width, extent_.height, 1};
    cmd.CopyImage(output.image, gpu::Layout::TransferSrc, hdr_copy_.image, gpu::Layout::TransferDst, {&copy, 1});
    cmd.UseTargets({{&output, gpu::Layout::ColorAttachment},
                     {&hdr_copy_, gpu::Layout::ShaderReadOnly},
                     {&particles_, gpu::Layout::ShaderReadOnly}});
    cmd.BeginPass(extent_, {{&output, false, {}}});
    gpu::PassPush push;
    push.ids.x = 1u;
    Fullscreen(cmd, vfx_composite_, push);
    cmd.EndPass();
    cmd.UseTargets({{&output, gpu::Layout::ColorAttachment}, {&depth_, gpu::Layout::DepthAttachment}});
    cmd.BeginPass(extent_, {{&output, false, {}}}, &depth_, false, false);
}

void SceneRenderer::RecordEffects(gpu::CommandList& cmd, RenderTarget& output, SceneVfxContext context) {
    cmd.EndPass();
    const gpu::Extent2D effect_extent = particles_near_.Extent();
    cmd.UseTargets({{&depth_, gpu::Layout::DepthReadOnly},
                     {&near_far_depth_, gpu::Layout::ColorAttachment},
                     {&near_far_depth_attachment_, gpu::Layout::DepthAttachment}});
    cmd.BeginPass(effect_extent, {{&near_far_depth_, true, {}}}, &near_far_depth_attachment_, false, true);
    BindSets(cmd, gpu::BindPoint::Graphics);
    Fullscreen(cmd, vfx_depth_down_, gpu::PassPush{});
    cmd.EndPass();
    cmd.UseTargets({{&near_far_depth_, gpu::Layout::ShaderReadOnly},
                     {&near_far_depth_attachment_, gpu::Layout::DepthReadOnly},
                     {&particles_near_, gpu::Layout::ColorAttachment},
                     {&particles_far_, gpu::Layout::ColorAttachment}});
    const gpu::ClearColor clear_transmittance{{0.0f, 0.0f, 0.0f, 1.0f}};
    cmd.BeginPass(effect_extent, {{&particles_near_, true, clear_transmittance}, {&particles_far_, true, clear_transmittance}},
              &near_far_depth_attachment_, true, false);
    uint32_t recorded = 0;
    context.subset = kVfxOffscreen;
    context.recorded = &recorded;
    context.extent = effect_extent;
    context.depth_view = near_far_depth_attachment_.image.view;
    context.near_far_depth_view = near_far_depth_.image.view;
    bool copied = false;
    context.copy_scene = nullptr;
    if (context.scene_copy_view) {
        context.copy_scene = [&] {
            if (copied) {
                return;
            }
            copied = true;
            cmd.EndPass();
            cmd.UseTargets({{&output, gpu::Layout::TransferSrc}, {&hdr_copy_, gpu::Layout::TransferDst}});
            gpu::ImageCopy copy;
            copy.src = {gpu::Aspect::Color, 0, 0, 1};
            copy.dst = copy.src;
            copy.extent = {extent_.width, extent_.height, 1};
            cmd.CopyImage(output.image, gpu::Layout::TransferSrc, hdr_copy_.image, gpu::Layout::TransferDst, {&copy, 1});
            cmd.UseTargets({{&hdr_copy_, gpu::Layout::ShaderReadOnly},
                             {&particles_near_, gpu::Layout::ColorAttachment},
                             {&particles_far_, gpu::Layout::ColorAttachment},
                             {&near_far_depth_, gpu::Layout::ShaderReadOnly},
                             {&near_far_depth_attachment_, gpu::Layout::DepthReadOnly}});
            cmd.BeginPass(effect_extent, {{&particles_near_, false, {}}, {&particles_far_, false, {}}},
                      &near_far_depth_attachment_, true, false);
        };
    }
    vfx_forward(context);
    BindSets(cmd, gpu::BindPoint::Graphics);
    cmd.EndPass();
    if (recorded > 0) {
        cmd.UseTargets({{&output, gpu::Layout::TransferSrc}, {&hdr_copy_, gpu::Layout::TransferDst}});
        gpu::ImageCopy copy;
        copy.src = {gpu::Aspect::Color, 0, 0, 1};
        copy.dst = copy.src;
        copy.extent = {extent_.width, extent_.height, 1};
        cmd.CopyImage(output.image, gpu::Layout::TransferSrc, hdr_copy_.image, gpu::Layout::TransferDst, {&copy, 1});
        cmd.UseTargets({{&output, gpu::Layout::ColorAttachment},
                         {&hdr_copy_, gpu::Layout::ShaderReadOnly},
                         {&depth_, gpu::Layout::DepthReadOnly},
                         {&near_far_depth_, gpu::Layout::ShaderReadOnly},
                         {&particles_near_, gpu::Layout::ShaderReadOnly},
                         {&particles_far_, gpu::Layout::ShaderReadOnly}});
        cmd.BeginPass(extent_, {{&output, false, {}}});
        gpu::PassPush push;
        push.f0.x = context.projection[3][2];
        Fullscreen(cmd, vfx_composite_, push);
        cmd.EndPass();
    }
    cmd.UseTargets({{&output, gpu::Layout::ColorAttachment}, {&depth_, gpu::Layout::DepthReadOnly}});
    cmd.BeginPass(extent_, {{&output, false, {}}}, &depth_, true, false);
}

void SceneRenderer::RecordSceneCopy(gpu::CommandList& cmd, RenderTarget& output) {
    cmd.EndPass();
    cmd.UseTargets({{&output, gpu::Layout::TransferSrc}, {&hdr_copy_, gpu::Layout::TransferDst}});
    gpu::ImageCopy copy;
    copy.src = {gpu::Aspect::Color, 0, 0, 1};
    copy.dst = copy.src;
    copy.extent = {extent_.width, extent_.height, 1};
    cmd.CopyImage(output.image, gpu::Layout::TransferSrc, hdr_copy_.image, gpu::Layout::TransferDst, {&copy, 1});
    cmd.UseTargets({{&output, gpu::Layout::ColorAttachment},
                     {&hdr_copy_, gpu::Layout::ShaderReadOnly},
                     {&depth_, gpu::Layout::DepthReadOnly}});
    cmd.BeginPass(extent_, {{&output, false, {}}}, &depth_, true, false);
}

void SceneRenderer::RecordMirrorTemporal(gpu::CommandList& cmd) {
    const uint32_t size = mirror_view_.area.width;
    const glm::vec3 origin(mirrors_.front()->transform[3]);
    const float capture_exposure = frame_->views[mirror_view_.index].exposure.x;
    const bool valid_history = mirror_history_valid_ && !up_.reset && mirror_history_size_ == size &&
                               glm::distance(origin, mirror_history_origin_) < 0.001f;
    cmd.UseTargets({{&mirror_, gpu::Layout::ShaderReadOnly},
                     {&depth_, gpu::Layout::DepthReadOnly},
                     {&mirror_history_, gpu::Layout::ShaderReadOnly},
                     {&mirror_temporal_, gpu::Layout::ColorAttachment}});
    cmd.BeginPass(mirror_view_.area, {{&mirror_temporal_, false, {}}});
    gpu::PassPush push;
    push.ids = glm::uvec4(valid_history ? 1u : 0u, 0, 0, mirror_view_.index);
    push.f0 = glm::vec4(size, size, 1.0f / extent_.width, 1.0f / extent_.height);
    push.f1 = glm::vec4(capture_exposure / std::max(mirror_history_exposure_, 1.0e-12f), 0.9f, 0.0f, 0.0f);
    push.m = mirror_previous_view_projection_;
    Fullscreen(cmd, mirror_temporal_pipeline_, push);
    cmd.EndPass();
    cmd.UseTargets({{&mirror_temporal_, gpu::Layout::TransferSrc},
                     {&mirror_history_, gpu::Layout::TransferDst},
                     {&mirror_, gpu::Layout::TransferDst}});
    gpu::ImageCopy copy;
    copy.src = {gpu::Aspect::Color, 0, 0, 1};
    copy.dst = copy.src;
    copy.extent = {size, size, 1};
    cmd.CopyImage(mirror_temporal_.image, gpu::Layout::TransferSrc, mirror_history_.image, gpu::Layout::TransferDst, {&copy, 1});
    cmd.CopyImage(mirror_temporal_.image, gpu::Layout::TransferSrc, mirror_.image, gpu::Layout::TransferDst, {&copy, 1});
    cmd.UseTargets({{&mirror_, gpu::Layout::ShaderReadOnly},
                     {&mirror_history_, gpu::Layout::ShaderReadOnly}});
    mirror_previous_view_projection_ = mirror_current_view_projection_;
    mirror_history_exposure_ = capture_exposure;
    mirror_history_origin_ = origin;
    mirror_history_size_ = size;
    mirror_history_valid_ = true;
}

void SceneRenderer::RecordReflectMapMarch(gpu::CommandList& cmd, const ViewSetup& view, const glm::vec3& plane) {
    constexpr float kFarLimit = 2000.0f - 0.01f;
    constexpr float kLines = 1080.0f;
    const float aspect = static_cast<float>(extent_.width) / static_cast<float>(std::max(extent_.height, 1u));
    cmd.UseTargets({{&normal_, gpu::Layout::ShaderReadOnly},
                     {&depth_, gpu::Layout::DepthReadOnly},
                     {&refmap_, gpu::Layout::ColorAttachment}});
    gpu::PassPush push;
    push.ids = glm::uvec4(view.index, 0, 0, 0);
    push.f0 = glm::vec4(plane, kFarLimit);
    static const bool depth_legacy = std::getenv("PT_REFLECT_DEPTH_LEGACY") != nullptr;
    static const bool shift_first = std::getenv("PT_REFLECT_MARCH_SHIFT_FIRST") != nullptr;
    push.f1 = glm::vec4(kLines * aspect, kLines, depth_legacy ? 1.0f : 0.0f, shift_first ? 1.0f : 0.0f);
    push.f2 = glm::vec4(1.0f / static_cast<float>(refmap_.Extent().width), 1.0f / static_cast<float>(refmap_.Extent().height), 0.0f, 0.0f);
    static const bool march_fixed = std::getenv("PT_REFLECT_MARCH_FIXED") != nullptr;
    static const bool temporal_off_march = std::getenv("PT_REFLECT_TEMPORAL_OFF") != nullptr;
    if (up_.enabled && !temporal_off_march && !march_fixed && reflect_layer_.Valid() && reflect_temporal_pipeline_ && reflect_layer_pipeline_) {
        push.f2.z = 1.0f;
        push.f2.w = static_cast<float>(frame_counter_ % 64u);
    }
    if (rt_reflections_) {
        cmd.UseTargets({{&refmap_color_, gpu::Layout::ColorAttachment}});
        cmd.BeginPass(refmap_.Extent(), {{&refmap_, false, {}}, {&refmap_color_, false, {}}});
        static const uint32_t debug = [] {
            const char* value = std::getenv("PT_RT_REFLECTION_DEBUG");
            return value ? static_cast<uint32_t>(std::clamp(std::atoi(value), 1, 7)) : 0u;
        }();
        push.ids = glm::uvec4(view.index, light_count_, probe_count_, debug);
        uint32_t hidden[16] = {};
        for (uint32_t i = 0; i < light_count_ && i < 256; ++i) {
            if ((light_sources_[i]->hidden_views & view.view_bit) != 0) {
                hidden[i >> 5] |= 1u << (i & 31u);
            }
        }
        if (rt_glossy_active_) {
            const float on = 1.0f;
            std::memcpy(&hidden[15], &on, sizeof(on));
        }
        std::memcpy(&push.m, hidden, sizeof(hidden));
        cmd.BindPipeline(gpu::BindPoint::Graphics, reflect_make_rt_);
        const gpu::BindingSetHandle rt_set = rt_->Set(renderer_->FrameIndex());
        cmd.BindSets(gpu::BindPoint::Graphics, rt_->Layout(), 2, {&rt_set, 1});
        PushConstants(cmd, &push, sizeof(push));
        cmd.Draw(3, 1, 0, 0);
        cmd.EndPass();
        cmd.UseTargets({{&refmap_color_, gpu::Layout::ShaderReadOnly}});
        push.ids = glm::uvec4(view.index, 0, 0, 0);
        push.m = glm::mat4(1.0f);
    } else {
        cmd.BeginPass(refmap_.Extent(), {{&refmap_, false, {}}});
        Fullscreen(cmd, reflect_make_, push);
        cmd.EndPass();
    }
}

void SceneRenderer::RecordReflectMap(gpu::CommandList& cmd, const ViewSetup& view) {
    const glm::mat3 rotation(frame_->views[view.index].view);
    RecordReflectMapMarch(cmd, view, glm::normalize(rotation * glm::vec3(0.0f, 1.0f, 0.0f)));
    reflect_map_made_ = true;
}

void SceneRenderer::RecordReflections(gpu::CommandList& cmd, const ViewSetup& view) {
    const glm::mat3 rotation(frame_->views[view.index].view);
    const glm::vec3 plane = glm::normalize(rotation * glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::vec3 forward = camera_.Forward();
    const float length = glm::length(forward);
    const glm::vec3 horizontal = length > 1.0e-6f ? rotation * (forward / length) : glm::vec3(0.0f);

    if (!std::exchange(reflect_map_made_, false)) {
        RecordReflectMapMarch(cmd, view, plane);
    }
    static const bool depth_legacy = std::getenv("PT_REFLECT_DEPTH_LEGACY") != nullptr;
    gpu::PassPush push;
    push.ids = glm::uvec4(view.index, 0, 0, 0);

    cmd.UseTargets({{&refmap_, gpu::Layout::ShaderReadOnly},
                     {&hdr_, gpu::Layout::TransferSrc},
                     {&hdr_copy_, gpu::Layout::TransferDst}});
    gpu::ImageCopy copy;
    copy.src = {gpu::Aspect::Color, 0, 0, 1};
    copy.dst = copy.src;
    copy.extent = {extent_.width, extent_.height, 1};
    cmd.CopyImage(hdr_.image, gpu::Layout::TransferSrc, hdr_copy_.image, gpu::Layout::TransferDst, {&copy, 1});
    push.f0 = glm::vec4(plane, depth_legacy ? 1.0f : 0.0f);
    push.ids.z = rt_glossy_active_ ? 1u : 0u;
    push.ids.y = rt_reflections_ && renderer_->OutputMode() != RendererOutputMode::Sdr ? 1u : 0u;
    const ScreenSettings& screen = lighting_->screen;
    push.f1 = glm::vec4(1.0f, 1.0f, screen.reflect_scale, screen.reflect_bias);
    push.f2 = glm::vec4(horizontal, screen.reflect_edge ? 1.0f : 0.0f);
    static const bool temporal_off = std::getenv("PT_REFLECT_TEMPORAL_OFF") != nullptr;
    if (up_.enabled && !temporal_off && reflect_layer_.Valid() && reflect_temporal_pipeline_ &&
        (rt_reflections_ ? reflect_layer_rt_ : reflect_layer_pipeline_)) {
        RecordReflectionTemporal(cmd, view, push);
        return;
    }
    reflect_history_valid_ = false;
    reflect_post_upscale_ = false;
    cmd.UseTargets({{&hdr_copy_, gpu::Layout::ShaderReadOnly},
                     {&material_, gpu::Layout::ShaderReadOnly},
                     {&hdr_, gpu::Layout::ColorAttachment}});
    cmd.BeginPass(extent_, {{&hdr_, false, {}}});
    if (rt_reflections_) {
        cmd.BindPipeline(gpu::BindPoint::Graphics, reflect_blend_rt_);
        const gpu::BindingSetHandle rt_set = rt_->Set(renderer_->FrameIndex());
        cmd.BindSets(gpu::BindPoint::Graphics, rt_->Layout(), 2, {&rt_set, 1});
        PushConstants(cmd, &push, sizeof(push));
        cmd.Draw(3, 1, 0, 0);
    } else {
        Fullscreen(cmd, reflect_blend_, push);
    }
    cmd.EndPass();
    cmd.UseTargets({{&hdr_, gpu::Layout::ShaderReadOnly}});
}

void SceneRenderer::RecordReflectionTemporal(gpu::CommandList& cmd, const ViewSetup& view, gpu::PassPush push) {
    const float exposure = frame_->views[view.index].exposure.x;
    const bool valid_history = reflect_history_valid_ && !up_.reset && reflect_history_frame_ + 1 == frame_counter_;
    static uint32_t logged = 0;
    if (logged < 3 || (frame_counter_ % 600) == 0) {
        ++logged;
        LogInfo("reflection temporal: frame {} history {} exposure {:.4g}", frame_counter_, valid_history ? 1 : 0, exposure);
    }
    const uint32_t previous = reflect_history_index_;
    const uint32_t current = 1u - previous;
    cmd.UseTargets({{&hdr_copy_, gpu::Layout::ShaderReadOnly},
                     {&material_, gpu::Layout::ShaderReadOnly},
                     {&reflect_layer_, gpu::Layout::ColorAttachment},
                     {&reflect_offset_, gpu::Layout::ColorAttachment}});
    cmd.BeginPass(extent_, {{&reflect_layer_, false, {}}, {&reflect_offset_, false, {}}});
    push.m = previous_view_projection_;
    if (rt_reflections_) {
        cmd.BindPipeline(gpu::BindPoint::Graphics, reflect_layer_rt_);
        const gpu::BindingSetHandle rt_set = rt_->Set(renderer_->FrameIndex());
        cmd.BindSets(gpu::BindPoint::Graphics, rt_->Layout(), 2, {&rt_set, 1});
        PushConstants(cmd, &push, sizeof(push));
        cmd.Draw(3, 1, 0, 0);
    } else {
        Fullscreen(cmd, reflect_layer_pipeline_, push);
    }
    cmd.EndPass();

    cmd.UseTargets({{&reflect_layer_, gpu::Layout::ShaderReadOnly},
                     {&reflect_offset_, gpu::Layout::ShaderReadOnly},
                     {&reflect_history_[previous], gpu::Layout::ShaderReadOnly},
                     {&reflect_history_[current], gpu::Layout::ColorAttachment},
                     {&hdr_, gpu::Layout::ColorAttachment}});
    cmd.BeginPass(extent_, {{&hdr_, false, {}}, {&reflect_history_[current], false, {}}});
    gpu::PassPush temporal;
    temporal.ids = glm::uvec4(view.index, valid_history ? 1u : 0u, gpu::kImgReflectHistoryA + previous, push.ids.y);
    static const bool pre_upscale = std::getenv("PT_REFLECT_PRE_UPSCALE") != nullptr;
    reflect_post_upscale_ = !pre_upscale;
    temporal.f1 = glm::vec4(exposure / std::max(reflect_history_exposure_, 1.0e-12f), 0.9f, 1.25f, reflect_post_upscale_ ? 1.0f : 0.0f);
    Fullscreen(cmd, reflect_temporal_pipeline_, temporal);
    cmd.EndPass();
    cmd.UseTargets({{&hdr_, gpu::Layout::ShaderReadOnly},
                     {&reflect_history_[current], gpu::Layout::ShaderReadOnly}});
    reflect_history_index_ = current;
    reflect_history_frame_ = frame_counter_;
    reflect_history_exposure_ = exposure;
    reflect_history_valid_ = true;
}

void SceneRenderer::Stamp(gpu::CommandList& cmd, uint32_t index) {
    if (queries_) {
        cmd.WriteTimestamp(queries_, index);
    }
}

void SceneRenderer::RecordObjectVelocity(gpu::CommandList& cmd, const ViewSetup& view) {
    cmd.UseTargets({{&object_velocity_, gpu::Layout::ColorAttachment}, {&depth_, gpu::Layout::DepthReadOnly}});
    cmd.BeginPass(extent_, {{&object_velocity_, true, {}}}, &depth_, true, false);
    cmd.BindPipeline(gpu::BindPoint::Graphics, velocity_pipeline_);
    for (const Draw& d : draws_) {
        const SubMesh& sub = *d.sub;
        if (d.previous_model == gpu::kInvalid || sub.pass != RenderPass::Opaque || sub.kind != gpu::kKindDeferred || !Visible(d, view)) {
            continue;
        }
        cmd.SetCullMode(sub.double_sided ? gpu::CullMode::None : gpu::CullMode::Back);
        cmd.SetFrontFace(gpu::FrontFace::CounterClockwise);
        cmd.BindVertexBuffer(d.mesh->vertices.buffer, 0);
        cmd.BindIndexBuffer(d.mesh->indices.buffer, 0, gpu::IndexType::Uint32);
        gpu::DrawPush push;
        push.model = d.transform;
        push.ids = glm::uvec4(view.index, d.material, motion_view_, sub.skinned ? d.skin_base : gpu::kInvalid);
        push.tint = d.tint;
        const uint32_t previous_skin = sub.skinned ? d.previous_skin : gpu::kInvalid;
        push.aux[0] = glm::vec4(std::bit_cast<float>(d.previous_model), std::bit_cast<float>(previous_skin), 0.0f, 0.0f);
        PushConstants(cmd, &push, sizeof(push));
        cmd.DrawIndexed(sub.index_count, 1, sub.first_index, sub.vertex_offset, 0);
    }
    cmd.EndPass();
    cmd.UseTargets({{&object_velocity_, gpu::Layout::ShaderReadOnly}});
}

void SceneRenderer::RecordView(gpu::CommandList& cmd, const ViewSetup& view, RenderTarget& output, bool main_view) {
    cmd.BeginLabel(main_view ? "gbuffer" : "mirror gbuffer");
    RecordGBuffer(cmd, view);
    cmd.EndLabel();
    if (main_view) {
        Stamp(cmd, 3);
        if (MotionBlurActive()) {
            cmd.BeginLabel("object velocity");
            RecordObjectVelocity(cmd, view);
            cmd.EndLabel();
        }
        Stamp(cmd, 12);
    }
    const bool occlusion = toggles.occlusion && lighting_->valid && main_view && !rt_path_;
    if (occlusion) {
        cmd.BeginLabel("occlusion");
        RecordOcclusion(cmd, view);
        cmd.EndLabel();
    }
    if (main_view) {
        Stamp(cmd, 13);
    }
    rt_ao_ready_ = false;
    if (main_view && rt_ao_active_ && lighting_->valid) {
        cmd.BeginLabel("ray traced ambient occlusion");
        RecordRtAmbientOcclusion(cmd, view);
        cmd.EndLabel();
    }
    if (main_view) {
        Stamp(cmd, 17);
        if (RtFullAny() && lighting_->valid) {
            cmd.BeginLabel("ray traced full tier");
            RecordRtFullTrace(cmd, view);
            cmd.EndLabel();
        } else {
            Stamp(cmd, 18);
            for (uint32_t s = 25; s <= 28; ++s) {
                Stamp(cmd, s);
            }
            Stamp(cmd, 19);
            Stamp(cmd, 20);
        }
        Stamp(cmd, 14);
    }
    cmd.BeginLabel(main_view ? "lighting" : "mirror lighting");
    RecordLighting(cmd, view);
    cmd.EndLabel();
    static const bool sss_off = [] {
        const char* v = std::getenv("PT_SUBSURFACE");
        return v && v[0] == '0';
    }();
    if (main_view && sss_ready_ && sss_.HasTargets() && !sss_off && lighting_->valid && lighting_->screen.subsurface_scatter && toggles.subsurface_scatter) {
        cmd.BeginLabel("subsurface scattering");
        sss_.Record(cmd, diffuse_, view.index, [&] { BindSets(cmd, gpu::BindPoint::Graphics); });
        cmd.EndLabel();
    }
    if (main_view && rt_path_ && rt_full_traced_ && lighting_->valid) {
        cmd.BeginLabel("path traced indirect light");
        RecordPathIndirect(cmd, view);
        cmd.EndLabel();
    }
    if (main_view) {
        Stamp(cmd, 4);
        cmd.BeginLabel("luminance");
        RecordLuminance(cmd);
        cmd.EndLabel();
        Stamp(cmd, 23);
        if (rt_full_traced_ && lighting_->valid) {
            RecordRtFullBounce(cmd, view);
            if (rt_path_) {
                cmd.BeginLabel("path traced layers");
                RecordPathLayers(cmd, view);
                cmd.EndLabel();
            }
        }
        Stamp(cmd, 24);
    }
    cmd.BeginLabel(main_view ? "compose and forward" : "mirror compose and forward");
    cmd.UseTargets({{&output, gpu::Layout::ColorAttachment},
                     {&albedo_, gpu::Layout::ShaderReadOnly},
                     {&depth_, gpu::Layout::DepthReadOnly}});
    if (main_view && mirror_active_) {
        cmd.UseTargets({{&mirror_, gpu::Layout::ShaderReadOnly}});
    }
    cmd.BeginPass(ViewArea(view), {{&output, false, {}}}, &depth_, true, false);
    gpu::PassPush push;
    push.ids = glm::uvec4(view.index, occlusion ? 1u : 0u, 0, 0);
    const TppAtmosphereSettings& tpp = lighting_->tpp;
    if (tpp.enabled && toggles.tpp_atmosphere) {
        push.ids.z = 1u;
        static const bool sky_off = [] {
            const char* s = std::getenv("PT_SKY");
            return s && std::string_view(s) == "0";
        }();
        if (tpp.sky && !sky_off) {
            push.ids.z |= 4u;
        }
    }
    if (main_view) {
        bloom_shoulder_ = glm::vec4(0.0f);
    }
    if (tpp.tonemap && toggles.tpp_atmosphere) {
        push.ids.z |= 2u;
        const float t = std::clamp(tpp.threshold, 0.0f, 0.99f);
        const float u = std::max(tpp.range - t, 1.0e-3f);
        float c = u * (1.0f - t) / std::max(t + u - 1.0f, 1.0e-3f);
        const float sdr_c = c;
        if (hdr_post_) {
            c = std::max(c, render::HdrShoulderAsymptote(frame_->display.y) - t);
        }
        push.f0 = glm::vec4(1.0f / (c * c), t, c, 0.0f);
        if (main_view && hdr_post_) {
            bloom_shoulder_ = glm::vec4(c, sdr_c, t, 1.0f);
        }
    }
    Fullscreen(cmd, compose_, push);
    const bool reference = main_view && PathReference() && rt_full_traced_ && lighting_->valid;
    if (reference) {
        RecordPathReferenceOut(cmd, view);
    } else if (main_view && rt_path_ && rt_full_traced_ && lighting_->valid) {
        RecordPathLayerOut(cmd, view);
    }
    cmd.EndPass();
    if (main_view && dump_requested_ && &output == &hdr_) {
        RecordDumpCopy(cmd, output, dump_compose_);
    }
    if (main_view && reflect_map_early_ && &output == &hdr_) {
        RecordReflectMap(cmd, view);
    }
    if (&output == &hdr_ && flare_depth_.Valid()) {
        cmd.UseTargets({{&depth_, gpu::Layout::TransferSrc}, {&flare_depth_, gpu::Layout::TransferDst}});
        gpu::ImageCopy copy;
        copy.src = {gpu::Aspect::Depth, 0, 0, 1};
        copy.dst = copy.src;
        copy.extent = {extent_.width, extent_.height, 1};
        cmd.CopyImage(depth_.image, gpu::Layout::TransferSrc, flare_depth_.image, gpu::Layout::TransferDst, {&copy, 1});
        cmd.UseTargets({{&flare_depth_, gpu::Layout::DepthReadOnly}});
    }
    cmd.UseTargets({{&depth_, gpu::Layout::DepthAttachment}});
    cmd.BeginPass(ViewArea(view), {{&output, false, {}}}, &depth_, false, false);
    if (!reference) {
        RecordForward(cmd, view, output);
    }
    cmd.EndPass();
    if (main_view && dump_requested_ && &output == &hdr_) {
        RecordDumpCopy(cmd, output, dump_forward_);
    }
    cmd.UseTargets({{&depth_, gpu::Layout::DepthReadOnly}});
    if (main_view) {
        Stamp(cmd, 7);
    }
    cmd.BeginPass(ViewArea(view), {{&output, false, {}}}, &depth_, true, false);
    if (main_view && up_.enabled) {
        RecordOpaqueSnapshot(cmd, output);
    }
    SceneVfxContext liquid_context;
    bool liquid_depth = false;
    if (vfx_forward) {
        SceneVfxContext context;
        context.cmd = &cmd;
        context.view = view.view;
        context.projection = view.projection;
        context.view_projection = view.projection * view.view;
        context.eye = view.eye;
        context.extent = extent_;
        context.color_format = kHdrTargetFormat;
        context.depth_format = kDepthTargetFormat;
        context.depth_view = depth_.image.view;
        context.exposure = exposure_;
        context.frame_index = renderer_->FrameIndex();
        context.mirrored = view.mirrored || !main_view;
        if (lighting_->tpp.enabled && toggles.tpp_atmosphere) {
            std::copy(std::begin(frame_->fog), std::end(frame_->fog), std::begin(context.fog));
            context.fog_mode = 1u;
        }
        context.scene_copy_view = hdr_copy_.image.view;
        if (&output == &hdr_) {
            context.copy_scene = [&] { RecordSceneCopy(cmd, output); };
            context.subset = kVfxScene;
        }
        vfx_forward(context);
        BindSets(cmd, gpu::BindPoint::Graphics);
        if (&output == &hdr_) {
            RecordEffects(cmd, output, context);
        }
        liquid_context = context;
        liquid_depth = vfx_liquid_depth && main_view && &output == &hdr_ && !context.mirrored;
    }
    cmd.EndPass();
    if (liquid_depth) {
        cmd.UseTargets({{&depth_, gpu::Layout::DepthAttachment}, {&hdr_copy_, gpu::Layout::ShaderReadOnly}});
        cmd.BeginPass(ViewArea(view), {}, &depth_, false, false);
        liquid_context.cmd = &cmd;
        vfx_liquid_depth(liquid_context);
        cmd.EndPass();
        cmd.UseTargets({{&depth_, gpu::Layout::DepthReadOnly}});
        BindSets(cmd, gpu::BindPoint::Graphics);
    }
    if (main_view && dump_requested_ && &output == &hdr_) {
        RecordDumpCopy(cmd, output, dump_scene_);
        dump_recorded_ = true;
    }
    cmd.UseTargets({{&output, gpu::Layout::ShaderReadOnly}});
    cmd.EndLabel();
    if (main_view) {
        Stamp(cmd, 5);
    }
}

void SceneRenderer::SettleExposure(const ExposureSettings& settings) {
    FrameSlot& slot = slots_[renderer_->FrameIndex()];
    gpu_->Submit([&](gpu::CommandList& cmd) {
        cmd.BeginLabel("settle exposure");
        BindSets(cmd, gpu::BindPoint::Graphics);
        RecordShadows(cmd);
        RecordGBuffer(cmd, main_view_);
        RecordLighting(cmd, main_view_);
        RecordLuminance(cmd);
        cmd.EndLabel();
    });
    gpu_->InvalidateMapped(slot.luminance, 0, ~0ull);
    const float measured = Meter(static_cast<const glm::vec2*>(slot.luminance.mapped), slot.luminance_groups);
    adaptation_valid_ = true;
    exposure_samples_.clear();
    if (measured < 0.0f) {
        return;
    }
    const float raw = measured / std::max(exposure_, 1.0e-12f);
    if (!std::isfinite(raw)) {
        adaptation_valid_ = false;
        return;
    }
    float ev = ev_;
    for (int i = 0; i < 64; ++i) {
        const float adapted = raw * std::exp2(ev);
        ev = std::clamp(ev + 0.5f * (std::log2(settings.key + 0.001f) - std::log2(adapted + 0.001f)), settings.min_ev, settings.max_ev);
    }
    ev_ = ev;
    exposure_ = ExposureFor(ev_, settings) * renderer_->exposure;
    for (uint32_t i = 0; i < view_count_; ++i) {
        frame_->views[i].exposure = glm::vec4(exposure_, 1.0f / std::max(exposure_, 1.0e-12f), ev_, frame_->views[i].exposure.w);
    }
    SetTppFog(lighting_->tpp);
    UploadFrame(slot);
    LogInfo("scene renderer: exposure settled at ev {:.2f} (luminance {:.4f}, exposure {:.5f}), {} lights, {} probes, {} shadow views, {} draws", ev_,
            raw, exposure_, light_count_, probe_count_, shadow_views_.size(), draws_.size());
}

void SceneRenderer::Render(const Camera& camera, const std::vector<DrawItem>& items) {
    Render(camera, items, fallback_, 1.0f / 60.0f);
}

void SceneRenderer::Render(const Camera& camera, const std::vector<DrawItem>& items, const SceneLighting& lighting, float dt) {
    const auto cpu_start = std::chrono::steady_clock::now();
    if (frame_counter_ == 0) {
        ApplyEnvironmentOverrides();
    }
    struct ToggleGuard {
        RenderToggles& toggles;
        RenderToggles saved;
        ~ToggleGuard() { toggles = saved; }
    } toggle_guard{toggles, toggles};
    if (vr_eye_ >= 0) {
        toggles.depth_of_field = false;
        toggles.motion_blur = false;
        toggles.film_grain = false;
        toggles.distortion = false;
        toggles.full_screen_blur = false;
    }
    struct DateBaselineGuard {
        SceneRenderer& renderer;
        RayTracingSettings raytracing;
        AppSettings::Graphics graphics;
        bool on;
        ~DateBaselineGuard() {
            if (on) {
                renderer.raytracing = raytracing;
                renderer.graphics = graphics;
            }
        }
    } date_baseline{*this, raytracing, graphics, false};
    static const bool date_baseline_off = [] {
        const char* v = std::getenv("PT_DATE_BASELINE");
        return v && std::string_view(v) == "0";
    }();
    if (date_resolve_on_ && !date_baseline_off && vr_eye_ < 0) {
        date_baseline.on = true;
        raytracing = RayTracingSettings{};
        const AppSettings::Graphics original;
        graphics.shadow_quality = original.shadow_quality;
        graphics.ambient_occlusion = original.ambient_occlusion;
        graphics.reflections = original.reflections;
        graphics.ray_quality = original.ray_quality;
        graphics.clarity = original.clarity;
    }
    if (std::chrono::steady_clock::now() < targets_retry_at_) return;
    if(!EnsureShadowTarget()) {
        targets_retry_at_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        return;
    }
    SetHdrPost(renderer_->OutputMode() != RendererOutputMode::Sdr);
    vfx::SetFlareSmooth(hdr_post_);
    frame_->display = glm::vec4(hdr_post_ ? 1.0f : 0.0f, renderer_->HdrPeak() / std::max(renderer_->HdrPaperWhite(), 1.0f),
                                hdr_post_ ? renderer_->HdrReach() : 1.0f, 0.0f);
    const render::HdrLook& look = renderer_->HdrLookSettings();
    frame_->grade = glm::vec4(graphics.pt_floor_reflection * 0.01f, hdr_post_ && look.keep_hue ? 1.0f : 0.0f, graphics.pt_glass_reflection * 0.01f,
                              graphics.pt_reflection_softness * 0.01f);
    const gpu::Extent2D output = renderer_->RenderExtent();
    const bool upscaling = BeginUpscaleFrame(output);
    if (!EnsureTargets(upscaling ? up_.render : output, output, upscaling)) {
        bool made = false;
        while (!made && shadow_.Valid() && shadow_.Extent().width > 512u * 4) {
            gpu_->WaitIdle();
            gpu_->DestroyImage(shadow_.image);
            shadow_ = RenderTarget{};
            shadow_tile_cap_ = std::max(512u, shadow_tile_cap_ / 2);
            LogWarn("graphics: shadow atlas tile lowered to {} px for the render targets: out of video memory", shadow_tile_cap_);
            made = EnsureShadowTarget() && EnsureTargets(upscaling ? up_.render : output, output, upscaling);
        }
        if (!made) {
            targets_retry_at_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            return;
        }
    }
    FrameSlot& slot = slots_[renderer_->FrameIndex()];
    textures_->FlushMaterials();
    const int measured_vr_eye = slot.vr_eye;
    const bool measured_vr_gpu = slot.timed;
    if (has_last_eye_ && glm::distance(camera.position, last_eye_) > kCameraCutDistance) {
        adaptation_valid_ = false;
    }
    last_eye_ = camera.position;
    has_last_eye_ = true;
    lighting_ = lighting.valid ? &lighting : &fallback_;
    UpdateColorLut(lighting_->screen);
    UpdateReflectionTexture(lighting_->reflection_texture);
    ReadMeasurements(slot, dt, lighting_->exposure);
    if (lighting_->exposure.pinned) {
        ev_ = lighting_->exposure.pinned_ev;
        adaptation_valid_ = true;
    } else if (ev_pinned_) {
        adaptation_valid_ = false;
    }
    ev_pinned_ = lighting_->exposure.pinned;
    if (!timing_ring_.empty()) {
        stats_.device_mb = static_cast<float>(static_cast<double>(gpu_->AllocatedBytes()) / (1024.0 * 1024.0));
    }
    if (slot.timed && frame_counter_ > 8) {
        gpu_ms_sum_ += stats_.gpu_ms;
        ++gpu_samples_;
        if (!timing_ring_.empty()) {
            FrameTiming& t = timing_ring_[timing_count_++ % timing_ring_.size()];
            t.gpu_ms = stats_.gpu_ms;
            std::copy(std::begin(stats_.pass_ms), std::end(stats_.pass_ms), std::begin(t.pass_ms));
            std::copy(std::begin(stats_.part_ms), std::end(stats_.part_ms), std::begin(t.part_ms));
            std::copy(std::begin(stats_.light_ms), std::end(stats_.light_ms), std::begin(t.light_ms));
            std::copy(std::begin(stats_.rt_ms), std::end(stats_.rt_ms), std::begin(t.rt_ms));
            std::copy(std::begin(stats_.pt_ms), std::end(stats_.pt_ms), std::begin(t.pt_ms));
            t.device_mb = stats_.device_mb;
            t.frame = frame_counter_ >= Renderer::kFramesInFlight ? frame_counter_ - Renderer::kFramesInFlight : 0;
        }
    }
    ReadUpscaleTimestamps(slot);
    if (lighting_->valid && toggles.adaptation) {
        exposure_ = ExposureFor(ev_, lighting_->exposure) * renderer_->exposure;
    } else {
        exposure_ = renderer_->exposure;
    }
    UpdateMotion(camera, dt);
    const bool rt_ready = rt_ && rt_->Ready(renderer_->FrameIndex());
    rt_active_ = rt_ready && raytracing.shadows && toggles.shadows;
    rt_reflections_ = rt_ready && reflect_make_rt_ && raytracing.reflections && toggles.local_reflections;
    rt_contact_active_ = rt_ready && light_contact_ && raytracing.contact_shadows && toggles.shadows;
    rt_ao_active_ = rt_ready && rt_ao_trace_ && rt_ao_filter_ && probe_ao_ && raytracing.ambient_occlusion && toggles.probes && EnsureAoTargets();
    const bool full_ready = rt_ready && raytracing.full && light_full_ && toggles.lights;
    rt_full_shadows_ = full_ready && raytracing.full_shadows && rt_active_;
    rt_gi_active_ = full_ready && raytracing.bounce_light;
    rt_glossy_active_ = full_ready && raytracing.glossy_reflections;
    rt_full_traced_ = false;
    rt_path_ = rt_ready && raytracing.path_traced && pt_direct_ && pt_resolve_ && rt_full_filter_ && toggles.lights && vr_eye_ < 0;
    if (rt_path_) {
        rt_full_shadows_ = rt_gi_active_ = rt_glossy_active_ = false;
    }
    fsr_rr_input_check_ = std::getenv("PT_FSR_RR_INPUT_CHECK") && gpu_->Info().backend == gpu::Backend::D3D12 &&
                          up_.kind == UpscalerKind::Fsr;
    rr_frame_ = rt_path_ && up_.enabled && up_.backend && (up_.ray_reconstruction || fsr_rr_input_check_) && pt_rr_guides_;
    if (RtFullAny() && !EnsureFullTargets()) {
        rt_full_shadows_ = rt_gi_active_ = rt_glossy_active_ = rt_path_ = false;
    }
    if (rt_path_ && !EnsurePathTargets()) {
        rt_path_ = false;
    }
    if (rr_frame_ && (!rt_path_ || !EnsureRrTargets())) {
        rr_frame_ = false;
    }
    if (rr_frame_ && (up_.kind == UpscalerKind::Fsr4 || fsr_rr_input_check_) && !EnsureFsrRrTargets()) {
        up_.backend->DisableRayReconstruction("FSR Ray Regeneration inputs could not be allocated");
        rr_frame_ = false;
    }
    const bool traced_any = rt_active_ || rt_reflections_ || rt_contact_active_ || rt_ao_active_ || RtFullAny();
    rr_raster_ = up_.kind == UpscalerKind::Dlss && !rt_path_ && traced_any && up_.enabled && up_.backend && up_.ray_reconstruction && rt_rr_guides_ && vr_eye_ < 0 &&
                 EnsureRrTargets();
    if (rt_path_) {
        rt_contact_active_ = false;
        rt_ao_active_ = false;
    } else {
        pt_history_ = false;
    }
    if (rt_full_shadows_) {
        rt_contact_active_ = false;
    }
    if (!RtFullAny() || vr_eye_ >= 0) {
        rt_full_history_ = false;
    }
    if (!rt_ao_active_ || vr_eye_ >= 0) {
        rt_ao_history_ = false;
    }
    rt_ao_ready_ = false;
    UpdateDominantLight(camera, *lighting_, timed_ ? pending_time_ : dt);
    pending_time_ = 0.0f;
    PrepareFrame(camera, items, *lighting_);
    SetTppFog(lighting_->tpp);
    UploadFrame(slot);
    if (!adaptation_valid_ && lighting_->valid && toggles.adaptation) {
        SettleExposure(lighting_->exposure);
    }
    gpu::CommandList& cmd = renderer_->List();
    cmd.ResetQueries(slot.queries, 0, kTimestamps);
    if (up_.enabled) {
        cmd.ResetQueries(slot.upscale_queries, 0, 4);
    }
    queries_ = slot.queries;
    Stamp(cmd, 0);
    BindSets(cmd, gpu::BindPoint::Graphics);
    cmd.BeginLabel("shadows");
    RecordShadows(cmd);
    cmd.EndLabel();
    Stamp(cmd, 1);
    if (mirror_render_cache_index_ != mirror_cache_index_) {
        auto copy_capture = [&](RenderTarget& source, RenderTarget& target, uint32_t size) {
            cmd.UseTargets({{&source, gpu::Layout::TransferSrc}, {&target, gpu::Layout::TransferDst}});
            gpu::ImageCopy copy;
            copy.src = {gpu::Aspect::Color, 0, 0, 1};
            copy.dst = copy.src;
            copy.extent = {size, size, 1};
            cmd.CopyImage(source.image, gpu::Layout::TransferSrc, target.image, gpu::Layout::TransferDst, {&copy, 1});
            cmd.UseTargets({{&source, gpu::Layout::ShaderReadOnly}, {&target, gpu::Layout::ShaderReadOnly}});
        };
        const MirrorStale& previous = mirror_stale_cache_[mirror_render_cache_index_];
        if (previous.valid) {
            copy_capture(mirror_, mirror_capture_cache_[mirror_render_cache_index_], previous.size);
        }
        if (mirror_stale_.valid) {
            copy_capture(mirror_capture_cache_[mirror_cache_index_], mirror_, mirror_stale_.size);
        }
        mirror_render_cache_index_ = mirror_cache_index_;
        mirror_history_valid_ = false;
    }
    if (mirror_active_) {
        RecordView(cmd, mirror_view_, mirror_, false);
        if (mirror_temporal_enabled_) {
            RecordMirrorTemporal(cmd);
        } else {
            mirror_history_valid_ = false;
        }
        if (dump_requested_) {
            RecordDumpCopy(cmd, mirror_, dump_mirror_);
        }
        static const bool trace = std::getenv("PT_MIRROR_TRACE") != nullptr;
        if (trace) {
            uint32_t drawn = 0;
            for (const Draw& d : draws_) {
                drawn += Visible(d, mirror_view_) ? 1u : 0u;
            }
            LogInfo("mirror trace: capture view {} eye ({:.3f} {:.3f} {:.3f}) distance {:.3f} size {} draws {} of {}, lights {}, exposure {:.6f} "
                    "(main {:.6f} ev {:.3f})",
                    mirror_view_.index, mirror_view_.eye.x, mirror_view_.eye.y, mirror_view_.eye.z, mirror_view_.projection[3][2],
                    mirror_view_.area.width, drawn, draws_.size(), light_count_, frame_->views[mirror_view_.index].exposure.x, exposure_, ev_);
            static const bool probe_capture = [] {
                const char* value = std::getenv("PT_MIRROR_PROBE");
                return value && std::atoi(value) != 0;
            }();
            static gpu::BufferResource probe_buffers[3];
            if (probe_capture) {
                RenderTarget* targets[3] = {&albedo_, &material_, &diffuse_};
                const char* names[3] = {"albedo", "material", "diffuse"};
                const uint32_t side = mirror_view_.area.width;
                for (int t = 0; t < 3; ++t) {
                    RenderTarget& target = *targets[t];
                    if (!target.Valid()) {
                        continue;
                    }
                    const gpu::Extent3D e = target.image.extent;
                    const uint64_t bytes = uint64_t(e.width) * e.height * TargetTexelBytes(target.image.format);
                    if (probe_buffers[t].size < bytes) {
                        gpu_->DestroyBuffer(probe_buffers[t]);
                        MakeBuffer(probe_buffers[t], bytes, gpu::BufferUsage::TransferDst, true);
                    }
                    if (!probe_buffers[t].mapped) {
                        continue;
                    }
                    RecordDumpCopy(cmd, target, probe_buffers[t]);
                    gpu_->Submit([&](gpu::CommandList& sync) { (void)sync; });
                    gpu_->InvalidateMapped(probe_buffers[t], 0, ~0ull);
                    const bool wide = TargetTexelBytes(target.image.format) == 8;
                    double sum = 0.0;
                    float peak = 0.0f;
                    size_t lit = 0;
                    for (uint32_t y = 0; y < side; ++y) {
                        for (uint32_t x = 0; x < side; ++x) {
                            const size_t at = (size_t(y) * e.width + x) * 4;
                            float value = 0.0f;
                            if (wide) {
                                const uint16_t* half = static_cast<const uint16_t*>(probe_buffers[t].mapped);
                                for (uint32_t c = 0; c < 3; ++c) {
                                    value = std::max(value, HalfToFloat(half[at + c]));
                                }
                            } else {
                                const uint8_t* bytes8 = static_cast<const uint8_t*>(probe_buffers[t].mapped);
                                for (uint32_t c = 0; c < 3; ++c) {
                                    value = std::max(value, float(bytes8[at + c]) / 255.0f);
                                }
                            }
                            sum += value;
                            peak = std::max(peak, value);
                            lit += value > 1.0e-4f ? 1u : 0u;
                        }
                    }
                    const double mean = sum / (double(side) * side);
                    LogInfo("mirror trace: capture {} {}x{} mean {:.6f} peak {:.4f} lit {} of {} ({:.1f} %)", names[t], side, side, mean, peak, lit,
                            size_t(side) * side, 100.0 * double(lit) / (double(side) * side));
                }
            }
        }
    } else {
        mirror_history_valid_ = false;
    }
    Stamp(cmd, 2);
    static const bool map_after_forward = std::getenv("PT_REFLECT_MAP_AFTER_FORWARD") != nullptr;
    reflect_map_early_ = !map_after_forward && (debug_view == DebugView::Lit || debug_view == DebugView::Ambient) &&
                         toggles.local_reflections && lighting_->valid && lighting_->screen.local_reflections && !rt_path_;
    reflect_map_made_ = false;
    RecordView(cmd, main_view_, hdr_, true);
    RecordReflectionSample(cmd, *lighting_);
    BindSets(cmd, gpu::BindPoint::Graphics);
    if (debug_view == DebugView::Lit || debug_view == DebugView::Ambient) {
        if (up_.enabled) {
            cmd.BeginLabel("upscale inputs");
            RecordUpscaleInputs(cmd, main_view_);
            cmd.EndLabel();
        }
        Stamp(cmd, 8);
        reflect_post_upscale_ = false;
        if (toggles.local_reflections && lighting_->valid && lighting_->screen.local_reflections && !rt_path_) {
            cmd.BeginLabel("reflections");
            RecordReflections(cmd, main_view_);
            cmd.EndLabel();
        }
        Stamp(cmd, 9);
        if (up_.enabled) {
            if (up_.backend && up_.backend->ExternalSubmit() && vr_eye_ < 0) {
                cmd.BeginLabel("upscale");
                RecordUpscalePrepare(dt);
                if (upscale.frame_generation == FrameGenKind::Metalfx) {
                    PrepareFrameGeneration(up_dispatch_, true);
                }
                cmd.EndLabel();
                VkSemaphore signal = VK_NULL_HANDLE;
                VkSemaphore wait = VK_NULL_HANDLE;
                uint64_t signal_value = 0;
                uint64_t wait_value = 0;
                if (up_.backend->ExternalSync(signal, signal_value, wait, wait_value) && renderer_->SplitForExternal(signal, signal_value, wait, wait_value)) {
                    const bool upscaled = up_.backend->ExternalRun(up_dispatch_);
                    cmd.BeginLabel("upscale resolve");
                    BindSets(cmd, gpu::BindPoint::Graphics);
                    RecordUpscaleResolve(cmd, upscaled);
                    cmd.EndLabel();
                } else {
                    upscale_stats_.error = "the upscaler's frame split failed";
                    RecordUpscaleResolve(renderer_->List(), false);
                }
                post_bindings_ = true;
                BindSets(cmd, gpu::BindPoint::Graphics);
            } else {
                cmd.BeginLabel("upscale");
                RecordUpscale(cmd, dt);
                cmd.EndLabel();
                post_bindings_ = true;
                BindSets(cmd, gpu::BindPoint::Graphics);
            }
        }
        Stamp(cmd, 10);
        cmd.BeginLabel("post");
        RecordPost(cmd, *lighting_, dt);
        cmd.EndLabel();
        post_bindings_ = false;
    } else {
        for (uint32_t index : {8u, 9u, 10u, 11u}) {
            Stamp(cmd, index);
        }
        cmd.BeginLabel("debug view");
        RecordDebug(cmd);
        cmd.EndLabel();
    }
    Stamp(cmd, 6);
    queries_ = {};
    last_timed_slot_ = renderer_->FrameIndex();
    slot.timed = true;
    slot.measured = true;
    slot.exposure_used = exposure_;
    slot.exposure_sample_time = exposure_time_;
    slot.vr_eye = vr_eye_;
    stats_.ev = ev_;
    stats_.exposure = exposure_;
    if (vr_eye_ != 0) {
        ++frame_counter_;
    }
    stats_.cpu_ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - cpu_start).count();
    static const bool vr_trace = std::getenv("PT_VR_RENDER_TRACE") != nullptr;
    if (vr_trace && vr_eye_ >= 0) {
        const uint64_t submission = vr_trace_submissions_++;
        if (vr_trace_lines_ < 512 && (submission < 8 || submission % 120 < 2)) {
            LogInfo("vr render: submission {} eye {} slot {} measured eye {} valid {} gpu {:.3f} ms cpu {:.3f} ms size {}x{} exposure {:.5f}",
                    submission, vr_eye_, renderer_->FrameIndex(), measured_vr_eye, measured_vr_gpu, stats_.gpu_ms, stats_.cpu_ms,
                    output.width, output.height, exposure_);
            if (++vr_trace_lines_ == 512) LogInfo("vr render: trace limit reached (512 eye timings)");
        }
    }
    if (static const bool frame_csv = std::getenv("PT_FRAME_CSV") != nullptr; frame_csv) {
        const float interval = previous_frame_start_ == std::chrono::steady_clock::time_point{}
                                   ? 0.0f
                                   : std::chrono::duration<float, std::milli>(cpu_start - previous_frame_start_).count();
        const vk::PipelineCreationStats pipelines = vk::PipelineStats(renderer_->Context().device);
        const uint64_t created = pipelines.graphics_count + pipelines.compute_count - pipelines.background_count;
        const uint64_t created_ns = pipelines.duration_ns - pipelines.background_ns;
        frame_trace_.push_back({stats_.cpu_ms, interval, stats_.lights, stats_.shadow_views, stats_.draws,
                                static_cast<uint32_t>(created - frame_trace_pipelines_),
                                static_cast<float>(static_cast<double>(created_ns - frame_trace_pipeline_ns_) * 1e-6)});
        frame_trace_pipelines_ = created;
        frame_trace_pipeline_ns_ = created_ns;
    }
    previous_frame_start_ = cpu_start;
    if (frame_counter_ > 8) {
        cpu_ms_sum_ += stats_.cpu_ms;
        ++cpu_samples_;
    }
}

}
