#include "engine/render/camera.h"
#include <cstdio>
int main() {
    int failures = 0;
    for (float angle : {0.2f, 0.8f, 1.6f, 3.0f}) {
        const auto from = glm::translate(glm::mat4(1), glm::vec3(.07f, 0, 8.2f));
        const auto to = from * glm::mat4_cast(glm::angleAxis(angle, glm::vec3(0,1,0)));
        const auto pose = glm::translate(glm::mat4(1), glm::vec3(.6f,.3f,8.2f));
        const auto from_skin = glm::inverse(from) * pose;
        const auto to_skin = glm::inverse(to) * pose;
        for (float t : {0.f,.25f,.5f,.75f,1.f}) {
            const auto world = pt::BlendTransform(from,to,t);
            const auto skin = pt::BlendSkinTransform(from,to,glm::inverse(world),from_skin,to_skin,t);
            const auto drawn = world * skin;
            for (int c=0;c<4;++c) if (glm::length(drawn[c]-pose[c])>1e-5f) { ++failures; break; }
        }
    }
    const auto a = glm::translate(glm::mat4(1), glm::vec3(.1f,0,8));
    const auto b = a * glm::mat4_cast(glm::angleAxis(1.5f,glm::vec3(0,1,0)));
    const auto pose_a = glm::translate(glm::mat4(1),glm::vec3(.6f,.3f,8.2f));
    const auto pose_b = glm::translate(glm::mat4(1),glm::vec3(.7f,.4f,8.3f));
    for (float t : {0.f,.25f,.5f,.75f,1.f}) {
        const auto world = pt::BlendTransform(a,b,t);
        const auto skin = pt::BlendSkinTransform(a,b,glm::inverse(world),glm::inverse(a)*pose_a,glm::inverse(b)*pose_b,t);
        const auto expected = pose_a + (pose_b-pose_a)*t;
        for (int c=0;c<4;++c) if (glm::length((world*skin)[c]-expected[c])>1e-5f) { ++failures; break; }
    }
    printf("compensated root/skin interpolation: %d failures\n",failures);
    return failures ? 1 : 0;
}
