#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "game/survival/survival_models.h"

namespace pt {
struct DrawItem;
struct GeomTriangle;
struct SceneLighting;
}

namespace pt::game {

class Game;

class StreetHouse {
public:
    enum class Entry : uint8_t { None, Porch };

    explicit StreetHouse(Game& game);
    ~StreetHouse();

    bool Ensure();
    std::vector<GeomTriangle> Walls() const;
    bool Hides(const DrawItem& item) const;
    void Begin(Entry entry);
    glm::vec3 EntryFeet() const;
    void Update(float dt);
    void AddDraws(std::vector<DrawItem>& out) const;
    void AddLights(SceneLighting& out, float street) const;

private:
    Game& game_;
    StreetHouseMeshes meshes_;
    const GpuMesh* pt_door_ = nullptr;
    glm::mat4 pt_door_fit_{1.0f};
    int32_t door_wood_ = -1;
    bool tried_ = false;
    bool ready_ = false;
    Entry entry_ = Entry::None;
    float open_ = 0.0f;
    bool slam_pending_ = false;
};

}
