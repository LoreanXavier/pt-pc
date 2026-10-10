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
#include <string>

#include "engine/core/log.h"
#include "engine/fs/vfs.h"
#include "engine/render/scene_renderer.h"

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

// The MirrorLight stands behind the bathroom wall, in the hallway, and shines into the room through the mirror opening. The
// capture caster set bypasses paper_a1 (submesh 10), a broad sheet 0.158 m behind the glass, and wood_b (submesh 2), the casing.
// The paper bypass lets rays outside the glass reach the wall; the MirrorLight's projective area test supplies the finite
// aperture. This is a port model inferred from the mesh and the PS4 patch, not a claim about the original shadow-map contents.
// PT_MIRROR_SKIP_SUB=<n,n,...> replaces the list (-1: none).
// Measured against mirror_f060 5335's 2048x2048 shadow passes (trace.txt, draws counted by index count): the pass that matches the
// MirrorLight's holds the port's draws except the cockroach models (coc0_main0_def, 37 draws, none in any original pass), the
// baby, the wall clock, the sconce shsb_lght005 and the flashlight's lens: those stay out of the MirrorLight's casters.
bool MirrorLightSkips(const GpuMesh& mesh, int sub) {
    static const bool all_casters = std::getenv("PT_MIRROR_CASTERS_ALL") != nullptr;  // tests: the room's casters as before
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

// PT_SHADOW_LEGACY=1: the port's former shadow stand-ins for A/B tests (a slope scaled polygon offset of -1.5 constant and
// -2 slope on every shadow draw, no viewBias receiver offset and no half texel offset in the spot lookup). Otherwise the
// original's: no polygon offset, and the spot receiver moved by viewBias along the view's z (0xDC07F0, rendering.md 12.5).
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

// An oriented box: centre, unit axes, half extents.
struct OrientedBox {
    glm::vec3 center{0.0f};
    glm::vec3 axes[3] = {glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f)};
    glm::vec3 half{0.0f};
};

// a box matrix that maps [-1, 1] on each axis to the world (a light or probe area)
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

// 0x534960: the separating axis test of two oriented boxes (the 15 axes, 1e-6 added to the rotation terms)
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

// 0x534440: a point within 1 mm of an oriented box
bool PointInBox(const glm::vec3& p, const OrientedBox& b) {
    const glm::vec3 d = p - b.center;
    float sq = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float out = std::max(std::abs(glm::dot(d, b.axes[i])) - b.half[i], 0.0f);
        sq += out * out;
    }
    return sq < 1.0e-6f;
}

// the clip w of the light's rank point (GrLight +0x80) sets the screen size
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

// The box min to max lies wholly in the space an occluder polygon hides from the eye: beyond its plane and inside the planes
// through the eye and its edges
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
        // the box corner least inside this plane
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

// the box centre c, half size h meets the space inside the four side planes (the light query has no near plane:
// lightsel_f060_1 keeps a spot light whose box lies wholly behind the camera)
bool BoxInPlanes(const glm::vec3& c, const glm::vec3& h, const glm::vec4* planes) {
    for (int i = 0; i < 4; ++i) {
        const glm::vec3 n = glm::vec3(planes[i]);
        if (glm::dot(n, c) + planes[i].w < -glm::dot(glm::abs(n), h)) {
            return false;
        }
    }
    return true;
}

// The light's box in the scene grid. Point lights: the position plus and minus outerRange (0xCDB1A0). Spot lights
// (0xCDB4F0): the apex, the point r = outerRange / cos(u / 2) along the axis and the octagon around the circle at angle u
// from the axis at distance r (u the umbra half angle, at most 90 degrees; octagon corners (1, tan 22.5) and its turns
// in the light's frame), cut to the light area's box (0x512340); the cut can leave a box whose minimum passes its maximum
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

// 0xCC3530's score of a light for a box seen from its centre with radius r (half the box's largest extent): |colour| times
// the spot's cone^e at the centre times 1/d^2 - d^2/R^4, d = max(distance, inner range), R = outer range + r. cone is the
// spot's cone^e at the centre, 1 for a point light.
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

// the texel size of the formats the dump copy writes (mirror diagnostics), as scene_renderer.cpp has it
static uint32_t TargetTexelBytes(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        return 16;
    default:
        return 4;
    }
}

// IEEE 754 half to float, for the readbacks of the 16-bit float targets (mirror diagnostics)
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
    return (draw.hidden_views & view.view_bit) == 0 && Visible(draw, view.planes);
}

void SceneRenderer::PrepareFrame(const Camera& camera, const std::vector<DrawItem>& items, const SceneLighting& lighting) {
    camera_ = camera;
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
        // w: the shadow taps' per frame rotation (lighting.glsl ShadowPcf), only with PT_SHADOW_ROTATE=1
        static const float shadow_rotate = std::getenv("PT_SHADOW_ROTATE") ? 1.0f : 0.0f;
        v.jitter = up_.enabled ? glm::vec4(up_.jitter_ndc, up_.mip_bias, shadow_rotate) : glm::vec4(0.0f);
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
    // the dither and shadow tap patterns the upscaler averages (View temporal): the Bayer rank steps by 39 a frame (about
    // 64 / phi, odd so all 64 come round) and d cycles (1,1) (-1,-1) (1,-1) (-1,1); PT_UPSCALE_FIXED_DITHER=1 keeps the
    // native fixed patterns under the upscaler, for A/B runs
    static const bool fixed_dither = std::getenv("PT_UPSCALE_FIXED_DITHER") != nullptr;
    if (up_.enabled && !fixed_dither) {
        static constexpr glm::vec2 kTaps[4] = {{1.0f, 1.0f}, {-1.0f, -1.0f}, {1.0f, -1.0f}, {-1.0f, 1.0f}};
        frame_->views[main_view_.index].temporal =
            glm::vec4(static_cast<float>((frame_counter_ * 39u) & 63u), kTaps[frame_counter_ & 3u], 1.0f);
    }
    motion_view_ = add_view(projection, gl_view, camera.position, glm::vec4(0.0f));
    frame_->views[motion_view_].dominant_light = dominant_light;
    frame_->views[motion_view_].view_projection = previous_view_projection_;
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
        // PT_DRAW_EXCLUDE=<text>: leave out every model whose name contains it (A/B of one model's share of a frame)
        if (static const char* exclude = std::getenv("PT_DRAW_EXCLUDE"); exclude && mesh.name.find(exclude) != std::string::npos) {
            continue;
        }
        for (size_t i = 0; i < mesh.submeshes.size(); ++i) {
            const SubMesh& sub = mesh.submeshes[i];
            if (masked && i < kMaxHiddenMeshes && item.hidden_meshes[i]) {
                continue;
            }
            // PT_DRAW_EXCLUDE_MESH=<model text>:<mesh index>[,...]: leave out one mesh of the models whose name contains the text
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
            d.mesh = item.mesh;
            d.sub = &sub;
            d.transform = item.transform;
            d.tint = item.tint;
            d.material = item.material_override >= 0 ? static_cast<uint32_t>(item.material_override) : sub.material;
            d.skin_base = skin_base;
            d.previous_model = previous_model;
            d.previous_skin = previous_skin;
            d.center = center;
            d.radius = local_radius * scale;
            d.hidden_views = item.hidden_views;
            draws_.push_back(d);
        }
    }
    motion_history_.swap(motion_history_next_);
    motion_history_next_.clear();
    // PT_TARGET_DUMP shots: every draw whose bounds come within 0.3 m of the camera, which the depth of a full screen
    // surface just in front of the eye points at
    if (dump_requested_) {
        for (const Draw& d : draws_) {
            if (glm::distance(d.center, camera.position) < d.radius + 0.3f) {
                LogInfo("near draw: {} submesh {} pass {} kind {} hidden {:#x} centre ({:.2f} {:.2f} {:.2f}) radius {:.2f} distance {:.2f}",
                        d.mesh->name, static_cast<int>(d.sub - d.mesh->submeshes.data()), static_cast<int>(d.sub->pass), static_cast<int>(d.sub->kind),
                        d.hidden_views, d.center.x, d.center.y, d.center.z, d.radius, glm::distance(d.center, camera.position));
            }
        }
    }
    // 0x958530: every Mirror hides its own model in the capture views (+0x1AC bit 1), so the mirror view neither draws it nor
    // takes it as a shadow caster
    if (lighting.mirror_capture) {
        for (Draw& d : draws_) {
            for (const SceneMirror& mirror : lighting.mirrors) {
                if (d.mesh == mirror.mesh && d.transform == mirror.transform) {
                    d.hidden_views |= 2u;
                }
            }
        }
    }

    mirror_active_ = false;
    mirror_stale_active_ = false;
    mirror_temporal_enabled_ = up_.enabled && !std::getenv("PT_MIRROR_TEMPORAL_OFF");
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
    if (toggles.mirrors && lighting.mirror_capture) {
        // 0x956D10: the capture follows the Mirror whose model origin is nearest to the camera; the port renders it only while
        // that mirror faces the camera within 12 m (a capture nobody sees changes nothing on screen)
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
            // 0x959870 (camera +0x150 the near distance, +0xF8 the focal length): the capture view looks from the camera position
            // mirrored at the plane through the mirror model's origin (normal its -Z), moved to 5.5 m (High, 0x1422E50) or 0.5 m
            // (Low) from the origin when it is farther, at the origin, with a square field of view whose half angle holds the
            // corners of the model box's x and y extents around the origin (focal length = distance / corner radius) and the
            // near plane through the origin, square to the view; no clip plane (the capture's draws enable no user clip planes,
            // PA_CL_CLIP_CNTL 0x00080000). The view is 512x512 (MirrorCapture) or 128x128 (MirrorCaptureLow, 0x959070) at the
            // top left of the targets, and the mirror reads it through its projection (ProjViewCol). mirror_f060's mirror draws
            // (4950 op 2248, 5335 op 4789) give the eye at the mirrored camera within 0.1 mm, the axis through the origin and
            // a corner radius of 0.4872 m, the port's half diagonal of the mirror mesh's bounds
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
            // the capture's own history (RecordMirrorTemporal) averages the same patterns as the upscaler (View temporal)
            if (mirror_temporal_enabled_) {
                capture.temporal = frame_->views[main_view_.index].temporal;
            }
            // the depth to view distance of the lighting passes takes the view's own near distance
            capture.projection_param.z = eye_distance;
            mirror_view_.area = VkExtent2D{size, size};
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
    // no capture this frame: the last one's square (still in mirror_) through the last capture camera, for that mirror only
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

    // PT_LIGHT_CULL_LEGACY=1: the port's former stand-in for A/B tests (every OccluderEx tested from the eye with no
    // selection, and BuildShadowViews' hysteresis and 4 + 8 limits); otherwise the original's occluder stage below
    static const bool legacy_cull = [] {
        const char* s = std::getenv("PT_LIGHT_CULL_LEGACY");
        return s && std::string_view(s) == "1";
    }();
    legacy_light_cull_ = legacy_cull;
    // PT_LIGHT_CULL_TRACE=1: which lights the frame's own rules drop and why, and what became of each occluder. A light
    // that comes and goes while the player walks forward and back is one of these tests flipping, and the reason names it.
    // PT_LIGHT_CULL_TRACE=every: each rendered frame (a headless run renders only its screenshot frames)
    static const char* trace_cull_env = std::getenv("PT_LIGHT_CULL_TRACE");
    static const bool trace_cull = trace_cull_env != nullptr;
    static const bool trace_cull_every = trace_cull && std::string_view(trace_cull_env) == "every";
    const bool trace_cull_frame = trace_cull && (trace_cull_every || frame_counter_ % 60 == 0);
    static const bool light_changes = std::getenv("PT_LIGHT_CHANGES") != nullptr;
    main_occluders_ = {};
    mirror_occluders_ = {};
    if (!legacy_cull) {
        // 0xD53AC0 runs its occluder stage (0xD29E90, 0xD2A100 per OccluderEx, 0xD2AC10) for every frustum query, so for
        // the camera view and for the mirror capture view on their own (rendering.md 12.23)
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
        // The view's query (0xCFFED0 on the scene grid) holds the lights whose box, filed in whole grid cells, is not
        // wholly outside one of the query's six planes (lightcull::GridBoxInFrustum), less the boxes an occluder volume of
        // the view holds. The grid's origin and cell size are set at run time by code not decompiled yet: without
        // PT_LIGHT_GRID the port tests the box itself against the view's four side planes, a stand-in. With a mirror
        // capture the port's one light list serves both views, so a light stays when either view keeps it.
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
        // 0xD553A0 drops a light whose intensity sum (pi times these, the GrLight value) times the exposure is below 0.01
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
        // range.w: the spot receiver's offset along the view's z (viewBias * 0.001, 0xDC07F0; SpotShadow in lighting.glsl)
        g.range = glm::vec4(l.dimmer, l.inner_range, 1.0f / std::max(outer * outer * outer * outer, 1.0e-12f),
                            ShadowLegacy() ? 0.0f : l.view_bias);
        g.cone = spot ? glm::vec4(l.cos_outer, l.inv_cone_range, l.cone_exponent, l.dimmer) : glm::vec4(0.0f, 0.0f, 0.0f, -1.0f);
        g.shadow_cone = glm::vec4(l.shadow_cos_outer, l.shadow_inv_cone_range, 0.0f, 0.0f);
        g.scales = glm::vec4(l.specular_scale * lod.x, l.diffuse_scale * lod.y, l.shadow_strength * lod.z, l.cast_shadow ? 1.0f : 0.0f);
        const bool own_mask = l.masked && l.mask_texture >= 0;
        g.info = glm::ivec4(-1, l.masked && !own_mask ? static_cast<int>(gpu::kResMask) : -1,
                            l.has_area ? static_cast<int>(l.area_clip_mode) : 0, own_mask ? l.mask_texture + 1 : 0);
        g.area = l.has_area ? l.area_to_box : glm::mat4(0.0f);
        if (l.masked) {
            const glm::vec3 dir = glm::normalize(l.direction);
            const glm::vec3 up = std::abs(glm::dot(dir, glm::normalize(l.up))) < 0.95f ? glm::normalize(l.up) : AnyPerpendicular(dir);
            glm::mat4 flip(1.0f);
            flip[1][1] = -1.0f;
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
    // PT_PROBE_AT="x y z nx ny nz": the probes that reach a world point, in drawing order, with their box weight there and their
    // irradiance for the normal (before the exposure), and the blend probe.frag makes of them
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
    // PT_LIGHT_CHANGES=1: each frame where a light enters or leaves the frame, or gains or loses its shadow, with the eye and
    // the light's diffuse scale, so a light that comes and goes while the player walks names itself at the frame it happens
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
    // SelectionLocalLight (0xD553A0) ranks the lights of the view's own list, so a light the view's query left out has
    // no shadow there: the port's one light list holds the lights of both views while a mirror capture runs
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
            // PT_LIGHT_CULL_LEGACY=1 only: a port rule, not the original's (0xD553A0 ranks every frame afresh). A light the
            // view shadowed last frame keeps its shadow until another scores 25 % more.
            if (legacy_light_cull_ && view.view_bit == 1 && previous_shadowed_.contains(src.name)) {
                score *= 1.25f;
            }
            // the MirrorLight's shadow is the mirror view's whichever view lights with it: that view's casters leave the Mirror
            // models out, so the light stands behind the wall and shines through the opening the mirror closes
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
    // The view's pair (params +0x84, +0x88) from 0xD3F0C0: the capture worker's hook on 0xD553A0 reads (3, 5) for the game's
    // view in every frame of lightsel_f060 and lightsel_f010 (12.5), so at most 3 lights below priority 0x80 and 8 in all;
    // (2, 2) for the mirror capture views. PT_LIGHT_CULL_LEGACY=1 keeps the port's former 4 and 12. With the view byte +0x5D4,
    // which the floor environment sets on f110 (maze A and B, ScreenSettings::wide_shadow_limit), the pair is (10, 5).
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
        if (++trace_frame % 120 == 0) {
            LogInfo("shadow selection: eye ({:.3f} {:.3f} {:.3f}) RT {}", eye.x, eye.y, eye.z, rt_active_);
            for (const Candidate& c : ranked) {
                const bool selected = std::any_of(candidates.begin(), candidates.end(), [&](const Candidate& s) { return s.light == c.light; });
                LogInfo("shadow selection: {} priority {} score {:.5f} strength {:.3f} selected {}", light_sources_[c.light]->name,
                        c.priority, c.score, frame_->lights[c.light].scales.z, selected);
            }
        }
    }
    if (rt_active_) {
        // ray traced shadows (12.21): the same lights, no maps; info.x holds the light's caster set (the mirror view's for a
        // light only that view has, as RecordShadows picks it), the ray culling flag and the ray count
        const uint32_t samples = raytracing.soft_shadows ? (2u << std::clamp(graphics.ray_quality,0,2)) : 1u;
        for (const Candidate& c : candidates) {
            const uint32_t casters = light_sources_[c.light]->id == kMirrorLightId ? 32u : c.view_bit;
            frame_->lights[c.light].info.x = static_cast<int>(casters | (rt_cull_ << 8) | (samples << 16));
        }
        return;
    }
    // the tiles go by score: a spot takes one tile, a point light two (its paraboloid halves), as the original's targets
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
            // shadow_f010_fix: no shadow draw enables a polygon offset; the receiver offsets (shadowBias along the light's
            // axis, viewBias along the view's z) keep lit surfaces out of their own shadow. PT_SHADOW_LEGACY=1 keeps the
            // port's former slope scaled offset.
            sv.bias_constant = ShadowLegacy() ? -1.5f : 0.0f;
            sv.bias_slope = ShadowLegacy() ? -2.0f : 0.0f;
            // shadow_f010 trace: shadow draws cull front faces (PA_SU_SC_MODE_CNTL 0x245), double-sided materials none (0x244)
            sv.cull = VK_CULL_MODE_FRONT_BIT;
            shadow_views_.push_back(sv);
            // 0xDC07F0's texture matrix (0x1E91370) maps clip to (0.5 x + 0.50024414, -0.5 y + 0.50024414): the lookup sits
            // half a texel (1/4096 of the 2048 map) past the plain mapping, 1/n in clip space for a tile of n texels
            const float half_texel = ShadowLegacy() ? 0.0f : 1.0f / static_cast<float>(shadow_tile);
            g.shadow = glm::translate(glm::mat4(1.0f), glm::vec3(half_texel, half_texel, 0.0f)) * v.view_projection;
            g.shadow_rect = glm::vec4(tx * tile_uv, ty * tile_uv, tile_uv, tile_uv);
        } else {
            const float n = static_cast<float>(shadow_tile);
            const float r = src->outer_range;
            const float k = r * n / (n - 2.0f);
            const glm::mat4 rotation = glm::lookAt(pos, pos + glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
            const glm::mat4 light_space = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, -1.0f)) * rotation;
            for (int side = 0; side < 2; ++side) {
                gpu::View& v = frame_->views[view_count_];
                v = gpu::View{};
                v.view = light_space;
                v.projection_param = glm::vec4(1.0f, 1.0f, 0.05f, 2.0f);
                v.eye = glm::vec4(pos, side == 0 ? -1.0f : 1.0f);
                v.shadow = glm::vec4(1.0f / k, 0.0f, 0.0f, 0.0f);
                ShadowView sv;
                sv.view = view_count_++;
                sv.light = c.light;
                sv.casters = c.view_bit;
                sv.rect = {{static_cast<int32_t>((tx + side) * shadow_tile), static_cast<int32_t>(ty * shadow_tile)}, {shadow_tile, shadow_tile}};
                sv.center = pos;
                sv.radius = r;
                sv.frustum = false;
                // no polygon offset (shadow_f010_fix, 86 paraboloid draws); the lookup's receiver offset is shadowBias along
                // the view's z (section 4.5)
                sv.bias_constant = ShadowLegacy() ? -1.5f : 0.0f;
                sv.bias_slope = ShadowLegacy() ? -2.0f : 0.0f;
                // Both halves share the lookup's x = -d.x, so one of them is a mirror image of a camera looking along its
                // axis: here the lower half (side 1, d.z > 0). Front faces culled on the upper half and back faces on the lower
                // cull the faces toward the light in both, as the original's 0x245 on one half and 0x246 on the other (shadow_f010).
                sv.cull = side == 0 ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
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
    // The froxel volume spans view depths 1 m to far (0xC79530, called by 0x8EF500 with the fog's far and 1.0: slice s at
    // 1 m x (far / 1 m)^(s / 127), far raised to 10 m at least); the composite and the Prim shaders clamp the slice, so a pixel
    // or particle takes the fog of the depth clamp(z, 1, far). ending_flare_trace's m_fogParam[2] = (1.0183, 55.155, 0, 127)
    // at its 3470 to 3545, while the demo's far is below 10 (it is 0 from demo frame 3989 and ramps from 4200): 127 / 55.155 =
    // ln 10.
    frame_->fog[0] = glm::vec4(tpp.fog_density, tpp.fog_falloff, tpp.fog_near, std::max(tpp.fog_far, 10.0f));
    frame_->fog[1] = glm::vec4(tpp.fog_self * e, 1.0f);
    frame_->fog[2] = glm::vec4(tpp.fog_mie, tpp.fog_mie_anisotropy);
    // 0x906330: m0.w = e dirLightGain |dir|, the weight of the directional light's in-scatter (the shaders' ray.w)
    frame_->fog[3] = glm::vec4(tpp.fog_rayleigh, e * tpp.dir_gain * glm::length(tpp.dir_color));
    // fog[4..6] are unused: the area box term of VolFog_TppVolFog_Area never reaches the volume (compose.frag)
    frame_->fog[4] = glm::vec4(0.0f);
    frame_->fog[5] = glm::vec4(0.0f);
    frame_->fog[6] = glm::vec4(0.0f);
    frame_->fog[7] = glm::vec4(tpp.light_dir, 0.0f);
}

void SceneRenderer::UploadFrame(FrameSlot& slot) {
    uint8_t* base = static_cast<uint8_t*>(slot.frame_data.mapped);
    std::memcpy(base + offsetof(gpu::FrameData, fog), frame_->fog, sizeof(frame_->fog));
    std::memcpy(base + offsetof(gpu::FrameData, mirror), &frame_->mirror, sizeof(frame_->mirror));
    std::memcpy(base + offsetof(gpu::FrameData, views), frame_->views, sizeof(gpu::View) * view_count_);
    std::memcpy(base + offsetof(gpu::FrameData, lights), frame_->lights, sizeof(gpu::Light) * light_upload_count_);
    std::memcpy(base + offsetof(gpu::FrameData, probes), frame_->probes, sizeof(gpu::Probe) * probe_count_);
    vmaFlushAllocation(renderer_->Context().allocator, slot.frame_data.allocation, 0, VK_WHOLE_SIZE);
    if (!skin_matrices_.empty()) {
        std::memcpy(slot.skin.mapped, skin_matrices_.data(), sizeof(glm::mat4) * skin_matrices_.size());
        vmaFlushAllocation(renderer_->Context().allocator, slot.skin.allocation, 0, sizeof(glm::mat4) * skin_matrices_.size());
    }
}

void SceneRenderer::BindSets(VkCommandBuffer cmd, VkPipelineBindPoint point) {
    const FrameSlot& slot = slots_[renderer_->FrameIndex()];
    VkDescriptorSet sets[2] = {textures_->Set(), post_bindings_ ? slot.post_set : slot.set};
    vkCmdBindDescriptorSets(cmd, point, layout_, 0, 2, sets, 0, nullptr);
}

void SceneRenderer::DrawMesh(VkCommandBuffer cmd, const Draw& draw, uint32_t view, uint32_t flags, bool mirrored, bool no_cull,
                             const glm::vec4* aux, VkCullModeFlags cull, bool shadow_pass) {
    const SubMesh& sub = *draw.sub;
    const bool double_sided = shadow_pass ? sub.shadow_double_sided : sub.double_sided;
    vkCmdSetCullMode(cmd, double_sided || no_cull ? VK_CULL_MODE_NONE : cull);
    vkCmdSetFrontFace(cmd, mirrored ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &draw.mesh->vertices.buffer, &offset);
    vkCmdBindIndexBuffer(cmd, draw.mesh->indices.buffer, 0, VK_INDEX_TYPE_UINT32);
    gpu::DrawPush push;
    push.model = draw.transform;
    push.ids = glm::uvec4(view, draw.material, flags, sub.skinned ? draw.skin_base : gpu::kInvalid);
    push.tint = draw.tint;
    if (aux) {
        push.aux[0] = aux[0];
        push.aux[1] = aux[1];
    }
    PushConstants(cmd, &push, sizeof(push));
    vkCmdDrawIndexed(cmd, sub.index_count, 1, sub.first_index, sub.vertex_offset, 0);
}

void SceneRenderer::PushConstants(VkCommandBuffer cmd, const void* data, uint32_t size) {
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, size, data);
}

void SceneRenderer::Fullscreen(VkCommandBuffer cmd, VkPipeline pipeline, const gpu::PassPush& push) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    PushConstants(cmd, &push, sizeof(push));
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void SceneRenderer::RecordRayTracing(VkCommandBuffer cmd) {
    if (rt_built_frame_ == frame_counter_) {
        return;
    }
    rt_built_frame_ = frame_counter_;
    // PT_RT_SKINNED=0 leaves the skinned casters out (tests)
    static const bool skinned = [] {
        const char* env = std::getenv("PT_RT_SKINNED");
        return !env || std::atoi(env) != 0;
    }();
    // the shadow pass's casters (RecordShadows): every submesh with the shadow flag, mask bits 0 and 1 for the views that draw it;
    // with the reflections also the camera view's G-buffer surfaces, mask bit 2
    rt_casters_.clear();
    for (const Draw& d : draws_) {
        if (!skinned && d.sub->skinned && d.skin_base != gpu::kInvalid) {
            continue;
        }
        uint32_t mask = 0;
        if (d.sub->shadow) {
            mask |= ((d.hidden_views & 1u) == 0 ? 1u : 0u) | ((d.hidden_views & 2u) == 0 ? 2u : 0u);
            // bit 5: the mirror view's casters as the MirrorLight takes them
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
        // PT_SHADOW_NO_SKIN=1: no skinned casters (Lisa, the player's body) in either shadow path, so a shot with and one
        // without isolate the characters' shadows
        if (static const bool no_skin_shadow = std::getenv("PT_SHADOW_NO_SKIN") != nullptr; no_skin_shadow && d.sub->skinned) {
            mask &= ~3u;
        }
        // the G-buffer's surfaces and the forward pass's (glass, emissive, the mirror), which write depth too
        const bool deferred = d.sub->pass == RenderPass::Opaque && d.sub->kind == gpu::kKindDeferred;
        const bool forward = (d.sub->pass == RenderPass::Unlit || d.sub->pass == RenderPass::Transparent) && d.sub->kind != gpu::kKindShadowOnly;
        const bool surface = (deferred || forward) && (d.hidden_views & 1u) == 0;
        if (rt_reflections_ && surface) {
            mask |= 4u;
        }
        // the ambient occlusion's occluders (bit 3): the camera view's G-buffer surfaces; the contact shadows' (bit 4): the same
        // without the lights' own fittings, a mesh under 1 m across whose bounds hold one of the frame's lights (a lamp's shade
        // or glass, which the original's light shines through)
        if (deferred && (d.hidden_views & 1u) == 0) {
            if (rt_ao_active_) {
                mask |= 8u;
            }
            if (rt_contact_active_) {
                bool fitting = false;
                if (d.radius < 0.5f) {
                    for (const SceneLight* l : light_sources_) {
                        if (glm::distance(l->position, d.center) < d.radius + 0.02f) {
                            fitting = true;
                            break;
                        }
                    }
                }
                if (!fitting) {
                    mask |= 16u;
                }
            }
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
        rt_casters_.push_back(c);
    }
    BindSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE);
    rt_->Build(cmd, renderer_->FrameIndex(), rt_casters_);
    static const bool log_frames = std::getenv("PT_RT_LOG") != nullptr;
    if (log_frames) {
        const RtStats& s = rt_->Stats();
        std::string skinned_meshes;
        for (const RtCaster& c : rt_casters_) {
            if (c.skin_base != gpu::kInvalid) {
                skinned_meshes += std::format(" {}#{}(mask {})", c.mesh->name, c.submesh, c.mask);
            }
        }
        LogInfo("ray tracing: frame {}: {} instances, {} skinned ({} vertices), {} structures built, {} static BLAS so far;{}", frame_counter_,
                s.instances, s.skinned_instances, s.skinned_vertices, s.built_this_frame, s.static_blas, skinned_meshes);
    }
}

void SceneRenderer::RecordShadows(VkCommandBuffer cmd) {
    if (rt_active_ || rt_reflections_ || rt_ao_active_ || rt_contact_active_) {
        RecordRayTracing(cmd);
    }
    if (rt_active_) {
        return;
    }
    if (shadow_views_.empty()) {
        return;
    }
    // tiles fill the atlas row by row: clear only the rows in use
    uint32_t rows = 0;
    for (const ShadowView& sv : shadow_views_) {
        rows = std::max(rows, static_cast<uint32_t>(sv.rect.offset.y) + sv.rect.extent.height);
    }
    UseTargets(cmd, {{&shadow_, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL}});
    BeginPass(cmd, {shadow_.Extent().width, rows}, {}, &shadow_, false, true);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_pipeline_);
    for (const ShadowView& sv : shadow_views_) {
        SetViewport(cmd, sv.rect);
        vkCmdSetDepthBias(cmd, sv.bias_constant, 0.0f, sv.bias_slope);
        // The camera view's lights take the casters that view draws (guess: the light views were not traced); a light only the
        // mirror view has (the MirrorLight) takes the mirror view's, as the capture views render their own shadow maps (0xD3F0C0)
        const uint8_t casters = sv.casters;
        uint32_t shadow_draws = 0, shadow_indices = 0;
        for (const Draw& d : draws_) {
            if (!d.sub->shadow || (d.hidden_views & casters) != 0) {
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
            LogInfo("shadow pass: {} casters {:#x} draws {} indices {} at ({:.3f} {:.3f} {:.3f})", light_sources_[sv.light]->name, sv.casters, shadow_draws, shadow_indices, sv.center.x, sv.center.y, sv.center.z);
        }
    }
    vkCmdEndRendering(cmd);
    UseTargets(cmd, {{&shadow_, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}});
}

void SceneRenderer::RecordGBuffer(VkCommandBuffer cmd, const ViewSetup& view) {
    UseTargets(cmd, {{&albedo_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                     {&normal_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                     {&material_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                     {&depth_, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL}});
    BeginPass(cmd, ViewArea(view), {{&albedo_, true, {}}, {&normal_, true, {}}, {&material_, true, {}}}, &depth_, false, true);
    uint32_t debug_flags = 0;
    if (debug_view == DebugView::UV0) {
        debug_flags = 2;
    } else if (debug_view == DebugView::VertexColor) {
        debug_flags = 3;
    }
    const glm::vec4 aux[2] = {glm::vec4(0.0f), glm::vec4(0.0f, 0.0f, 0.0f, static_cast<float>(reflection_cube_))};
    for (int pass = 0; pass < 2; ++pass) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pass == 0 ? gbuffer_ : decal_);
        if (pass == 1) {
            vkCmdSetDepthBias(cmd, 1.0f, 0.0f, 1.0f);
        }
        const RenderPass wanted = pass == 0 ? RenderPass::Opaque : RenderPass::Decal;
        // decal meshes carry a layer in mesh flag bits 20 to 23 (the ending street's blood is layer 2 to 4 over the layer 0
        // wet asphalt of the road placements, which come later in the draw list): lower layers blend first
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
    vkCmdEndRendering(cmd);
}

void SceneRenderer::RecordOcclusion(VkCommandBuffer cmd, const ViewSetup& view) {
    UseTargets(cmd, {{&depth_, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}, {&ao_[0], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}});
    BeginPass(cmd, extent_, {{&ao_[0], false, {}}});
    gpu::PassPush push;
    push.ids = glm::uvec4(view.index, 0, 0, 0);
    push.f0 = glm::vec4(3.0f, 1.0f, 0.0f, 0.0f);
    push.f1 = glm::vec4(1.0f, 4.0f, 1.0f, 0.25f);
    // GrxLineSSAOParameters gainonStart 0.85, gainonRange 5, falloffRange 150 (0xE21CD0 copies them to the plugin, 0xDA39F0 makes
    // the constants); the fade ends at min(far clip, falloffStart 100 + falloffRange), 100 m in play, so near the camera the
    // occlusion keeps two thirds of its strength
    push.f2 = glm::vec4(1.0f / 5.0f, 0.85f, std::min(camera_.far_plane, 100.0f + 150.0f), 1.0f / 150.0f);
    Fullscreen(cmd, occlusion_, push);
    vkCmdEndRendering(cmd);
    UseTargets(cmd, {{&ao_[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {&ao_[1], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}});
    BeginPass(cmd, extent_, {{&ao_[1], false, {}}});
    push.f0 = glm::vec4(0.45f, 0.0f, 0.0f, 0.0f);
    Fullscreen(cmd, occlusion_blur_, push);
    vkCmdEndRendering(cmd);
    UseTargets(cmd, {{&ao_[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
}

bool SceneRenderer::EnsureAoTargets() {
    if (rt_ao_images_[0].Valid() && rt_ao_images_[0].Extent().width == extent_.width && rt_ao_images_[0].Extent().height == extent_.height) {
        return true;
    }
    vk::Context& ctx = renderer_->Context();
    // the sets of the frames in flight get new descriptors
    vkDeviceWaitIdle(device_);
    for (RenderTarget& t : rt_ao_images_) {
        ctx.DestroyImage(t.image);
        t = RenderTarget{};
    }
    const VkFormat formats[RayTracing::kAoImages] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
                                                     VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_SFLOAT};
    VkImageView views[RayTracing::kAoImages]{};
    for (uint32_t i = 0; i < RayTracing::kAoImages; ++i) {
        if (!CreateTarget(rt_ao_images_[i], formats[i], extent_, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT)) {
            LogError("ray tracing: AO image creation failed at {}x{}", extent_.width, extent_.height);
            for (RenderTarget& t : rt_ao_images_) {
                ctx.DestroyImage(t.image);
                t = RenderTarget{};
            }
            return false;
        }
        views[i] = rt_ao_images_[i].image.view;
    }
    ctx.Submit([&](VkCommandBuffer cmd) {
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        for (uint32_t i = 0; i < RayTracing::kAoImages; ++i) {
            UseTargets(cmd, {{&rt_ao_images_[i], VK_IMAGE_LAYOUT_GENERAL}});
            // no occlusion, and histories without frames
            const VkClearColorValue value = (i == 1 || i == 2) ? VkClearColorValue{} : VkClearColorValue{{1.0f, 1.0f, 1.0f, 1.0f}};
            vkCmdClearColorImage(cmd, rt_ao_images_[i].image.image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
        }
        UseTargets(cmd, {{&rt_ao_images_[0], VK_IMAGE_LAYOUT_GENERAL}});
    });
    rt_->SetAoImages(views);
    rt_ao_history_ = false;
    LogInfo("ray tracing: ambient occlusion images {}x{}", extent_.width, extent_.height);
    return true;
}

// Ray traced ambient occlusion (12.21) of the main view: rt_ao.comp traces, rt_ao_filter.comp accumulates and blurs; the
// lighting pass then draws the probes with probe_ao.frag, which reads the result
void SceneRenderer::RecordRtAmbientOcclusion(VkCommandBuffer cmd, const ViewSetup& view) {
    const gpu::View& v = frame_->views[view.index];
    // a camera cut starts the history again
    const bool cut = glm::distance(rt_ao_previous_eye_, view.eye) > kCameraCutDistance;
    const uint32_t previous = rt_ao_written_;
    gpu::PassPush push;
    push.ids = glm::uvec4(view.index, static_cast<uint32_t>(frame_counter_ & 0xFFFFFFu), std::getenv("PT_RT_AO_RAYS")?rt_ao_rays_:2u << std::clamp(graphics.ray_quality,0,2), 0);
    push.f0 = glm::vec4(rt_ao_reach_, rt_ao_frames_, 0.0f, 0.0f);
    push.m = rt_ao_previous_;
    BindSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE);
    const VkDescriptorSet rt_set = rt_->Set(renderer_->FrameIndex());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rt_->Layout(), 2, 1, &rt_set, 0, nullptr);
    const uint32_t groups_x = (extent_.width + 7) / 8;
    const uint32_t groups_y = (extent_.height + 7) / 8;
    UseTargets(cmd, {{&depth_, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}, {&normal_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rt_ao_trace_);
    PushConstants(cmd, &push, sizeof(push));
    vkCmdDispatch(cmd, groups_x, groups_y, 1);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rt_ao_filter_);
    for (uint32_t pass = 0; pass < 3; ++pass) {
        // each pass reads what the one before wrote
        UseTargets(cmd, {{&rt_ao_images_[0], VK_IMAGE_LAYOUT_GENERAL}, {&rt_ao_images_[1], VK_IMAGE_LAYOUT_GENERAL},
                         {&rt_ao_images_[2], VK_IMAGE_LAYOUT_GENERAL}, {&rt_ao_images_[3], VK_IMAGE_LAYOUT_GENERAL}});
        push.ids.z = pass;
        push.ids.w = previous | (rt_ao_history_ && !cut ? 0u : 2u);
        PushConstants(cmd, &push, sizeof(push));
        vkCmdDispatch(cmd, groups_x, groups_y, 1);
    }
    UseTargets(cmd, {{&rt_ao_images_[4], VK_IMAGE_LAYOUT_GENERAL}});
    BindSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS);
    rt_ao_written_ = previous ^ 1u;
    rt_ao_history_ = true;
    rt_ao_previous_ = v.view_projection;
    rt_ao_previous_eye_ = view.eye;
    rt_ao_ready_ = true;
}

void SceneRenderer::RecordLighting(VkCommandBuffer cmd, const ViewSetup& view) {
    const VkCullModeFlags cull = view.mirrored ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_FRONT_BIT;
    const bool probes = toggles.probes && probe_count_ > 0;
    const bool lights = toggles.lights && debug_view != DebugView::Ambient;
    const VkExtent2D area = ViewArea(view);
    // The SH resolve (rendering.md 5): SSLighting2_SH_MultiBlend accumulates the probes at half resolution (isShrinkSHBuffer,
    // 960x540 of the 1080p frame), SSLighting2_SH_SkyLight_Encode divides the sum by its coverage 1 - T where T <= 0.9 (else the
    // atlas's cell 0, black in P.T.) and BilateralUpscale2x2 takes it to full resolution by depth. With a temporal upscaler (and
    // with the ray traced ambient occlusion, whose result is per render pixel) the probes accumulate at render resolution and
    // the resolve only normalizes: the fixed 2x2 grid of the half resolution pass would not move with the jitter.
    // PT_SH_RESOLVE=legacy draws the probes straight into the diffuse target without the normalization, as before.
    static const bool legacy_resolve = [] {
        const char* v = std::getenv("PT_SH_RESOLVE");
        return v && std::string_view(v) == "legacy";
    }();
    const bool ao = rt_ao_ready_ && view.index == main_view_.index;
    const bool shrink = !up_.enabled && !ao && !legacy_resolve;
    const VkExtent2D acc_area{shrink ? (area.width + 1) / 2 : area.width, shrink ? (area.height + 1) / 2 : area.height};
    auto draw_probes = [&](uint32_t shrink_flag) {
        if (ao) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, probe_ao_);
            const VkDescriptorSet rt_set = rt_->Set(renderer_->FrameIndex());
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rt_->Layout(), 2, 1, &rt_set, 0, nullptr);
        } else {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, probe_);
        }
        vkCmdSetCullMode(cmd, cull);
        for (uint32_t i = 0; i < probe_count_; ++i) {
            const SceneProbe& p = lighting_->probes[probe_order_[i]];
            gpu::PassPush push;
            push.ids = glm::uvec4(view.index, i, shrink_flag, 0);
            push.m = p.box_world;
            PushConstants(cmd, &push, sizeof(push));
            vkCmdDraw(cmd, 36, 1, 0, 0);
        }
    };
    if (probes) {
        UseTargets(cmd, {{&normal_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                         {&depth_, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                         {&probe_acc_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}});
        VkClearColorValue transmittance{};
        transmittance.float32[3] = 1.0f;
        BeginPass(cmd, acc_area, {{&probe_acc_, true, transmittance}});
        draw_probes(shrink ? 1u : 0u);
        vkCmdEndRendering(cmd);
        UseTargets(cmd, {{&probe_acc_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
    }
    UseTargets(cmd, {{&albedo_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                     {&normal_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                     {&material_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                     {&depth_, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                     {&diffuse_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                     {&specular_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}});
    BeginPass(cmd, area, {{&diffuse_, true, {}}, {&specular_, true, {}}}, &depth_, true, false);
    if (probes) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, probe_resolve_);
        gpu::PassPush push;
        // ids.x: 0 the accumulation's own pixel, 1 the bilateral upscale from half resolution, 2 legacy (no normalization)
        push.ids = glm::uvec4(legacy_resolve ? 2u : (shrink ? 1u : 0u), acc_area.width, acc_area.height, view.index);
        PushConstants(cmd, &push, sizeof(push));
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    // the contact shadows are the main view's (their occluders are its surfaces)
    const bool contact = rt_contact_active_ && view.index == main_view_.index;
    if (lights && light_count_ > 0) {
        if (rt_active_ || contact) {
            // sets 0 and 1 stay bound: the ray tracing layout is compatible with the scene layout up to set 1
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rt_active_ ? light_rt_ : light_contact_);
            const VkDescriptorSet rt_set = rt_->Set(renderer_->FrameIndex());
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rt_->Layout(), 2, 1, &rt_set, 0, nullptr);
        } else {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, light_);
        }
        vkCmdSetCullMode(cmd, cull);
        for (uint32_t i = 0; i < light_count_; ++i) {
            const SceneLight& l = *light_sources_[i];
            if (const char* only = std::getenv("PT_LIGHT_DRAW_ONLY"); only && l.name.find(only) == std::string::npos) {
                continue;
            }
            if ((l.hidden_views & view.view_bit) != 0) {
                continue;
            }
            const float radius = l.outer_range * 1.02f + 0.05f;
            glm::mat4 volume = glm::translate(glm::mat4(1.0f), l.position) * glm::scale(glm::mat4(1.0f), glm::vec3(radius));
            if (l.has_area) {
                const glm::mat3 box(l.area_world);
                const glm::vec3 half(glm::length(box[0]), glm::length(box[1]), glm::length(box[2]));
                if (half.x * half.y * half.z < radius * radius * radius) {
                    volume = l.area_world;
                }
            }
            gpu::PassPush push;
            const uint32_t mirror_light = mirror_shadow_lights_[i];
            const bool has_shadow = view.view_bit == 2 ? mirror_light != gpu::kInvalid : (light_shadow_views_[i] & view.view_bit) != 0;
            const uint32_t light_index = view.view_bit == 2 && has_shadow ? mirror_light : i;
            push.ids = glm::uvec4(view.index, light_index, toggles.shadows && has_shadow ? 1u : 0u, 0);
            if (const char* unshadowed = std::getenv("PT_LIGHT_DRAW_NO_SHADOW"); unshadowed && l.name.find(unshadowed) != std::string::npos) {
                push.ids.z = 0;
            }
            push.f0.x = contact ? rt_contact_reach_ : 0.0f;
            push.f0.y = contact && rt_contact_legacy_ ? 1.0f : 0.0f;
            push.m = volume;
            PushConstants(cmd, &push, sizeof(push));
            vkCmdDraw(cmd, 36, 1, 0, 0);
        }
    }
    vkCmdEndRendering(cmd);
    UseTargets(cmd, {{&diffuse_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {&specular_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
}

void SceneRenderer::RecordLuminance(VkCommandBuffer cmd) {
    FrameSlot& slot = slots_[renderer_->FrameIndex()];
    const uint32_t gx = (extent_.width + 31) / 32;
    const uint32_t gy = (extent_.height + 31) / 32;
    if (gx * gy > 16384) {
        return;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, luminance_);
    BindSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE);
    gpu::PassPush push;
    PushConstants(cmd, &push, sizeof(push));
    vkCmdDispatch(cmd, gx, gy, 1);
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
    slot.luminance_groups = gx * gy;
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

// 0xD6BB90, 0xCC3530, 0xCC4FE0, 0xDB6C10
uint32_t SceneRenderer::ForwardLights(const Draw& draw, glm::vec4 aux[2], uint8_t view_bit) const {
    const glm::vec3 local_center = (draw.mesh->bounds_min + draw.mesh->bounds_max) * 0.5f;
    const glm::vec3 local_half = (draw.mesh->bounds_max - draw.mesh->bounds_min) * 0.5f;
    const glm::mat3 m(draw.transform);
    const glm::vec3 half = glm::abs(m[0]) * local_half.x + glm::abs(m[1]) * local_half.y + glm::abs(m[2]) * local_half.z;
    const glm::vec3 center = glm::vec3(draw.transform * glm::vec4(local_center, 1.0f));
    // The job's own box of the model: the local box centre plus the matrix translation (the rotation is left out of the centre,
    // so for the house, turned 90 degrees, that point lies outside the hallway), the local extents and the matrix rotation.
    // A probe counts when it holds that point (0x534440), a light with a lightArea when its area box meets that box (0x534960).
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
        // the lights whose grid box meets the model's box (0x534400)
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

// DominantLightSearch (ShEffet, 0x8D12A0, once a frame; rendering.md 3): the direction g_psSystem.m_dominantLightDir
// gives fox3DDF_Hair's highlight. It reads the render camera of MainViewport (GrCamera at view +0x4A8, rows right +0x30,
// up +0x40, forward +0x50, position +0x60) composed with a translation by (0, 0, 2) applied after the camera matrix, so the
// point P it works from is the camera position plus 2 m along world Z (not along the view). The box of the four points
// P + 0.25 forward +- 0.9 up +- 0.25 right goes to 0xCC6570: the scene's lights whose grid box meets it (0xCC30F0, all
// views, no light area test), scored at the box centre with half its largest extent as the radius (0xCC3530) and packed by
// 0xCC4FE0, whose first four enabled point or spot lights carry their colour / pi (a spot's times its cone^e at the centre).
// The strongest of those (|colour / pi| above the scene's directional light, which P.T. does not have, so above 0) gives
// the direction normalize(P - light position); with none it is (0, -1, 0). DominantLightState fades or turns it to a new
// result. The view constant takes it through the view's rotation (0xD8B8E0); every other view keeps the (0, 1, 0, 1) its
// constructor sets (0xCAC3D0, 0xCAD110), the mirror captures among them. dt is the game time since the last render.
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

// 0x958790, the mirror material's lights. With the handy light on, LocalLightMask (0, 0, 0, 1) keeps only the LocalLight:
// the handy light, its colour scaled by its cone at the point of the mirror's box nearest to where the light's axis line
// crosses the mirror plane (0x543DF0 intersects the whole line; a parallel axis leaves the world origin), with its own
// position, dimmer, inner range and 1/R^4. Otherwise (1, 1, 1, 0) keeps the model's light block as for glass. The hemisphere
// terms of the light block apply either way. The mirror's front normal is its model's -Z (0x95ADA0).
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

// The original draws its forward materials (fox3dfw_constant, fox3dfw_glass, the mirror) on the 8-bit scene buffer through a UNORM view
// with CB_BLEND 0x65000501 (colour src + dst (1 - src.a), alpha dst.a (1 - src.a); f010_dumps 1490, draws 567 to 575), so what lies behind
// translucent glass is mixed on sRGB encoded values. In the main view the port draws them into the effect buffer instead, cleared to
// (0, 0, 0, 1): the encoded premultiplied colours chain there as on the scene buffer (P = src + P (1 - a), T = T (1 - a)), and
// vfx_composite then sets scene = decode(P + T encode(scene)) with the scene alpha times T, the result of the same blends in the same
// order. The mirror view keeps the linear blend.
void SceneRenderer::RecordForward(VkCommandBuffer cmd, const ViewSetup& view, RenderTarget& output) {
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
        vkCmdEndRendering(cmd);
        UseTargets(cmd, {{&particles_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, {&depth_, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL}});
        BeginPass(cmd, extent_, {{&particles_, true, {{0.0f, 0.0f, 0.0f, 1.0f}}}}, &depth_, false, false);
    }
    VkPipeline bound = VK_NULL_HANDLE;
    for (const Draw& d : draws_) {
        const RenderPass pass = d.sub->pass;
        if ((pass != RenderPass::Unlit && pass != RenderPass::Transparent) || d.sub->kind == gpu::kKindShadowOnly || !Visible(d, view)) {
            continue;
        }
        const bool emissive = d.sub->kind == gpu::kKindConstant || d.sub->kind == gpu::kKindSky;
        const VkPipeline pipeline = emissive ? forward_emissive_ : forward_;
        if (pipeline != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            bound = pipeline;
        }
        // forward.frag: write the encoded premultiplied colour for the effect buffer
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
                // 0x958530 hides the mirror's own model in the capture views (+0x1AC bit 1)
                if (!main_view) {
                    continue;
                }
                flags |= 1u;
            }
            flags |= MirrorLights(d, aux) << 8;
            // PT_MIRROR_DIRT=0|1: the mirror's blend forced to the reflection alone (0) or the lit dirt alone (1), for
            // the measurement that tells which of the two is short of the original (forward.frag's flags bit 1 and 2)
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
            // PT_MIRROR_RAW_REFLECTION=1: the capture sampled without the sRGB encode the material applies to it
            static const bool raw_reflection = std::getenv("PT_MIRROR_RAW_REFLECTION") != nullptr;
            if (raw_reflection) {
                flags |= 8u;
            }
            // PT_MIRROR_LINEAR_DIRT=1: the mirror's dirt taken as a linear colour instead of as the encoded value the
            // shader comment describes. The port applies SrgbDecode to it while the comment says the original writes the
            // lit dirt into the gamma buffer without a curve; the two readings differ by the decode, which is the size
            // of the mirror's excess over the capture (scratch/mirror-region/FINDINGS.md).
            static const bool linear_dirt = std::getenv("PT_MIRROR_LINEAR_DIRT") != nullptr;
            if (linear_dirt) {
                flags |= 16u;
            }
            // A mirror whose lit dirt never changes with the frame's lights needs these two checked: the parallax branch
            // is what gives it the light block, and a zero aux leaves the shader's `lit` term at zero.
            static const bool trace_mirror_draw = std::getenv("PT_MIRROR_TRACE") != nullptr;
            // PT_MIRROR_TRACE=every: each rendered frame
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
    vkCmdEndRendering(cmd);
    UseTargets(cmd, {{&output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL}, {&hdr_copy_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL}});
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = copy.srcSubresource;
    copy.extent = {extent_.width, extent_.height, 1};
    vkCmdCopyImage(cmd, output.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hdr_copy_.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &copy);
    UseTargets(cmd, {{&output, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                     {&hdr_copy_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                     {&particles_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
    BeginPass(cmd, extent_, {{&output, false, {}}});
    gpu::PassPush push;
    // vfx_composite: the forward blends also take the scene alpha (the composite's bloom luminance) down by the transmittance
    push.ids.x = 1u;
    Fullscreen(cmd, vfx_composite_, push);
    vkCmdEndRendering(cmd);
    UseTargets(cmd, {{&output, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, {&depth_, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL}});
    BeginPass(cmd, extent_, {{&output, false, {}}}, &depth_, false, false);
}

// the liquid materials' scene copy (Primitive_Liquid2Final inScreenTexture): the forward pass ends, the HDR target is copied to
// hdr_copy_ and the pass resumes with the same attachments
// The original downsamples reverse-Z depth to RG16F min/max, draws alpha/additive/subtractive effects into two 960x540 RGBA16F
// premultiplied layers, then resolves them at full resolution with NearFarUpScale2x2. Called inside the effect pass on output; leaves
// that pass open again.
void SceneRenderer::RecordEffects(VkCommandBuffer cmd, RenderTarget& output, SceneVfxContext context) {
    vkCmdEndRendering(cmd);
    const VkExtent2D effect_extent = particles_near_.Extent();
    UseTargets(cmd, {{&depth_, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                     {&near_far_depth_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                     {&near_far_depth_attachment_, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL}});
    BeginPass(cmd, effect_extent, {{&near_far_depth_, true, {}}}, &near_far_depth_attachment_, false, true);
    BindSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS);
    Fullscreen(cmd, vfx_depth_down_, gpu::PassPush{});
    vkCmdEndRendering(cmd);
    UseTargets(cmd, {{&near_far_depth_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                     {&near_far_depth_attachment_, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                     {&particles_near_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                     {&particles_far_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}});
    const VkClearColorValue clear_transmittance{{0.0f, 0.0f, 0.0f, 1.0f}};
    BeginPass(cmd, effect_extent, {{&particles_near_, true, clear_transmittance}, {&particles_far_, true, clear_transmittance}},
              &near_far_depth_attachment_, true, false);
    uint32_t recorded = 0;
    context.subset = kVfxOffscreen;
    context.recorded = &recorded;
    context.extent = effect_extent;
    context.depth_view = near_far_depth_attachment_.image.view;
    context.near_far_depth_view = near_far_depth_.image.view;
    // The liquids of the effect pass (the ones without the alpha test, VfxPass::DrawLayer) refract the scene as it stands under the
    // effect buffer, as the original copies its scene buffer before them; nothing in this pass changes the scene, so one copy
    // serves every one of them
    bool copied = false;
    context.copy_scene = nullptr;
    if (context.scene_copy_view) {
        context.copy_scene = [&] {
            if (copied) {
                return;
            }
            copied = true;
            vkCmdEndRendering(cmd);
            UseTargets(cmd, {{&output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL}, {&hdr_copy_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL}});
            VkImageCopy copy{};
            copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.dstSubresource = copy.srcSubresource;
            copy.extent = {extent_.width, extent_.height, 1};
            vkCmdCopyImage(cmd, output.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hdr_copy_.image.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            UseTargets(cmd, {{&hdr_copy_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                             {&particles_near_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                             {&particles_far_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                             {&near_far_depth_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                             {&near_far_depth_attachment_, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}});
            BeginPass(cmd, effect_extent, {{&particles_near_, false, {}}, {&particles_far_, false, {}}},
                      &near_far_depth_attachment_, true, false);
        };
    }
    vfx_forward(context);
    BindSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS);
    vkCmdEndRendering(cmd);
    if (recorded > 0) {
        UseTargets(cmd, {{&output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL}, {&hdr_copy_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL}});
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = copy.srcSubresource;
        copy.extent = {extent_.width, extent_.height, 1};
        vkCmdCopyImage(cmd, output.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hdr_copy_.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &copy);
        UseTargets(cmd, {{&output, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                         {&hdr_copy_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                         {&depth_, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                         {&near_far_depth_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                         {&particles_near_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                         {&particles_far_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
        BeginPass(cmd, extent_, {{&output, false, {}}});
        gpu::PassPush push;
        push.f0.x = context.projection[3][2];
        Fullscreen(cmd, vfx_composite_, push);
        vkCmdEndRendering(cmd);
    }
    UseTargets(cmd, {{&output, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, {&depth_, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}});
    BeginPass(cmd, extent_, {{&output, false, {}}}, &depth_, true, false);
}

void SceneRenderer::RecordSceneCopy(VkCommandBuffer cmd, RenderTarget& output) {
    vkCmdEndRendering(cmd);
    UseTargets(cmd, {{&output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL}, {&hdr_copy_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL}});
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = copy.srcSubresource;
    copy.extent = {extent_.width, extent_.height, 1};
    vkCmdCopyImage(cmd, output.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hdr_copy_.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &copy);
    UseTargets(cmd, {{&output, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                     {&hdr_copy_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                     {&depth_, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}});
    BeginPass(cmd, extent_, {{&output, false, {}}}, &depth_, true, false);
}

void SceneRenderer::RecordMirrorTemporal(VkCommandBuffer cmd) {
    const uint32_t size = mirror_view_.area.width;
    const glm::vec3 origin(mirrors_.front()->transform[3]);
    const float capture_exposure = frame_->views[mirror_view_.index].exposure.x;
    const bool valid_history = mirror_history_valid_ && !up_.reset && mirror_history_size_ == size &&
                               glm::distance(origin, mirror_history_origin_) < 0.001f;
    UseTargets(cmd, {{&mirror_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                     {&depth_, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                     {&mirror_history_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                     {&mirror_temporal_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}});
    BeginPass(cmd, mirror_view_.area, {{&mirror_temporal_, false, {}}});
    gpu::PassPush push;
    push.ids = glm::uvec4(valid_history ? 1u : 0u, 0, 0, mirror_view_.index);
    push.f0 = glm::vec4(size, size, 1.0f / extent_.width, 1.0f / extent_.height);
    push.f1 = glm::vec4(capture_exposure / std::max(mirror_history_exposure_, 1.0e-12f), 0.9f, 0.0f, 0.0f);
    push.m = mirror_previous_view_projection_;
    Fullscreen(cmd, mirror_temporal_pipeline_, push);
    vkCmdEndRendering(cmd);
    UseTargets(cmd, {{&mirror_temporal_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL},
                     {&mirror_history_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL},
                     {&mirror_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL}});
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = copy.srcSubresource;
    copy.extent = {size, size, 1};
    vkCmdCopyImage(cmd, mirror_temporal_.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mirror_history_.image.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    vkCmdCopyImage(cmd, mirror_temporal_.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mirror_.image.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    UseTargets(cmd, {{&mirror_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                     {&mirror_history_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
    mirror_previous_view_projection_ = mirror_current_view_projection_;
    mirror_history_exposure_ = capture_exposure;
    mirror_history_origin_ = origin;
    mirror_history_size_ = size;
    mirror_history_valid_ = true;
}

void SceneRenderer::RecordReflections(VkCommandBuffer cmd, const ViewSetup& view) {
    constexpr float kFarLimit = 2000.0f - 0.01f;
    constexpr float kLines = 1080.0f;
    const glm::mat3 rotation(frame_->views[view.index].view);
    const glm::vec3 plane = glm::normalize(rotation * glm::vec3(0.0f, 1.0f, 0.0f));
    // ReflectMapBlend's localParam[2] is the camera's own forward in view space ((0, 0, 1) in the original's convention,
    // sink_refl_cb_f060 4185 to 4195); the horizontal forward the port took (the original's localParam[3], which the blend
    // does not read) made the lean term 2 on a floor seen from above where the original's is 1: twice the reflection
    const glm::vec3 forward = camera_.Forward();
    const float length = glm::length(forward);
    const glm::vec3 horizontal = length > 1.0e-6f ? rotation * (forward / length) : glm::vec3(0.0f);
    const float aspect = static_cast<float>(extent_.width) / static_cast<float>(std::max(extent_.height, 1u));

    UseTargets(cmd, {{&normal_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                     {&depth_, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                     {&refmap_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}});
    gpu::PassPush push;
    push.ids = glm::uvec4(view.index, 0, 0, 0);
    push.f0 = glm::vec4(plane, kFarLimit);
    // PT_REFLECT_DEPTH_LEGACY=1: under a jittered view the march and the reflection layer interpolate the depth wherever the 2x2
    // texels lie within 5 % of each other, as before ReflectionPlanarDepth (reflection_mix.glsl), for A/B
    static const bool depth_legacy = std::getenv("PT_REFLECT_DEPTH_LEGACY") != nullptr;
    // PT_REFLECT_MARCH_SHIFT_FIRST=1: the per frame start shifts the first sample too, as before (A/B; reflect_make.frag)
    static const bool shift_first = std::getenv("PT_REFLECT_MARCH_SHIFT_FIRST") != nullptr;
    push.f1 = glm::vec4(kLines * aspect, kLines, depth_legacy ? 1.0f : 0.0f, shift_first ? 1.0f : 0.0f);
    push.f2 = glm::vec4(1.0f / static_cast<float>(refmap_.Extent().width), 1.0f / static_cast<float>(refmap_.Extent().height), 0.0f, 0.0f);
    // the coarse march's per frame start (reflect_make.frag) only where reflect_temporal.frag accumulates it; PT_REFLECT_MARCH_FIXED=1
    // keeps the original's fixed samples there too
    static const bool march_fixed = std::getenv("PT_REFLECT_MARCH_FIXED") != nullptr;
    static const bool temporal_off_march = std::getenv("PT_REFLECT_TEMPORAL_OFF") != nullptr;
    if (up_.enabled && !temporal_off_march && !march_fixed && reflect_layer_.Valid() && reflect_temporal_pipeline_ && reflect_layer_pipeline_) {
        push.f2.z = 1.0f;
        push.f2.w = static_cast<float>(frame_counter_ % 64u);
    }
    if (rt_reflections_) {
        // 12.21: the map traced, with a second target for the hits off screen; the lights hidden in the camera view as bits of m
        UseTargets(cmd, {{&refmap_color_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}});
        BeginPass(cmd, refmap_.Extent(), {{&refmap_, false, {}}, {&refmap_color_, false, {}}});
        // PT_RT_REFLECTION_DEBUG=1: the reflection shows the hits it shaded in red and those it took from the screen in green;
        // =2: every hit shaded at full weight, to compare the shading with the screen
        static const uint32_t debug = [] {
            const char* value = std::getenv("PT_RT_REFLECTION_DEBUG");
            return value ? (std::atoi(value) == 2 ? 2u : 1u) : 0u;
        }();
        push.ids = glm::uvec4(view.index, light_count_, probe_count_, debug);
        uint32_t hidden[16] = {};
        for (uint32_t i = 0; i < light_count_ && i < 256; ++i) {
            if ((light_sources_[i]->hidden_views & view.view_bit) != 0) {
                hidden[i >> 5] |= 1u << (i & 31u);
            }
        }
        std::memcpy(&push.m, hidden, sizeof(hidden));
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, reflect_make_rt_);
        const VkDescriptorSet rt_set = rt_->Set(renderer_->FrameIndex());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rt_->Layout(), 2, 1, &rt_set, 0, nullptr);
        PushConstants(cmd, &push, sizeof(push));
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        UseTargets(cmd, {{&refmap_color_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
        push.ids = glm::uvec4(view.index, 0, 0, 0);
        push.m = glm::mat4(1.0f);
    } else {
        BeginPass(cmd, refmap_.Extent(), {{&refmap_, false, {}}});
        Fullscreen(cmd, reflect_make_, push);
        vkCmdEndRendering(cmd);
    }

    UseTargets(cmd, {{&refmap_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                     {&hdr_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL},
                     {&hdr_copy_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL}});
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = copy.srcSubresource;
    copy.extent = {extent_.width, extent_.height, 1};
    vkCmdCopyImage(cmd, hdr_.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hdr_copy_.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &copy);
    push.f0 = glm::vec4(plane, depth_legacy ? 1.0f : 0.0f);
    // localParam[1] = (1, 1, scale, bias), localParam[2] = (h, edge): the floor environment's REFLECT_MAP values (12.16)
    const ScreenSettings& screen = lighting_->screen;
    push.f1 = glm::vec4(1.0f, 1.0f, screen.reflect_scale, screen.reflect_bias);
    push.f2 = glm::vec4(horizontal, screen.reflect_edge ? 1.0f : 0.0f);
    // PC addition (12.16): with a temporal upscaler the reflection is accumulated over the frames before it is mixed in
    static const bool temporal_off = std::getenv("PT_REFLECT_TEMPORAL_OFF") != nullptr;
    if (up_.enabled && !temporal_off && reflect_layer_.Valid() && reflect_temporal_pipeline_ &&
        (rt_reflections_ ? reflect_layer_rt_ : reflect_layer_pipeline_)) {
        RecordReflectionTemporal(cmd, view, push);
        return;
    }
    reflect_history_valid_ = false;
    reflect_post_upscale_ = false;
    UseTargets(cmd, {{&hdr_copy_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                     {&material_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                     {&hdr_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}});
    BeginPass(cmd, extent_, {{&hdr_, false, {}}});
    if (rt_reflections_) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, reflect_blend_rt_);
        const VkDescriptorSet rt_set = rt_->Set(renderer_->FrameIndex());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rt_->Layout(), 2, 1, &rt_set, 0, nullptr);
        PushConstants(cmd, &push, sizeof(push));
        vkCmdDraw(cmd, 3, 1, 0, 0);
    } else {
        Fullscreen(cmd, reflect_blend_, push);
    }
    vkCmdEndRendering(cmd);
    UseTargets(cmd, {{&hdr_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
}

// The reflection the upscaler cannot resolve (12.16, PC addition): reflect_layer.frag writes the blend's reflection on its own
// (premultiplied colour and amount) with the offset to where it was last frame (its virtual image through the previous
// unjittered view projection), and reflect_temporal.frag accumulates it there, clamped to this frame's neighbourhood, and mixes
// it into the scene as the original blend does. The history resets with the upscaler's (camera cuts) and on any frame without it.
void SceneRenderer::RecordReflectionTemporal(VkCommandBuffer cmd, const ViewSetup& view, gpu::PassPush push) {
    const float exposure = frame_->views[view.index].exposure.x;
    const bool valid_history = reflect_history_valid_ && !up_.reset && reflect_history_frame_ + 1 == frame_counter_;
    static uint32_t logged = 0;
    if (logged < 3 || (frame_counter_ % 600) == 0) {
        ++logged;
        LogInfo("reflection temporal: frame {} history {} exposure {:.4g}", frame_counter_, valid_history ? 1 : 0, exposure);
    }
    const uint32_t previous = reflect_history_index_;
    const uint32_t current = 1u - previous;
    UseTargets(cmd, {{&hdr_copy_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                     {&material_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                     {&reflect_layer_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                     {&reflect_offset_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}});
    BeginPass(cmd, extent_, {{&reflect_layer_, false, {}}, {&reflect_offset_, false, {}}});
    push.m = previous_view_projection_;
    if (rt_reflections_) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, reflect_layer_rt_);
        const VkDescriptorSet rt_set = rt_->Set(renderer_->FrameIndex());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rt_->Layout(), 2, 1, &rt_set, 0, nullptr);
        PushConstants(cmd, &push, sizeof(push));
        vkCmdDraw(cmd, 3, 1, 0, 0);
    } else {
        Fullscreen(cmd, reflect_layer_pipeline_, push);
    }
    vkCmdEndRendering(cmd);

    UseTargets(cmd, {{&reflect_layer_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                     {&reflect_offset_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                     {&reflect_history_[previous], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                     {&reflect_history_[current], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                     {&hdr_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}});
    BeginPass(cmd, extent_, {{&hdr_, false, {}}, {&reflect_history_[current], false, {}}});
    gpu::PassPush temporal;
    temporal.ids = glm::uvec4(view.index, valid_history ? 1u : 0u, gpu::kImgReflectHistoryA + previous, 0u);
    // 0.9 of the history at rest; the neighbourhood clip keeps 1.25 standard deviations around this frame's mean
    // the reflection goes in after the upscaler (upscale_resolve.frag) unless PT_REFLECT_PRE_UPSCALE=1
    static const bool pre_upscale = std::getenv("PT_REFLECT_PRE_UPSCALE") != nullptr;
    reflect_post_upscale_ = !pre_upscale;
    temporal.f1 = glm::vec4(exposure / std::max(reflect_history_exposure_, 1.0e-12f), 0.9f, 1.25f, reflect_post_upscale_ ? 1.0f : 0.0f);
    Fullscreen(cmd, reflect_temporal_pipeline_, temporal);
    vkCmdEndRendering(cmd);
    UseTargets(cmd, {{&hdr_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                     {&reflect_history_[current], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
    reflect_history_index_ = current;
    reflect_history_frame_ = frame_counter_;
    reflect_history_exposure_ = exposure;
    reflect_history_valid_ = true;
}

void SceneRenderer::Stamp(VkCommandBuffer cmd, uint32_t index) {
    if (queries_) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries_, index);
    }
}

void SceneRenderer::RecordObjectVelocity(VkCommandBuffer cmd, const ViewSetup& view) {
    UseTargets(cmd, {{&object_velocity_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, {&depth_, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}});
    BeginPass(cmd, extent_, {{&object_velocity_, true, {}}}, &depth_, true, false);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, velocity_pipeline_);
    for (const Draw& d : draws_) {
        const SubMesh& sub = *d.sub;
        if (d.previous_model == gpu::kInvalid || sub.pass != RenderPass::Opaque || sub.kind != gpu::kKindDeferred || !Visible(d, view)) {
            continue;
        }
        vkCmdSetCullMode(cmd, sub.double_sided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
        vkCmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &d.mesh->vertices.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, d.mesh->indices.buffer, 0, VK_INDEX_TYPE_UINT32);
        gpu::DrawPush push;
        push.model = d.transform;
        push.ids = glm::uvec4(view.index, d.material, motion_view_, sub.skinned ? d.skin_base : gpu::kInvalid);
        push.tint = d.tint;
        const uint32_t previous_skin = sub.skinned ? d.previous_skin : gpu::kInvalid;
        push.aux[0] = glm::vec4(std::bit_cast<float>(d.previous_model), std::bit_cast<float>(previous_skin), 0.0f, 0.0f);
        PushConstants(cmd, &push, sizeof(push));
        vkCmdDrawIndexed(cmd, sub.index_count, 1, sub.first_index, sub.vertex_offset, 0);
    }
    vkCmdEndRendering(cmd);
    UseTargets(cmd, {{&object_velocity_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
}

void SceneRenderer::RecordView(VkCommandBuffer cmd, const ViewSetup& view, RenderTarget& output, bool main_view) {
    BeginLabel(cmd, main_view ? "gbuffer" : "mirror gbuffer");
    RecordGBuffer(cmd, view);
    EndLabel(cmd);
    if (main_view) {
        Stamp(cmd, 3);
        if (MotionBlurActive()) {
            BeginLabel(cmd, "object velocity");
            RecordObjectVelocity(cmd, view);
            EndLabel(cmd);
        }
    }
    // the occlusion pass samples the depth over the whole target; the capture view, which renders into a corner, goes without
    const bool occlusion = toggles.occlusion && lighting_->valid && main_view;
    if (occlusion) {
        BeginLabel(cmd, "occlusion");
        RecordOcclusion(cmd, view);
        EndLabel(cmd);
    }
    rt_ao_ready_ = false;
    if (main_view && rt_ao_active_ && lighting_->valid) {
        BeginLabel(cmd, "ray traced ambient occlusion");
        RecordRtAmbientOcclusion(cmd, view);
        EndLabel(cmd);
    }
    BeginLabel(cmd, main_view ? "lighting" : "mirror lighting");
    RecordLighting(cmd, view);
    EndLabel(cmd);
    // 0x922C60 sets the SUBSURFACE_SCATTER plugin's bit in the main view's plugin mask on the ending only (rendering.md 12.26)
    static const bool sss_off = [] {
        const char* v = std::getenv("PT_SUBSURFACE");
        return v && v[0] == '0';
    }();
    if (main_view && sss_ready_ && !sss_off && lighting_->valid && lighting_->screen.subsurface_scatter && toggles.subsurface_scatter) {
        BeginLabel(cmd, "subsurface scattering");
        sss_.Record(cmd, diffuse_, view.index, [&] { BindSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS); });
        EndLabel(cmd);
    }
    if (main_view) {
        Stamp(cmd, 4);
        BeginLabel(cmd, "luminance");
        RecordLuminance(cmd);
        EndLabel(cmd);
    }
    BeginLabel(cmd, main_view ? "compose and forward" : "mirror compose and forward");
    UseTargets(cmd, {{&output, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                     {&albedo_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                     {&depth_, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}});
    if (main_view && mirror_active_) {
        UseTargets(cmd, {{&mirror_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
    }
    BeginPass(cmd, ViewArea(view), {{&output, false, {}}}, &depth_, true, false);
    gpu::PassPush push;
    push.ids = glm::uvec4(view.index, occlusion ? 1u : 0u, 0, 0);
    const TppAtmosphereSettings& tpp = lighting_->tpp;
    if (tpp.enabled && toggles.tpp_atmosphere) {
        push.ids.z = 1u;
        // PT_SKY=0: the sky pixels as the composite alone leaves them (the port's former black sky behind the fog), for A/B runs
        static const bool sky_off = [] {
            const char* s = std::getenv("PT_SKY");
            return s && std::string_view(s) == "0";
        }();
        if (tpp.sky && !sky_off) {
            push.ids.z |= 4u;
        }
    }
    if (tpp.tonemap && toggles.tpp_atmosphere) {
        push.ids.z |= 2u;
        const float t = std::clamp(tpp.threshold, 0.0f, 0.99f);
        const float u = std::max(tpp.range - t, 1.0e-3f);
        const float c = u * (1.0f - t) / std::max(t + u - 1.0f, 1.0e-3f);
        push.f0 = glm::vec4(1.0f / (c * c), t, c, 0.0f);
    }
    Fullscreen(cmd, compose_, push);
    // f010_dumps 1490: fox3dfw_glass and fox3dfw_constant draws write depth (DB_DEPTH_CONTROL 0x7e6), so the effects behind them are hidden
    vkCmdEndRendering(cmd);
    if (main_view && dump_requested_ && &output == &hdr_) {
        RecordDumpCopy(cmd, output, dump_compose_);
    }
    UseTargets(cmd, {{&depth_, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL}});
    BeginPass(cmd, ViewArea(view), {{&output, false, {}}}, &depth_, false, false);
    RecordForward(cmd, view, output);
    vkCmdEndRendering(cmd);
    if (main_view && dump_requested_ && &output == &hdr_) {
        RecordDumpCopy(cmd, output, dump_forward_);
    }
    UseTargets(cmd, {{&depth_, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}});
    if (main_view) {
        Stamp(cmd, 7);
    }
    BeginPass(cmd, ViewArea(view), {{&output, false, {}}}, &depth_, true, false);
    if (main_view && up_.enabled) {
        RecordOpaqueSnapshot(cmd, output);
    }
    if (vfx_forward) {
        SceneVfxContext context;
        context.cmd = cmd;
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
        // effects stay out of the capture view
        context.mirrored = view.mirrored || !main_view;
        if (lighting_->tpp.enabled && toggles.tpp_atmosphere) {
            std::copy(std::begin(frame_->fog), std::end(frame_->fog), std::begin(context.fog));
            context.fog_mode = 1u;
        }
        // hdr_copy_ is free here: the reflection pass fills it again after the forward pass
        context.scene_copy_view = hdr_copy_.image.view;
        if (&output == &hdr_) {
            context.copy_scene = [&] { RecordSceneCopy(cmd, output); };
            context.subset = kVfxScene;
        }
        vfx_forward(context);
        BindSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS);
        if (&output == &hdr_) {
            RecordEffects(cmd, output, context);
        }
    }
    vkCmdEndRendering(cmd);
    if (main_view && dump_requested_ && &output == &hdr_) {
        RecordDumpCopy(cmd, output, dump_scene_);
        dump_recorded_ = true;
    }
    UseTargets(cmd, {{&output, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}});
    EndLabel(cmd);
    if (main_view) {
        Stamp(cmd, 5);
    }
}

void SceneRenderer::SettleExposure(const ExposureSettings& settings) {
    FrameSlot& slot = slots_[renderer_->FrameIndex()];
    renderer_->Context().Submit([&](VkCommandBuffer cmd) {
        BeginLabel(cmd, "settle exposure");
        BindSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS);
        RecordShadows(cmd);
        RecordGBuffer(cmd, main_view_);
        RecordLighting(cmd, main_view_);
        RecordLuminance(cmd);
        EndLabel(cmd);
    });
    vmaInvalidateAllocation(renderer_->Context().allocator, slot.luminance.allocation, 0, VK_WHOLE_SIZE);
    const glm::vec2* partial = static_cast<const glm::vec2*>(slot.luminance.mapped);
    double sum = 0.0;
    double count = 0.0;
    for (uint32_t i = 0; i < slot.luminance_groups; ++i) {
        sum += partial[i].x;
        count += partial[i].y;
    }
    adaptation_valid_ = true;
    if (count <= 0.0) {
        return;
    }
    const float raw = static_cast<float>(sum / count) / std::max(exposure_, 1.0e-12f);
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
    // VR: the screen effects stay out of the eyes for this frame (SetVrEye), the toggles come back as they were
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
    if (frame_counter_ == 0) {
        ApplyEnvironmentOverrides();
    }
    if(!EnsureShadowTarget()) return;
    const VkExtent2D output = renderer_->RenderExtent();
    const bool upscaling = BeginUpscaleFrame(output);
    if (!EnsureTargets(upscaling ? up_.render : output, output, upscaling)) {
        return;
    }
    FrameSlot& slot = slots_[renderer_->FrameIndex()];
    textures_->FlushMaterials();
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
        // the port's own device memory (VMA's blocks over every heap), to see growth or a burst of allocations in a timing run
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
        vmaGetHeapBudgets(renderer_->Context().allocator, budgets);
        const VkPhysicalDeviceMemoryProperties* memory = nullptr;
        vmaGetMemoryProperties(renderer_->Context().allocator, &memory);
        double bytes = 0.0;
        for (uint32_t h = 0; memory && h < memory->memoryHeapCount; ++h) {
            bytes += static_cast<double>(budgets[h].statistics.blockBytes);
        }
        stats_.device_mb = static_cast<float>(bytes / (1024.0 * 1024.0));
    }
    if (slot.timed && frame_counter_ > 8) {
        gpu_ms_sum_ += stats_.gpu_ms;
        ++gpu_samples_;
        if (!timing_ring_.empty()) {
            FrameTiming& t = timing_ring_[timing_count_++ % timing_ring_.size()];
            t.gpu_ms = stats_.gpu_ms;
            std::copy(std::begin(stats_.pass_ms), std::end(stats_.pass_ms), std::begin(t.pass_ms));
            std::copy(std::begin(stats_.part_ms), std::end(stats_.part_ms), std::begin(t.part_ms));
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
    rt_active_ = rt_ && raytracing.shadows && toggles.shadows;
    rt_reflections_ = rt_ && reflect_make_rt_ && raytracing.reflections && toggles.local_reflections;
    rt_contact_active_ = rt_ && light_contact_ && raytracing.contact_shadows && toggles.shadows;
    rt_ao_active_ = rt_ && rt_ao_trace_ && rt_ao_filter_ && probe_ao_ && raytracing.ambient_occlusion && toggles.probes && EnsureAoTargets();
    if (!rt_ao_active_) {
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
    VkCommandBuffer cmd = renderer_->Cmd();
    vkCmdResetQueryPool(cmd, slot.queries, 0, kTimestamps);
    if (up_.enabled) {
        vkCmdResetQueryPool(cmd, slot.upscale_queries, 0, 4);
    }
    queries_ = slot.queries;
    Stamp(cmd, 0);
    BindSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS);
    BeginLabel(cmd, "shadows");
    RecordShadows(cmd);
    EndLabel(cmd);
    Stamp(cmd, 1);
    if (mirror_active_) {
        RecordView(cmd, mirror_view_, mirror_, false);
        if (mirror_temporal_enabled_) {
            RecordMirrorTemporal(cmd);
        } else {
            mirror_history_valid_ = false;
        }
        if (dump_requested_) {
            // the capture the mirror samples: the black mirror of a camera close to the wall is either an empty
            // capture or the material's own output, and this tells them apart
            RecordDumpCopy(cmd, mirror_, dump_mirror_);
        }
        static const bool trace = std::getenv("PT_MIRROR_TRACE") != nullptr;
        if (trace) {
            // what the capture view renders: a mirror that shows the room must have geometry inside this frustum and
            // lights that reach it (the black mirror of a camera at the wall had both and still came out at the clear
            // value, see the mirror notes in rendering.md)
            uint32_t drawn = 0;
            for (const Draw& d : draws_) {
                drawn += Visible(d, mirror_view_) ? 1u : 0u;
            }
            LogInfo("mirror trace: capture view {} eye ({:.3f} {:.3f} {:.3f}) distance {:.3f} size {} draws {} of {}, lights {}, exposure {:.6f} "
                    "(main {:.6f} ev {:.3f})",
                    mirror_view_.index, mirror_view_.eye.x, mirror_view_.eye.y, mirror_view_.eye.z, mirror_view_.projection[3][2],
                    mirror_view_.area.width, drawn, draws_.size(), light_count_, frame_->views[mirror_view_.index].exposure.x, exposure_, ev_);
            // What the capture view's own passes produced, read back before the main view overwrites the shared targets:
            // the mirror's pixels come out at the clear value while these hold radiance, so the loss is in the compose or
            // the forward pass and one of these three is the term that is missing. PT_MIRROR_PROBE=0 turns it off.
            static const bool probe_capture = [] {
                const char* value = std::getenv("PT_MIRROR_PROBE");
                return value && std::atoi(value) != 0;
            }();
            static vk::Buffer probe_buffers[3];
            if (probe_capture) {
                vk::Context& pctx = renderer_->Context();
                RenderTarget* targets[3] = {&albedo_, &material_, &diffuse_};
                const char* names[3] = {"albedo", "material", "diffuse"};
                const uint32_t side = mirror_view_.area.width;
                for (int t = 0; t < 3; ++t) {
                    RenderTarget& target = *targets[t];
                    if (!target.Valid()) {
                        continue;
                    }
                    const VkExtent3D e = target.image.extent;
                    const VkDeviceSize bytes = VkDeviceSize(e.width) * e.height * TargetTexelBytes(target.image.format);
                    if (probe_buffers[t].size < bytes) {
                        pctx.DestroyBuffer(probe_buffers[t]);
                        pctx.CreateBuffer(probe_buffers[t], bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
                    }
                    if (!probe_buffers[t].mapped) {
                        continue;
                    }
                    RecordDumpCopy(cmd, target, probe_buffers[t]);
                    pctx.Submit([&](VkCommandBuffer sync) { (void)sync; });
                    vmaInvalidateAllocation(pctx.allocator, probe_buffers[t].allocation, 0, VK_WHOLE_SIZE);
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
    RecordView(cmd, main_view_, hdr_, true);
    RecordReflectionSample(cmd, *lighting_);
    BindSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS);
    if (debug_view == DebugView::Lit || debug_view == DebugView::Ambient) {
        if (up_.enabled) {
            BeginLabel(cmd, "upscale inputs");
            RecordUpscaleInputs(cmd, main_view_);
            EndLabel(cmd);
        }
        Stamp(cmd, 8);
        reflect_post_upscale_ = false;
        if (toggles.local_reflections && lighting_->valid && lighting_->screen.local_reflections) {
            BeginLabel(cmd, "reflections");
            RecordReflections(cmd, main_view_);
            EndLabel(cmd);
        }
        Stamp(cmd, 9);
        if (up_.enabled) {
            BeginLabel(cmd, "upscale");
            RecordUpscale(cmd, dt);
            EndLabel(cmd);
            post_bindings_ = true;
            BindSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS);
        }
        Stamp(cmd, 10);
        BeginLabel(cmd, "post");
        RecordPost(cmd, *lighting_, dt);
        EndLabel(cmd);
        post_bindings_ = false;
    } else {
        for (uint32_t index : {8u, 9u, 10u, 11u}) {
            Stamp(cmd, index);
        }
        BeginLabel(cmd, "debug view");
        RecordDebug(cmd);
        EndLabel(cmd);
    }
    Stamp(cmd, 6);
    queries_ = VK_NULL_HANDLE;
    last_timed_slot_ = renderer_->FrameIndex();
    slot.timed = true;
    slot.measured = true;
    slot.exposure_used = exposure_;
    stats_.ev = ev_;
    stats_.exposure = exposure_;
    if (vr_eye_ != 1) {
        ++frame_counter_;
    }
    stats_.cpu_ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - cpu_start).count();
    if (static const bool frame_csv = std::getenv("PT_FRAME_CSV") != nullptr; frame_csv) {
        const float interval = previous_frame_start_ == std::chrono::steady_clock::time_point{}
                                   ? 0.0f
                                   : std::chrono::duration<float, std::milli>(cpu_start - previous_frame_start_).count();
        frame_trace_.push_back({stats_.cpu_ms, interval, stats_.lights, stats_.shadow_views, stats_.draws});
    }
    previous_frame_start_ = cpu_start;
    if (frame_counter_ > 8) {
        cpu_ms_sum_ += stats_.cpu_ms;
        ++cpu_samples_;
    }
}

}
