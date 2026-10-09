#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/assets/geom.h"

namespace pt {

struct CollisionTriangle {
    glm::vec3 a;
    glm::vec3 b;
    glm::vec3 c;
    glm::vec3 normal;
    glm::vec3 min;
    glm::vec3 max;
    uint64_t tags = 0;
    uint32_t owner = 0;
    uint32_t shape_flags = 0;
    uint16_t prim_info = 0;
};

struct RayHit {
    float distance = 0.0f;
    glm::vec3 point{0.0f};
    glm::vec3 normal{0.0f};
    uint32_t triangle = 0;
};

// hit record +0x2C: squared distance or travel, the overlap depth as a negative length when the cast starts inside
struct SphereHit {
    float distance_sq = 0.0f;
    glm::vec3 point{0.0f};
    glm::vec3 normal{0.0f};
    bool edge = false;
};

bool OriginalController();

class CollisionWorld {
public:
    void Clear();
    void AddTriangles(std::span<const GeomTriangle> triangles, const glm::mat4& world, int node_filter, uint32_t owner = 0);
    // the world space triangle AddTriangles makes of t, false for a degenerate one (it adds none)
    static bool MakeTriangle(const GeomTriangle& t, const glm::mat4& world, CollisionTriangle& out);
    // triangles already in world space (MakeTriangle), each block with its owner set to the block's owner, in block order
    struct Block {
        const std::vector<CollisionTriangle>* triangles = nullptr;
        uint32_t owner = 0;
    };
    void AppendBlocks(std::span<const Block> blocks);
    uint32_t AddOwner(std::string name) {
        owners_.push_back(std::move(name));
        owner_active_.push_back(1);
        owner_triangles_.push_back(0);
        return static_cast<uint32_t>(owners_.size() - 1);
    }
    const std::string& OwnerName(uint32_t owner) const { return owners_[owner]; }
    // An inactive owner's triangles stay in the world and its grid but no query, ray or sweep sees them: a geom toggle
    // (BodyState::geom_active) is a flag here instead of a rebuild. The active triangles keep their relative order, so the
    // queries return what a world built from the active owners alone returns, in the same order.
    void SetOwnerActive(uint32_t owner, bool active) { owner_active_[owner] = active ? 1 : 0; }
    bool OwnerActive(uint32_t owner) const { return owner_active_[owner] != 0; }
    // the triangles of the active owners
    size_t ActiveTriangles() const;
    void Build();

    // Reflection query 0x80700: C11C20 rejects backfaces unless the shape is double-sided (normalized flag 0x200).
    bool Raycast(const glm::vec3& origin, const glm::vec3& direction, float max_distance, RayHit& hit, bool respect_winding = false) const;
    // 0xC0F470: one-sided triangles within the radius, nearest first
    void SphereOverlap(const glm::vec3& center, float radius, std::vector<SphereHit>& hits) const;
    // 0xC11C20: one-sided triangles touched by the swept sphere, in order along the path
    void SphereCast(const glm::vec3& from, const glm::vec3& to, float radius, std::vector<SphereHit>& hits) const;
    void Query(const glm::vec3& min, const glm::vec3& max, std::vector<uint32_t>& out) const;
    const std::vector<CollisionTriangle>& Triangles() const { return triangles_; }

private:
    static constexpr float kCellSize = 1.0f;
    static int64_t CellKey(int x, int z) { return (static_cast<int64_t>(x) << 32) ^ static_cast<uint32_t>(z); }
    // the triangles registered in cell (x, z), in ascending order
    std::span<const uint32_t> Cell(int x, int z) const;

    std::vector<CollisionTriangle> triangles_;
    std::vector<std::string> owners_{""};
    std::vector<uint8_t> owner_active_{1};
    std::vector<uint32_t> owner_triangles_{0};
    // The x/z grid as one array of triangle indices, cell after cell: a dense table of cell starts over the cells the triangles
    // cover (cell_x0_, cell_z0_, cell_nx_ by cell_nz_), or, where that table would be too large, a map from the cell key to its
    // start and count. Build used to fill a hash map of vectors, which took most of a rebuild's 6 to 25 ms.
    std::vector<uint32_t> cell_items_;
    std::vector<uint32_t> cell_start_;
    int cell_x0_ = 0;
    int cell_z0_ = 0;
    int cell_nx_ = 0;
    int cell_nz_ = 0;
    std::unordered_map<int64_t, std::pair<uint32_t, uint32_t>> sparse_cells_;
    glm::vec3 bounds_min_{0.0f};
    glm::vec3 bounds_max_{0.0f};
    mutable std::vector<uint32_t> stamp_;
    mutable uint32_t stamp_value_ = 0;
    mutable std::vector<uint32_t> candidates_;
};

// 0xB02700 defaults with the player values of 0x963570
struct CharacterShape {
    float radius = 0.4f;
    float center_height = 0.8f;
    float ground_probe = 0.5f;
    float gravity = 9.8f;
    float rise_rate = 0.25f;
    float rise_accel = 0.005f;
};

// Fox character controller 0xB001E0: one sphere at center_height above the feet
class CharacterController {
public:
    glm::vec3 position{0.0f};
    float vertical_velocity = 0.0f;
    bool grounded = false;
    CharacterShape shape;

    void Reset();
    // gravity_dt: the frame length on the first tick of a 29.97 fps frame, else 0
    void Move(const CollisionWorld& world, const glm::vec3& displacement, float dt, float gravity_dt);
    glm::vec3 BodyPosition() const { return glm::vec3(position.x, body_y_, position.z); }
    float Radius() const { return radius_; }

private:
    static constexpr int kMaxContacts = 32;
    struct Contacts {
        glm::vec3 normals[kMaxContacts];
        float distances[kMaxContacts];
        int count = 0;
        int touching = 0;
    };

    float Depenetrate(const CollisionWorld& world, glm::vec3& center);
    void GatherContacts(const CollisionWorld& world, const glm::vec3& center, Contacts& contacts);
    bool Resolve(const glm::vec3& center, const Contacts& contacts, glm::vec3& direction, float& amount, float& opposite);
    void Sweep(const CollisionWorld& world, const glm::vec3& start, glm::vec3& end, const glm::vec3& requested);
    void SweepOriginal(const CollisionWorld& world, const glm::vec3& start, glm::vec3& end, const glm::vec3& requested);
    void Ground(const CollisionWorld& world, glm::vec3& center);

    float radius_ = 0.4f;
    glm::vec3 contact_normal_{0.0f};
    glm::vec3 previous_normal_{0.0f};
    bool contact_ = false;
    bool touching_ = false;
    float ground_y_ = -100.0f;
    float body_y_ = 0.0f;
    float body_rise_ = 0.0f;
    glm::vec3 last_position_{0.0f};
    bool placed_ = false;
    std::vector<SphereHit> hits_;
};

}
