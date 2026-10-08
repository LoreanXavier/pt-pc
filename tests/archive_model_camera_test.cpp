#ifdef NDEBUG
#undef NDEBUG
#endif
#include "game/archive_model_camera.h"

#include <cassert>
#include <cmath>

using pt::game::PanArchiveModel;

int main() {
    const glm::vec3 home(1.0f, 2.0f, 3.0f);
    const glm::vec3 right(1.0f, 0.0f, 0.0f);
    const glm::vec3 up(0.0f, 1.0f, 0.0f);

    const glm::vec3 centered = PanArchiveModel(home, home, right, up, glm::vec2(0.0f), 1.0f, 2.0f, 1.0f);
    assert(centered == home);

    const glm::vec3 horizontal = PanArchiveModel(home, home, right, up, glm::vec2(1.0f, 0.0f), 0.5f, 2.0f, 1.0f);
    assert(horizontal.x > home.x && horizontal.y == home.y && horizontal.z == home.z);

    const glm::vec3 vertical = PanArchiveModel(home, home, right, up, glm::vec2(0.0f, -1.0f), 0.5f, 2.0f, 1.0f);
    assert(vertical.y > home.y && vertical.x == home.x && vertical.z == home.z);

    const glm::vec3 bounded = PanArchiveModel(home, home, right, up, glm::vec2(1.0f, -1.0f), 100.0f, 20.0f, 1.0f);
    assert(std::abs(glm::length(bounded - home) - 1.5f) < 1e-5f);

    const glm::vec3 invalid_dt = PanArchiveModel(home, home, right, up, glm::vec2(1.0f), 0.0f, 2.0f, 1.0f);
    assert(invalid_dt == home);
}
