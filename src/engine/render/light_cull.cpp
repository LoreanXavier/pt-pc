#include "engine/render/light_cull.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace pt::lightcull {
namespace {

constexpr uint8_t kRight = 0x80;
constexpr uint8_t kTop = 0x40;
constexpr uint8_t kNear = 0x20;
constexpr uint8_t kLeft = 0x08;
constexpr uint8_t kBottom = 0x04;
constexpr uint8_t kFar = 0x02;

uint8_t Outcode(const glm::vec4& c) {
    uint8_t code = 0;
    code |= c.w < c.x ? kRight : 0;
    code |= c.w < c.y ? kTop : 0;
    code |= c.w < c.z ? kNear : 0;
    code |= c.w < -c.x ? kLeft : 0;
    code |= c.w < -c.y ? kBottom : 0;
    code |= c.w < -c.z ? kFar : 0;
    return code;
}

constexpr uint8_t kRejectMask = 0xEE;

/* Buffer sizes copied from the original's culling core (0xD53AC0), so an overfull view overflows at the same count it did on PS4. */
constexpr uint32_t kMaxEntries = 0x80;
constexpr uint32_t kMaxPoints = 0x100;
constexpr uint32_t kMaxPlanes = 0x100;

struct Entry {
    OccluderVolume volume;
    std::array<glm::vec3, 7> points{};
    uint32_t point_count = 0;
    bool flag4 = false;
    bool selected = false;
};

float SegmentDistance2(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b) {
    const glm::vec3 e = b - a;
    const float len2 = glm::dot(e, e);
    const float t = std::max(std::min(glm::dot(p - a, e), len2), 0.0f);
    const glm::vec3 q = len2 > 0.0f ? a + e * (t / len2) : a;
    const glm::vec3 d = q - p;
    return glm::dot(d, d);
}

}

const char* VerdictName(OccluderVerdict v) {
    switch (v) {
    case OccluderVerdict::Kept:
        return "kept";
    case OccluderVerdict::Disabled:
        return "fewer than 3 points";
    case OccluderVerdict::OverCapacity:
        return "over the buffers";
    case OccluderVerdict::OutsideView:
        return "outside the view";
    case OccluderVerdict::SmallArea:
        return "projected area too small";
    case OccluderVerdict::BackFacing:
        return "one sided, seen from the back";
    case OccluderVerdict::TooFar:
        return "too far from the eye";
    case OccluderVerdict::HiddenByOther:
        return "inside a larger occluder's volume";
    case OccluderVerdict::OverLimit:
        return "over the view's occluder limit";
    }
    return "?";
}

OccluderSet BuildOccluderVolumes(std::span<const SceneOccluder> occluders, const glm::mat4& view_projection, const glm::vec3& eye,
                                 const OccluderLimits& limits) {
    OccluderSet out;
    out.verdicts.assign(occluders.size(), OccluderVerdict::Kept);
    const glm::vec4 axis = glm::inverse(view_projection) * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    std::vector<Entry> entries;
    uint32_t points_used = 0;
    uint32_t planes_used = 0;

    for (uint32_t index = 0; index < occluders.size(); ++index) {
        const SceneOccluder& o = occluders[index];
        const uint32_t n = std::min<uint32_t>(o.count, 7);
        if (n < 3) {
            out.verdicts[index] = OccluderVerdict::Disabled;
            continue;
        }
        if (entries.size() >= kMaxEntries || points_used + n > kMaxPoints || planes_used + n + 1 > kMaxPlanes) {
            out.verdicts[index] = OccluderVerdict::OverCapacity;
            continue;
        }
        std::array<glm::vec4, 7> clip{};
        std::array<uint8_t, 7> code{};
        uint8_t all = 0xFF;
        uint8_t any_near = 0;
        for (uint32_t i = 0; i < n; ++i) {
            clip[i] = view_projection * glm::vec4(o.points[i], 1.0f);
            code[i] = Outcode(clip[i]);
            all &= code[i];
            any_near |= code[i] & kNear;
        }
        if ((all & kRejectMask) != 0) {
            out.verdicts[index] = OccluderVerdict::OutsideView;
            continue;
        }
        std::array<glm::vec4, 14> poly{};
        uint32_t m = 0;
        if (any_near) {
            glm::vec4 prev = clip[n - 1];
            bool prev_behind = (code[n - 1] & kNear) != 0;
            for (uint32_t i = 0; i < n; ++i) {
                const glm::vec4 cur = clip[i];
                const bool cur_behind = (code[i] & kNear) != 0;
                if (prev_behind != cur_behind) {
                    glm::vec4 x = (cur.w * prev - prev.w * cur) / (prev.w - cur.w);
                    x.w = x.z;
                    poly[m++] = x;
                }
                if (!cur_behind) {
                    poly[m++] = cur;
                }
                prev = cur;
                prev_behind = cur_behind;
            }
        } else {
            for (uint32_t i = 0; i < n; ++i) {
                poly[m++] = clip[i];
            }
        }
        float fan = 0.0f;
        glm::vec2 last(0.0f);
        glm::vec2 before_last(0.0f);
        glm::vec2 first(0.0f);
        if (m >= 3) {
            const auto ndc = [&](uint32_t k) { return glm::vec2(poly[k]) / poly[k].w; };
            first = ndc(0);
            last = ndc(1);
            for (uint32_t k = 2; k < m; ++k) {
                before_last = last;
                last = ndc(k);
                const glm::vec2 a = before_last - first;
                const glm::vec2 b = last - first;
                const float c = a.x * b.y - a.y * b.x;
                if (c * c > std::numeric_limits<float>::min()) {
                    fan += std::abs(c);
                }
            }
        }
        if (fan < limits.min_fan_area || m < 3) {
            out.verdicts[index] = OccluderVerdict::SmallArea;
            continue;
        }
        const glm::vec3 normal_rh = glm::cross(o.points[1] - o.points[0], o.points[2] - o.points[1]);
        if (o.one_sided && glm::dot(normal_rh, eye - o.points[0]) > 0.0f) {
            out.verdicts[index] = OccluderVerdict::BackFacing;
            continue;
        }
        const float normal_len = glm::length(normal_rh);
        if (!(normal_len > 0.0f)) {
            out.verdicts[index] = OccluderVerdict::SmallArea;
            continue;
        }
        const glm::vec3 nl = -normal_rh / normal_len;
        const float plane_offset = glm::dot(o.points[0] - eye, nl);
        const glm::vec3 foot = eye + nl * plane_offset;
        bool inside = true;
        for (uint32_t i = 0, prev = n - 1; i < n; prev = i++) {
            if (glm::dot(nl, glm::cross(foot - o.points[prev], o.points[i] - o.points[prev])) < 0.0f) {
                inside = false;
                break;
            }
        }
        float distance = std::abs(plane_offset);
        if (!inside) {
            float best = std::numeric_limits<float>::max();
            for (uint32_t i = 0, prev = n - 1; i < n; prev = i++) {
                best = std::min(best, SegmentDistance2(eye, o.points[prev], o.points[i]));
            }
            distance = std::sqrt(best);
        }
        const bool flag4 = o.flag4;
        if (distance > (flag4 ? limits.max_distance_flag4 : limits.max_distance)) {
            out.verdicts[index] = OccluderVerdict::TooFar;
            continue;
        }
        Entry e;
        e.flag4 = flag4;
        e.point_count = n;
        for (uint32_t i = 0; i < n; ++i) {
            e.points[i] = o.points[i];
        }
        e.volume.source = index;
        e.volume.area = fan * 0.5f;
        e.volume.distance = distance;
        const glm::vec3 pn = normal_rh / normal_len;
        glm::vec4 own(pn, -glm::dot(pn, o.points[0]));
        const float turn = glm::dot(own, glm::vec4(eye, 1.0f)) > 0.0f ? -1.0f : 1.0f;
        e.volume.planes[e.volume.plane_count++] = own * turn;
        for (uint32_t i = 0, prev = n - 1; i < n; prev = i++) {
            const glm::vec3 cur = o.points[i];
            const glm::vec3 c = glm::cross(cur - o.points[prev], eye - cur);
            const float len = glm::length(c);
            const glm::vec3 m3 = len > 0.0f ? c / len : c;
            const glm::vec4 plane = glm::vec4(m3, -glm::dot(m3, cur)) * turn;
            if ((code[i] & code[prev]) != 0 && glm::dot(plane, axis) > 0.0f) {
                continue;
            }
            e.volume.planes[e.volume.plane_count++] = plane;
        }
        points_used += n;
        planes_used += e.volume.plane_count;
        entries.push_back(e);
    }

    std::vector<uint32_t> near_order;
    std::vector<uint32_t> area_order;
    for (uint32_t i = 0; i < entries.size(); ++i) {
        (entries[i].flag4 ? near_order : area_order).push_back(i);
    }
    std::stable_sort(near_order.begin(), near_order.end(),
                     [&](uint32_t a, uint32_t b) { return entries[a].volume.distance < entries[b].volume.distance; });
    std::stable_sort(area_order.begin(), area_order.end(), [&](uint32_t a, uint32_t b) { return entries[a].volume.area > entries[b].volume.area; });
    const uint32_t near_keep = std::min<uint32_t>({static_cast<uint32_t>(near_order.size()), limits.nearest, 32u});
    const uint32_t area_keep = std::min<uint32_t>(static_cast<uint32_t>(area_order.size()), 32u - near_keep);
    for (uint32_t k = 0; k < near_keep; ++k) {
        entries[near_order[k]].selected = true;
    }
    for (uint32_t k = 0; k < area_keep; ++k) {
        entries[area_order[k]].selected = true;
    }
    for (uint32_t k = near_keep; k < near_order.size(); ++k) {
        out.verdicts[entries[near_order[k]].volume.source] = OccluderVerdict::OverLimit;
    }
    for (uint32_t k = area_keep; k < area_order.size(); ++k) {
        out.verdicts[entries[area_order[k]].volume.source] = OccluderVerdict::OverLimit;
    }
    for (uint32_t a = 0; a < entries.size(); ++a) {
        if (!entries[a].selected || entries[a].volume.area < 0.4f) {
            continue;
        }
        for (uint32_t b = 0; b < entries.size(); ++b) {
            if (a == b || !entries[b].selected) {
                continue;
            }
            float least = std::numeric_limits<float>::max();
            for (uint32_t p = 0; p < entries[a].volume.plane_count; ++p) {
                for (uint32_t v = 0; v < entries[b].point_count; ++v) {
                    least = std::min(least, glm::dot(entries[a].volume.planes[p], glm::vec4(entries[b].points[v], 1.0f)));
                }
            }
            if (least > 0.0f) {
                entries[b].selected = false;
                out.verdicts[entries[b].volume.source] = OccluderVerdict::HiddenByOther;
            }
        }
    }
    uint32_t near_count = 0;
    std::vector<uint32_t> rest;
    for (uint32_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].selected) {
            continue;
        }
        if (entries[i].flag4) {
            ++near_count;
        } else {
            entries[i].selected = false;
            rest.push_back(i);
        }
    }
    std::stable_sort(rest.begin(), rest.end(), [&](uint32_t a, uint32_t b) { return entries[a].volume.area > entries[b].volume.area; });
    if (rest.size() > 32) {
        rest.resize(32);
    }
    const uint32_t rest_keep = near_count < limits.total ? std::min<uint32_t>(limits.total - near_count, static_cast<uint32_t>(rest.size())) : 0u;
    for (uint32_t k = 0; k < rest.size(); ++k) {
        if (k < rest_keep) {
            entries[rest[k]].selected = true;
        } else {
            out.verdicts[entries[rest[k]].volume.source] = OccluderVerdict::OverLimit;
        }
    }
    for (const Entry& e : entries) {
        if (e.selected) {
            out.volumes.push_back(e.volume);
        }
    }
    return out;
}

bool BoxInVolume(const glm::vec3& lo, const glm::vec3& hi, const OccluderVolume& volume) {
    for (uint32_t i = 0; i < volume.plane_count; ++i) {
        const glm::vec4& p = volume.planes[i];
        const glm::vec3 worst(p.x > 0.0f ? lo.x : hi.x, p.y > 0.0f ? lo.y : hi.y, p.z > 0.0f ? lo.z : hi.z);
        if (!(glm::dot(glm::vec3(p), worst) + p.w > 0.0f)) {
            return false;
        }
    }
    return volume.plane_count > 0;
}

int FindOccludingVolume(const glm::vec3& lo, const glm::vec3& hi, const OccluderSet& set) {
    for (size_t i = 0; i < set.volumes.size(); ++i) {
        if (BoxInVolume(lo, hi, set.volumes[i])) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const OccluderLimits& LimitsFromEnv() {
    static const OccluderLimits limits = [] {
        OccluderLimits l;
        if (const char* s = std::getenv("PT_OCCLUDER_LIMITS")) {
            unsigned total = l.total;
            unsigned nearest = l.nearest;
            std::sscanf(s, "%f %f %f %u %u", &l.min_fan_area, &l.max_distance, &l.max_distance_flag4, &total, &nearest);
            l.total = std::min(total, 16u);
            l.nearest = nearest;
        }
        return l;
    }();
    return limits;
}

const LightGrid& GridFromEnv() {
    static const LightGrid grid = [] {
        LightGrid g;
        if (const char* s = std::getenv("PT_LIGHT_GRID")) {
            g.valid = std::sscanf(s, "%f %f %f %f", &g.origin.x, &g.origin.y, &g.origin.z, &g.cell) == 4 && g.cell > 0.0f;
        }
        return g;
    }();
    return grid;
}

bool GridBoxInFrustum(const glm::vec3& lo, const glm::vec3& hi, const glm::mat4& view_projection, const LightGrid& grid) {
    const float inv = 1.0f / grid.cell;
    const glm::vec3 cell_lo = glm::floor(glm::max(glm::min((lo - grid.origin) * inv, glm::vec3(32767.0f)), glm::vec3(-32768.0f)));
    const glm::vec3 cell_hi = glm::floor(glm::max(glm::min((hi - grid.origin) * inv + 1.0f, glm::vec3(32767.0f)), glm::vec3(-32768.0f)));
    const glm::vec3 a = grid.origin + cell_lo * grid.cell;
    const glm::vec3 b = grid.origin + cell_hi * grid.cell;
    const glm::mat4 m = glm::transpose(view_projection);
    const glm::vec4 planes[6] = {m[3] - m[0], m[3] + m[0], m[3] - m[1], m[3] + m[1], m[3] - m[2], m[3] + m[2]};
    for (const glm::vec4& p : planes) {
        const float most = std::max(p.x * a.x, p.x * b.x) + std::max(p.y * a.y, p.y * b.y) + std::max(p.z * a.z, p.z * b.z) + p.w;
        if (!(most > 0.0f)) {
            return false;
        }
    }
    return true;
}

}
