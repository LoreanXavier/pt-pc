#include "engine/physics/collision_world.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include "engine/core/log.h"
#include "engine/core/parallel_for.h"

namespace pt {
namespace {

bool InsideTriangle(const glm::vec3& p, const CollisionTriangle& t) {
    return glm::dot(glm::cross(t.b - t.a, p - t.a), t.normal) >= 0.0f && glm::dot(glm::cross(t.c - t.b, p - t.b), t.normal) >= 0.0f &&
           glm::dot(glm::cross(t.a - t.c, p - t.c), t.normal) >= 0.0f;
}

glm::vec3 ClosestOnSegment(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b) {
    const glm::vec3 ab = b - a;
    const float length_sq = glm::dot(ab, ab);
    const float u = length_sq > 0.0f ? std::clamp(glm::dot(p - a, ab) / length_sq, 0.0f, 1.0f) : 0.0f;
    return a + ab * u;
}

// 0xC151B0: nearest point on the three edges
glm::vec3 ClosestOnEdges(const glm::vec3& p, const CollisionTriangle& t) {
    const glm::vec3 candidates[3] = {ClosestOnSegment(p, t.a, t.b), ClosestOnSegment(p, t.b, t.c), ClosestOnSegment(p, t.c, t.a)};
    glm::vec3 best = candidates[0];
    float best_sq = glm::dot(p - best, p - best);
    for (int i = 1; i < 3; ++i) {
        const float sq = glm::dot(p - candidates[i], p - candidates[i]);
        if (sq < best_sq) {
            best = candidates[i];
            best_sq = sq;
        }
    }
    return best;
}

bool SweepPoint(const glm::vec3& s, const glm::vec3& d, float r, const glm::vec3& v, float& t) {
    const glm::vec3 m = s - v;
    const float a = glm::dot(d, d);
    const float b = glm::dot(m, d);
    const float c = glm::dot(m, m) - r * r;
    const float disc = b * b - a * c;
    if (a <= 1e-12f || b >= 0.0f || c < 0.0f || disc < 0.0f) {
        return false;
    }
    t = (-b - std::sqrt(disc)) / a;
    return t >= 0.0f && t <= 1.0f;
}

bool SweepSegment(const glm::vec3& s, const glm::vec3& d, float r, const glm::vec3& a, const glm::vec3& b, float& t) {
    const glm::vec3 e = b - a;
    const float ee = glm::dot(e, e);
    if (ee <= 1e-12f) {
        return false;
    }
    const glm::vec3 m = s - a;
    const float de = glm::dot(d, e);
    const float me = glm::dot(m, e);
    const float qa = glm::dot(d, d) - de * de / ee;
    const float qb = glm::dot(m, d) - me * de / ee;
    const float qc = glm::dot(m, m) - me * me / ee - r * r;
    const float disc = qb * qb - qa * qc;
    if (qa <= 1e-12f || qb >= 0.0f || disc < 0.0f) {
        return false;
    }
    const float time = (-qb - std::sqrt(disc)) / qa;
    const float u = (me + de * time) / ee;
    if (time < 0.0f || time > 1.0f || u < 0.0f || u > 1.0f) {
        return false;
    }
    t = time;
    return true;
}

void SortHits(std::vector<SphereHit>& hits) {
    std::stable_sort(hits.begin(), hits.end(), [](const SphereHit& a, const SphereHit& b) { return a.distance_sq < b.distance_sq; });
}

}

// The decompiled controller (0xAFEB60's cast radius 10 cm inside the nearest contact plane, 0x12AD310's single cast that
// stops 8 mm before the hit, the move from the center before the depenetration). PT_CONTROLLER_SLIDE=1 selects the sliding
// variant e3d02e9 had made the default (player radius 0.3, the remaining motion slid along the hit plane), which walks the
// player 10 cm closer to the walls than the captures show (gameplay.md 10.3) and left f060's baby out of view
bool OriginalController() {
    static const bool original = [] {
        const char* v = std::getenv("PT_CONTROLLER_SLIDE");
        return !(v && v[0] == '1');
    }();
    return original;
}

void CollisionWorld::Clear() {
    triangles_.clear();
    owners_.assign(1, std::string());
    owner_active_.assign(1, 1);
    owner_triangles_.assign(1, 0);
    cell_items_.clear();
    cell_start_.clear();
    sparse_cells_.clear();
    cell_nx_ = 0;
    cell_nz_ = 0;
    stamp_.clear();
}

bool CollisionWorld::MakeTriangle(const GeomTriangle& t, const glm::mat4& world, CollisionTriangle& c) {
    c.a = glm::vec3(world * glm::vec4(t.a, 1.0f));
    c.b = glm::vec3(world * glm::vec4(t.b, 1.0f));
    c.c = glm::vec3(world * glm::vec4(t.c, 1.0f));
    const glm::vec3 n = glm::cross(c.b - c.a, c.c - c.a);
    const float length = glm::length(n);
    if (length < 1e-8f) {
        return false;
    }
    c.normal = n / length;
    c.min = glm::min(c.a, glm::min(c.b, c.c));
    c.max = glm::max(c.a, glm::max(c.b, c.c));
    c.tags = t.tags;
    c.owner = 0;
    c.shape_flags = t.shape_flags;
    c.prim_info = t.prim_info;
    return true;
}

void CollisionWorld::AddTriangles(std::span<const GeomTriangle> triangles, const glm::mat4& world, int node_filter, uint32_t owner) {
    for (const GeomTriangle& t : triangles) {
        if (node_filter >= 0 && t.node != node_filter) {
            continue;
        }
        CollisionTriangle c;
        if (!MakeTriangle(t, world, c)) {
            continue;
        }
        c.owner = owner;
        triangles_.push_back(c);
        ++owner_triangles_[owner];
    }
}

size_t CollisionWorld::ActiveTriangles() const {
    size_t count = 0;
    for (size_t owner = 0; owner < owner_triangles_.size(); ++owner) {
        count += owner_active_[owner] ? owner_triangles_[owner] : 0;
    }
    return count;
}

void CollisionWorld::AppendBlocks(std::span<const Block> blocks) {
    std::vector<size_t> offsets(blocks.size() + 1, triangles_.size());
    for (size_t b = 0; b < blocks.size(); ++b) {
        const size_t n = blocks[b].triangles ? blocks[b].triangles->size() : 0;
        offsets[b + 1] = offsets[b] + n;
        owner_triangles_[blocks[b].owner] += static_cast<uint32_t>(n);
    }
    triangles_.resize(offsets.back());
    // each block lands at its own offset, so the copy splits over threads without changing the result
    ParallelChunks(blocks.size(), ParallelChunkCount(offsets.back() - offsets.front(), 32768), [&](size_t, size_t begin, size_t end) {
        for (size_t b = begin; b < end; ++b) {
            if (!blocks[b].triangles) {
                continue;
            }
            CollisionTriangle* out = triangles_.data() + offsets[b];
            for (const CollisionTriangle& t : *blocks[b].triangles) {
                *out = t;
                out->owner = blocks[b].owner;
                ++out;
            }
        }
    });
}

void CollisionWorld::Build() {
    cell_items_.clear();
    cell_start_.clear();
    sparse_cells_.clear();
    cell_nx_ = 0;
    cell_nz_ = 0;
    bounds_min_ = glm::vec3(INFINITY);
    bounds_max_ = glm::vec3(-INFINITY);
    for (const CollisionTriangle& t : triangles_) {
        bounds_min_ = glm::min(bounds_min_, t.min);
        bounds_max_ = glm::max(bounds_max_, t.max);
    }
    stamp_.assign(triangles_.size(), 0);
    if (triangles_.empty()) {
        return;
    }
    // Each cell lists its triangles in ascending order, as the per cell vectors filled in triangle order did, so queries visit
    // the same candidates in the same order.
    const auto cells = [](const CollisionTriangle& t, int& x0, int& x1, int& z0, int& z1) {
        x0 = static_cast<int>(std::floor(t.min.x / kCellSize));
        x1 = static_cast<int>(std::floor(t.max.x / kCellSize));
        z0 = static_cast<int>(std::floor(t.min.z / kCellSize));
        z1 = static_cast<int>(std::floor(t.max.z / kCellSize));
    };
    const int gx0 = static_cast<int>(std::floor(bounds_min_.x / kCellSize));
    const int gx1 = static_cast<int>(std::floor(bounds_max_.x / kCellSize));
    const int gz0 = static_cast<int>(std::floor(bounds_min_.z / kCellSize));
    const int gz1 = static_cast<int>(std::floor(bounds_max_.z / kCellSize));
    const int64_t nx = static_cast<int64_t>(gx1) - gx0 + 1;
    const int64_t nz = static_cast<int64_t>(gz1) - gz0 + 1;
    if (nx > 0 && nz > 0 && nx * nz <= (int64_t{1} << 20)) {
        cell_x0_ = gx0;
        cell_z0_ = gz0;
        cell_nx_ = static_cast<int>(nx);
        cell_nz_ = static_cast<int>(nz);
        const size_t table = static_cast<size_t>(nx * nz);
        // A counting sort split over contiguous runs of triangles: run r's entries of a cell go after those of the runs before
        // it, which keeps every cell in ascending triangle order whatever the run count.
        const size_t runs = ParallelChunkCount(triangles_.size(), 16384);
        std::vector<std::vector<uint32_t>> counts(runs, std::vector<uint32_t>(table, 0));
        ParallelChunks(triangles_.size(), runs, [&](size_t run, size_t begin, size_t end) {
            std::vector<uint32_t>& count = counts[run];
            int x0, x1, z0, z1;
            for (size_t i = begin; i < end; ++i) {
                cells(triangles_[i], x0, x1, z0, z1);
                for (int x = x0; x <= x1; ++x) {
                    for (int z = z0; z <= z1; ++z) {
                        ++count[static_cast<size_t>(x - gx0) * cell_nz_ + (z - gz0)];
                    }
                }
            }
        });
        cell_start_.assign(table + 1, 0);
        uint32_t total = 0;
        for (size_t c = 0; c < table; ++c) {
            cell_start_[c] = total;
            for (size_t run = 0; run < runs; ++run) {
                const uint32_t n = counts[run][c];
                counts[run][c] = total;
                total += n;
            }
        }
        cell_start_[table] = total;
        cell_items_.resize(total);
        ParallelChunks(triangles_.size(), runs, [&](size_t run, size_t begin, size_t end) {
            std::vector<uint32_t>& next = counts[run];
            int x0, x1, z0, z1;
            for (size_t i = begin; i < end; ++i) {
                cells(triangles_[i], x0, x1, z0, z1);
                for (int x = x0; x <= x1; ++x) {
                    for (int z = z0; z <= z1; ++z) {
                        cell_items_[next[static_cast<size_t>(x - gx0) * cell_nz_ + (z - gz0)]++] = static_cast<uint32_t>(i);
                    }
                }
            }
        });
        return;
    }
    // too wide for the table: a map from each cell to its run of the array
    std::unordered_map<int64_t, std::vector<uint32_t>> grid;
    int x0, x1, z0, z1;
    for (uint32_t i = 0; i < triangles_.size(); ++i) {
        cells(triangles_[i], x0, x1, z0, z1);
        for (int x = x0; x <= x1; ++x) {
            for (int z = z0; z <= z1; ++z) {
                grid[CellKey(x, z)].push_back(i);
            }
        }
    }
    for (const auto& [key, items] : grid) {
        sparse_cells_[key] = {static_cast<uint32_t>(cell_items_.size()), static_cast<uint32_t>(items.size())};
        cell_items_.insert(cell_items_.end(), items.begin(), items.end());
    }
}

std::span<const uint32_t> CollisionWorld::Cell(int x, int z) const {
    if (cell_nx_ > 0) {
        if (x < cell_x0_ || z < cell_z0_ || x - cell_x0_ >= cell_nx_ || z - cell_z0_ >= cell_nz_) {
            return {};
        }
        const size_t index = static_cast<size_t>(x - cell_x0_) * cell_nz_ + (z - cell_z0_);
        return std::span<const uint32_t>(cell_items_.data() + cell_start_[index], cell_start_[index + 1] - cell_start_[index]);
    }
    const auto it = sparse_cells_.find(CellKey(x, z));
    if (it == sparse_cells_.end()) {
        return {};
    }
    return std::span<const uint32_t>(cell_items_.data() + it->second.first, it->second.second);
}

void CollisionWorld::Query(const glm::vec3& min, const glm::vec3& max, std::vector<uint32_t>& out) const {
    out.clear();
    if (++stamp_value_ == 0) {
        std::fill(stamp_.begin(), stamp_.end(), 0);
        stamp_value_ = 1;
    }
    const int x0 = static_cast<int>(std::floor(min.x / kCellSize));
    const int x1 = static_cast<int>(std::floor(max.x / kCellSize));
    const int z0 = static_cast<int>(std::floor(min.z / kCellSize));
    const int z1 = static_cast<int>(std::floor(max.z / kCellSize));
    for (int x = x0; x <= x1; ++x) {
        for (int z = z0; z <= z1; ++z) {
            for (uint32_t i : Cell(x, z)) {
                if (stamp_[i] == stamp_value_) {
                    continue;
                }
                stamp_[i] = stamp_value_;
                const CollisionTriangle& t = triangles_[i];
                if (!owner_active_[t.owner]) {
                    continue;
                }
                if (t.max.x < min.x || t.min.x > max.x || t.max.y < min.y || t.min.y > max.y || t.max.z < min.z || t.min.z > max.z) {
                    continue;
                }
                out.push_back(i);
            }
        }
    }
}

bool CollisionWorld::Raycast(const glm::vec3& origin, const glm::vec3& direction, float max_distance, RayHit& hit, bool respect_winding) const {
    hit.distance = max_distance;
    if (triangles_.empty() || !(max_distance > 0.0f)) {
        return false;
    }
    // Walk only the x/z cells the ray crosses, nearest first, so a long camera probe costs its path rather than its bounding box.
    float t0 = 0.0f;
    float t1 = max_distance;
    const float lo[2] = {bounds_min_.x - 0.01f, bounds_min_.z - 0.01f};
    const float hi[2] = {bounds_max_.x + 0.01f, bounds_max_.z + 0.01f};
    const float o[2] = {origin.x, origin.z};
    const float d[2] = {direction.x, direction.z};
    for (int k = 0; k < 2; ++k) {
        if (std::abs(d[k]) < 1e-12f) {
            if (o[k] < lo[k] || o[k] > hi[k]) {
                return false;
            }
            continue;
        }
        float a = (lo[k] - o[k]) / d[k];
        float b = (hi[k] - o[k]) / d[k];
        if (a > b) {
            std::swap(a, b);
        }
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
    }
    if (t0 > t1) {
        return false;
    }
    if (++stamp_value_ == 0) {
        std::fill(stamp_.begin(), stamp_.end(), 0);
        stamp_value_ = 1;
    }
    const glm::vec3 entry = origin + direction * t0;
    int cell[2] = {static_cast<int>(std::floor(entry.x / kCellSize)), static_cast<int>(std::floor(entry.z / kCellSize))};
    int step[2] = {0, 0};
    float next[2] = {INFINITY, INFINITY};
    float delta[2] = {INFINITY, INFINITY};
    for (int k = 0; k < 2; ++k) {
        if (std::abs(d[k]) < 1e-12f) {
            continue;
        }
        step[k] = d[k] > 0.0f ? 1 : -1;
        const float boundary = (cell[k] + (step[k] > 0 ? 1 : 0)) * kCellSize;
        next[k] = (boundary - o[k]) / d[k];
        delta[k] = kCellSize / std::abs(d[k]);
    }
    bool found = false;
    for (;;) {
        {
            for (uint32_t i : Cell(cell[0], cell[1])) {
                if (stamp_[i] == stamp_value_) {
                    continue;
                }
                stamp_[i] = stamp_value_;
                const CollisionTriangle& t = triangles_[i];
                if (!owner_active_[t.owner]) {
                    continue;
                }
                if (respect_winding && !(t.shape_flags & 0x200) && glm::dot(t.normal, direction) >= 0.0f) {
                    continue;
                }
                const glm::vec3 e1 = t.b - t.a;
                const glm::vec3 e2 = t.c - t.a;
                const glm::vec3 p = glm::cross(direction, e2);
                const float det = glm::dot(e1, p);
                if (std::abs(det) < 1e-9f) {
                    continue;
                }
                const float inv = 1.0f / det;
                const glm::vec3 s = origin - t.a;
                const float u = glm::dot(s, p) * inv;
                if (u < 0.0f || u > 1.0f) {
                    continue;
                }
                const glm::vec3 q = glm::cross(s, e1);
                const float v = glm::dot(direction, q) * inv;
                if (v < 0.0f || u + v > 1.0f) {
                    continue;
                }
                const float distance = glm::dot(e2, q) * inv;
                if (distance >= 0.0f && distance < hit.distance) {
                    hit.distance = distance;
                    hit.point = origin + direction * distance;
                    hit.normal = glm::dot(t.normal, direction) < 0.0f ? t.normal : -t.normal;
                    hit.triangle = i;
                    found = true;
                }
            }
        }
        const int axis = next[0] < next[1] ? 0 : 1;
        const float leave = next[axis];
        // Every triangle a hit before this cell's exit could come from is registered in a cell already visited.
        if (leave > t1 || (found && hit.distance <= leave) || !std::isfinite(leave)) {
            break;
        }
        cell[axis] += step[axis];
        next[axis] += delta[axis];
    }
    return found;
}

void CollisionWorld::SphereOverlap(const glm::vec3& center, float radius, std::vector<SphereHit>& hits) const {
    hits.clear();
    Query(center - glm::vec3(radius + 0.01f), center + glm::vec3(radius + 0.01f), candidates_);
    for (uint32_t i : candidates_) {
        const CollisionTriangle& t = triangles_[i];
        const float distance = glm::dot(t.normal, center - t.a);
        if (distance < 0.0f || distance > radius) {
            continue;
        }
        const glm::vec3 projected = center - t.normal * distance;
        if (InsideTriangle(projected, t)) {
            hits.push_back({distance * distance, projected, t.normal, false});
            continue;
        }
        const glm::vec3 q = ClosestOnEdges(center, t);
        const glm::vec3 delta = center - q;
        const float sq = glm::dot(delta, delta);
        if (radius > 0.0f && sq < radius * radius) {
            hits.push_back({sq, q, sq > 1e-12f ? delta / std::sqrt(sq) : t.normal, true});
        }
    }
    SortHits(hits);
}

void CollisionWorld::SphereCast(const glm::vec3& from, const glm::vec3& to, float radius, std::vector<SphereHit>& hits) const {
    hits.clear();
    const glm::vec3 d = to - from;
    const float length_sq = glm::dot(d, d);
    Query(glm::min(from, to) - glm::vec3(radius + 0.01f), glm::max(from, to) + glm::vec3(radius + 0.01f), candidates_);
    for (uint32_t i : candidates_) {
        const CollisionTriangle& t = triangles_[i];
        const float start = glm::dot(t.normal, from - t.a);
        if (start < 0.0f) {
            continue;
        }
        if (start < radius) {
            const glm::vec3 projected = from - t.normal * start;
            if (InsideTriangle(projected, t)) {
                hits.push_back({start - radius, projected, t.normal, false});
                continue;
            }
            const glm::vec3 q = ClosestOnEdges(from, t);
            const float distance = glm::length(from - q);
            if (distance < radius) {
                hits.push_back({distance - radius, q, distance > 1e-6f ? (from - q) / distance : t.normal, true});
                continue;
            }
        }
        const float approach = glm::dot(t.normal, d);
        float best = 2.0f;
        SphereHit hit;
        if (approach < 0.0f && start >= radius) {
            const float time = (start - radius) / -approach;
            const glm::vec3 point = from + d * time - t.normal * radius;
            if (time <= 1.0f && InsideTriangle(point, t)) {
                best = time;
                hit.point = point;
                hit.normal = t.normal;
            }
        }
        if (best > 1.0f && radius > 0.0f) {
            const glm::vec3* corners[3] = {&t.a, &t.b, &t.c};
            float time = 0.0f;
            for (int k = 0; k < 3; ++k) {
                if (SweepSegment(from, d, radius, *corners[k], *corners[(k + 1) % 3], time) && time < best) {
                    best = time;
                }
                if (SweepPoint(from, d, radius, *corners[k], time) && time < best) {
                    best = time;
                }
            }
            if (best <= 1.0f) {
                const glm::vec3 center = from + d * best;
                hit.point = ClosestOnEdges(center, t);
                const glm::vec3 delta = center - hit.point;
                const float length = glm::length(delta);
                hit.normal = length > 1e-6f ? delta / length : t.normal;
                hit.edge = true;
            }
        }
        if (best <= 1.0f) {
            hit.distance_sq = length_sq * best * best;
            hits.push_back(hit);
        }
    }
    SortHits(hits);
}

void CharacterController::Reset() {
    vertical_velocity = 0.0f;
    grounded = false;
    radius_ = shape.radius;
    contact_normal_ = previous_normal_ = glm::vec3(0.0f);
    contact_ = touching_ = false;
    body_y_ = position.y;
    body_rise_ = 0.0f;
    last_position_ = position;
    placed_ = true;
}

// 0xAFDAA0: one plane per contact within radius + 0.1 and within radius of the center height
void CharacterController::GatherContacts(const CollisionWorld& world, const glm::vec3& center, Contacts& contacts) {
    const float r = shape.radius;
    contacts.count = 0;
    contacts.touching = 0;
    world.SphereOverlap(center, r + 0.1f, hits_);
    float previous_sq = 0.0f;
    bool previous_floor = false;
    bool previous_touching = false;
    for (const SphereHit& hit : hits_) {
        const float dy = std::abs(hit.point.y - center.y);
        if (dy > r || !(shape.center_height < 1.4f || shape.center_height - radius_ >= 0.3f || dy <= 1.0f)) {
            continue;
        }
        const float sq = std::max(hit.distance_sq, 0.0f);
        if (contacts.count > 0 && std::abs(previous_sq - sq) < 0.001f) {
            if (previous_floor && hit.normal.y <= 0.707f) {
                --contacts.count;
                contacts.touching -= previous_touching ? 1 : 0;
            } else if (!previous_floor && hit.normal.y > 0.707f) {
                continue;
            }
        }
        bool covered = false;
        for (int i = 0; i < contacts.count && !covered; ++i) {
            const glm::vec3& n = contacts.normals[i];
            covered = (glm::dot(n, center) - contacts.distances[i]) * (glm::dot(n, hit.point) - contacts.distances[i]) <= 0.0f;
        }
        if (covered || contacts.count == kMaxContacts) {
            continue;
        }
        glm::vec3 normal = hit.normal;
        glm::vec3 point = hit.point;
        float distance = std::sqrt(sq);
        if (hit.edge && hit.normal.y <= 0.707f && point.y < center.y) {
            if (std::abs(normal.y) < 0.95f) {
                normal = glm::normalize(glm::vec3(normal.x, 0.0f, normal.z));
            }
            point.y = center.y;
            distance = glm::dot(normal, center) - glm::dot(normal, point);
        }
        previous_touching = distance <= r + 0.01f;
        contacts.touching += previous_touching ? 1 : 0;
        contacts.normals[contacts.count] = normal;
        contacts.distances[contacts.count] = glm::dot(normal, point);
        ++contacts.count;
        previous_sq = sq;
        previous_floor = hit.normal.y > 0.707f;
    }
}

// 0xAFE2B0: push out to radius + 0.01 from every plane, and how far the opposite planes push back
bool CharacterController::Resolve(const glm::vec3& center, const Contacts& contacts, glm::vec3& direction, float& amount, float& opposite) {
    if (contacts.touching < 1) {
        return false;
    }
    const float reach = shape.radius + 0.01f;
    contact_normal_ = contacts.normals[0];
    contact_ = touching_ = true;
    glm::vec3 push(0.0f);
    for (int i = 0; i < contacts.count; ++i) {
        const float depth = reach + contacts.distances[i] - glm::dot(center + push, contacts.normals[i]);
        if (depth >= 0.0f) {
            push += contacts.normals[i] * depth;
            const glm::vec3 delta = previous_normal_ - contacts.normals[i];
            if (glm::dot(delta, delta) < 1e-5f) {
                contact_normal_ = previous_normal_;
            }
        }
    }
    direction = glm::dot(push, push) > 0.0001f ? glm::normalize(push) : glm::vec3(0.0f);
    float along = 0.0f;
    bool behind = false;
    for (int i = 0; i < contacts.count; ++i) {
        const float distance = glm::dot(center, contacts.normals[i]) - contacts.distances[i];
        behind = behind || distance < 0.0f;
        const float facing = glm::dot(direction, contacts.normals[i]);
        const float depth = reach - distance;
        if (facing >= 0.0001f && depth >= 0.0f) {
            along = std::max(along, depth / facing);
        }
    }
    if (behind) {
        return false;
    }
    glm::vec3 moved = center + direction * std::min(along, radius_);
    float length = glm::length(moved - center);
    if (length >= 0.0001f) {
        direction = (moved - center) / length;
    }
    if (length > radius_) {
        moved = center + direction * radius_;
        length = radius_;
    }
    opposite = -1.0f;
    for (int i = 0; i < contacts.count; ++i) {
        const float facing = glm::dot(direction, contacts.normals[i]);
        if (facing < 0.0f && -facing > 0.1f) {
            const float depth = reach - std::abs(glm::dot(moved, contacts.normals[i]) - contacts.distances[i]);
            if (depth >= 0.0001f) {
                opposite = std::max(opposite, depth / -facing);
            }
        }
    }
    amount = length;
    opposite = std::min(opposite, length);
    return true;
}

// 0xAFEB60: depenetrate, halfway between opposite planes, and the sphere radius for the next cast
float CharacterController::Depenetrate(const CollisionWorld& world, glm::vec3& center) {
    const glm::vec3 before = center;
    Contacts contacts;
    GatherContacts(world, center, contacts);
    if (contacts.count == 0) {
        return shape.radius;
    }
    glm::vec3 direction(0.0f);
    float amount = -1.0f;
    float opposite = -1.0f;
    if (Resolve(center, contacts, direction, amount, opposite)) {
        center += direction * (opposite != -1.0f ? (amount - opposite) * 0.5f : amount);
    }
    float nearest = 3.4e38f;
    for (int i = 0; i < contacts.count; ++i) {
        nearest = std::min(nearest, std::abs(glm::dot(center, contacts.normals[i]) - contacts.distances[i]));
    }
    if (std::getenv("PT_TRACE_COLLISION") && glm::length(center-before) > .035f) {
        LogInfo("depenetration: shift {:.4f}, nearest {:.4f}, cast radius {:.4f}, contacts {}", glm::length(center-before), nearest, radius_, contacts.count);
        for(int i=0;i<contacts.count;++i)
            LogInfo("contact: normal ({:.4f} {:.4f} {:.4f}), plane {:.4f}, before distance {:.4f}", contacts.normals[i].x,contacts.normals[i].y,contacts.normals[i].z,contacts.distances[i],glm::dot(before,contacts.normals[i])-contacts.distances[i]);
    }
    // Cast the same sphere that Resolve pushes out. Shrinking by 10 cm here let the
    // next movement penetrate the full-size sphere, then correction snapped it out.
    if (OriginalController()) {
        // 0xAFEB60: the next cast's radius, 10 cm inside the nearest contact plane, at least 5 cm
        /* The next cast sits 10 cm inside the nearest contact, never under 5 cm (0xAFEB60); that is what fits the player through the 0.796 m stair gap on f010. */
        return std::max(std::min(shape.radius, nearest - 0.1f), 0.05f);
    }
    return shape.radius;
}

// 0xAFED90: ground under a half-radius sphere, the center raised to center_height above it
void CharacterController::Ground(const CollisionWorld& world, glm::vec3& center) {
    const float probe = radius_ * shape.ground_probe;
    const glm::vec3 below = center - glm::vec3(0.0f, shape.center_height + radius_, 0.0f);
    world.SphereCast(center, below, probe, hits_);
    if (hits_.empty()) {
        world.SphereCast(center, below - glm::vec3(0.0f, 100.0f, 0.0f), 0.0f, hits_);
    }
    grounded = false;
    ground_y_ = -100.0f;
    for (const SphereHit& hit : hits_) {
        if (hit.normal.y >= 0.087f) {
            grounded = true;
            ground_y_ = hit.point.y;
            break;
        }
    }
    const float floor = ground_y_ + shape.center_height;
    if (center.y < floor) {
        world.SphereCast(center, glm::vec3(center.x, floor, center.z), probe, hits_);
        center.y = hits_.empty() ? floor : std::max(center.y, hits_.front().point.y - 0.01f);
        vertical_velocity = 0.0f;
    }
}

// Retain the original radius/backoff, but consume the remaining tangent motion at a wall or doorway edge.
// 0x12AD310: cast with the current radius, 8 mm back along the path from the hit
/* A hit stops 8 mm back along the path, as 0x12AD310 does; the depenetration next frame pushes out to radius + 0.01 from there. */
void CharacterController::SweepOriginal(const CollisionWorld& world, const glm::vec3& start, glm::vec3& end, const glm::vec3& requested) {
    const glm::vec3 path = end - start;
    world.SphereCast(start, end, radius_, hits_);
    const SphereHit* hit = nullptr;
    for (const SphereHit& h : hits_) {
        if (h.distance_sq >= 0.0f || glm::dot(path, h.normal) < 0.0f) {
            hit = &h;
            break;
        }
    }
    if (!hit) {
        if (touching_ && radius_ > 0.1f && glm::dot(previous_normal_, requested) < 0.0f) {
            end -= previous_normal_ * 0.05f;
        }
        touching_ = false;
    } else {
        const float length_sq = glm::dot(path, path);
        end = length_sq > 1e-6f ? start + path * std::sqrt(std::max(hit->distance_sq, 0.0f) / length_sq) : start;
        contact_normal_ = hit->normal;
        contact_ = touching_ = true;
        const glm::vec3 moved = end - start;
        const float facing = glm::dot(hit->normal, moved);
        if (facing != 0.0f) {
            end += moved * std::clamp(0.008f / facing, -1.0f, 0.0f);
        }
    }
    Ground(world, end);
    const glm::vec3 grounded_end = end;
    radius_ = Depenetrate(world, end);
    const glm::vec3 shift = end - grounded_end;
    if (glm::dot(shift, shift) >= 0.0001f) {
        Ground(world, end);
    }
}

void CharacterController::Sweep(const CollisionWorld& world, const glm::vec3& start, glm::vec3& end, const glm::vec3& requested) {
    if (OriginalController()) {
        SweepOriginal(world, start, end, requested);
        return;
    }
    glm::vec3 current = start;
    glm::vec3 target = end;
    for (int iteration = 0; iteration < 4; ++iteration) {
        const glm::vec3 path = target - current;
        const float length_sq = glm::dot(path, path);
        if (length_sq < 1e-12f) break;
        world.SphereCast(current, target, radius_, hits_);
        const SphereHit* hit = nullptr;
        for (const SphereHit& h : hits_) {
            if (h.distance_sq >= 0.0f || glm::dot(path, h.normal) < -1e-6f) {
                hit = &h;
                break;
            }
        }
        if (!hit) {
            current = target;
            break;
        }
        const glm::vec3 before = current;
        current += path * std::clamp(std::sqrt(std::max(hit->distance_sq, 0.0f) / length_sq), 0.0f, 1.0f);
        contact_normal_ = hit->normal;
        contact_ = touching_ = true;
        const glm::vec3 moved = current - before;
        const float facing = glm::dot(hit->normal, moved);
        if (facing != 0.0f) current += moved * std::clamp(0.008f / facing, -1.0f, 0.0f);
        glm::vec3 remaining = target - current;
        remaining -= hit->normal * std::min(glm::dot(remaining, hit->normal), 0.0f);
        target = current + remaining;
    }
    end = current;
    Ground(world, end);
    const glm::vec3 grounded_end = end;
    radius_ = Depenetrate(world, end);
    const glm::vec3 shift = end - grounded_end;
    if (glm::dot(shift, shift) >= 0.0001f) {
        Ground(world, end);
    }
}

void CharacterController::Move(const CollisionWorld& world, const glm::vec3& displacement, float dt, float gravity_dt) {
    const glm::vec3 begin = position;
    if (!placed_ || position != last_position_) {
        const float velocity = vertical_velocity;
        Reset();
        vertical_velocity = velocity;
    }
    const float ticks = std::min(dt * 300.0f, 20.0f);
    const glm::vec3 up(0.0f, shape.center_height, 0.0f);
    vertical_velocity -= shape.gravity * gravity_dt;
    glm::vec3 move = displacement + glm::vec3(0.0f, vertical_velocity * dt, 0.0f);
    glm::vec3 center = position + up;
    const glm::vec3 start = center;
    radius_ = Depenetrate(world, center);
    if (contact_) {
        const float into = glm::dot(move, contact_normal_);
        if (into < 0.0f) {
            move -= contact_normal_ * (into * 1.0001f);
        }
    }
    contact_ = false;
    // the original adds the move to the center before the depenetration (0xB001E0)
    glm::vec3 end = (OriginalController() ? start : center) + move;
    Sweep(world, center, end, displacement);
    position = end - up;
    // 0xB010B0: rises eased by +0x3C8 with +0x3CC acceleration, drops at once
    previous_normal_ = contact_normal_;
    if (position.y <= body_y_) {
        body_y_ = position.y;
        body_rise_ = 0.0f;
    } else {
        const float rate = ticks * 0.2f;
        const float y = std::min(body_y_ + body_rise_ + rate * (position.y - body_y_) * shape.rise_rate, position.y);
        body_rise_ = position.y - y <= 0.0f ? std::abs(position.y - body_y_) : body_rise_ + rate * shape.rise_accel;
        body_y_ = y;
    }
    last_position_ = position;
    static const bool trace = std::getenv("PT_TRACE_COLLISION") != nullptr;
    const float moved_xz = glm::length(glm::vec2(position.x-begin.x,position.z-begin.z));
    const float requested_xz = glm::length(glm::vec2(displacement.x,displacement.z));
    if(trace && moved_xz > requested_xz + .035f)
        LogInfo("collision correction: requested {:.4f}, moved {:.4f}, from ({:.3f} {:.3f} {:.3f}) to ({:.3f} {:.3f} {:.3f}), radius {:.3f}",
                requested_xz,moved_xz,begin.x,begin.y,begin.z,position.x,position.y,position.z,radius_);
}

}
