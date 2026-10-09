#ifdef NDEBUG
#undef NDEBUG
#endif
#include "engine/platform/display_modes.h"

#include <cassert>

int main() {
    const std::vector<glm::ivec2> listed{{1920, 1080}, {1280, 720}, {1920, 1080}, {0, 0}, {1600, 900}};
    const auto sizes = pt::UniqueDisplaySizes(listed);
    assert((sizes == std::vector<glm::ivec2>{{1280, 720}, {1600, 900}, {1920, 1080}}));
    assert((pt::ClosestDisplaySize(sizes, {1920, 1080}) == glm::ivec2(1920, 1080)));
    assert((pt::ClosestDisplaySize(sizes, {1900, 1050}) == glm::ivec2(1920, 1080)));
    assert((pt::ClosestDisplaySize({}, {1900, 1050}) == glm::ivec2(1900, 1050)));
}
