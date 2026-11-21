#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "engine/render/mesh.h"

namespace pt {
class ModelCache;
struct DrawItem;
}

namespace pt::game {

class Game;

struct StreetEdge {
    glm::vec2 a, b;
    glm::vec2 toward;
    bool fence;
};
std::span<const StreetEdge> StreetEdges();
struct StreetBody {
    glm::vec2 at, x_axis;
    float half_x, back, front;
    bool Holds(const glm::vec2& p) const;
};
std::span<const StreetBody> StreetBodies();
void DumpStreetWalk(Game& game, const std::string& path);

class StreetCity {
public:
    bool Ensure(ModelCache& models, uint32_t road_material, float road_tile);
    bool Up() const { return ready_; }
    void Sheets(ModelCache& models, bool night);
    void AddDraws(std::vector<DrawItem>& out) const;

private:
    struct Placed {
        const GpuMesh* mesh = nullptr;
        glm::mat4 transform{1.0f};
        glm::vec4 tint{1.0f};
    };
    std::vector<Placed> placed_;
    const GpuMesh* ground_ = nullptr;
    std::map<uint32_t, std::unique_ptr<GpuMesh>> grounds_;
    std::unique_ptr<GpuMesh> shell_;
    std::vector<bool> sheets_own_;
    bool tried_ = false;
    bool ready_ = false;
};

}
