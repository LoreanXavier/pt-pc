#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

#include "engine/physics/collision_world.h"

int main() {
    // A back-facing furniture plane precedes a front-facing wall along +Z.
    std::array<pt::GeomTriangle, 2> triangles{};
    triangles[0].a = {-2, -2, 1};
    triangles[0].b = {2, -2, 1};
    triangles[0].c = {0, 2, 1};
    triangles[1].a = {-2, -2, 2};
    triangles[1].b = {0, 2, 2};
    triangles[1].c = {2, -2, 2};
    pt::CollisionWorld world;
    const auto build = [&] {
        world.Clear();
        world.AddTriangles(triangles, glm::mat4(1.0f), -1);
        world.Build();
    };
    build();
    int failures = 0;
    const auto check = [&](bool reflection, float distance) {
        pt::RayHit hit;
        if (!world.Raycast({0, 0, 0}, {0, 0, 1}, 3.0f, hit, reflection) ||
            std::abs(hit.distance - distance) > 1e-6f) {
            ++failures;
        }
    };
    check(false, 1.0f);
    check(true, 2.0f);
    triangles[0].shape_flags = 0x200;
    build();
    check(true, 1.0f);
    pt::RayHit hit;
    if (world.Raycast({0, 0, 0}, {0, 0, 1}, 0.5f, hit, true)) {
        ++failures;
    }
    std::array<pt::GeomTriangle, 4> wall_floor{};
    wall_floor[0].a = {0, -2, -20}; wall_floor[0].b = {0, 5, -20}; wall_floor[0].c = {0, 5, 20};
    wall_floor[1].a = {0, -2, -20}; wall_floor[1].b = {0, 5, 20}; wall_floor[1].c = {0, -2, 20};
    wall_floor[2].a = {-20, 0, -20}; wall_floor[2].b = {-20, 0, 20}; wall_floor[2].c = {20, 0, 20};
    wall_floor[3].a = {-20, 0, -20}; wall_floor[3].b = {20, 0, 20}; wall_floor[3].c = {20, 0, -20};
    world.Clear(); world.AddTriangles(wall_floor, glm::mat4(1.0f), -1); world.Build();
    // The decompiled controller (the default) stops a move at its first hit, 8 mm before it (0x12AD310), and takes the
    // part into the wall off the next frame's move (0xB001E0), so a push along a wall slides from the second frame on;
    // the sliding variant (PT_CONTROLLER_SLIDE=1) slides within the frame of the hit
    const bool original = pt::OriginalController();
    std::printf("controller: %s\n", original ? "decompiled" : "sliding variant");
    pt::CharacterController player;
    player.position = {1, 0, 0};
    player.Move(world, {-1, 0, .5f}, 1.0f / 30.0f, 0);
    const bool slides = player.position.x >= .39f && (original || player.position.z > .49f);
    std::printf("wall hit: %s, position %.4f %.4f %.4f\n", slides ? "PASS" : "FAIL", player.position.x, player.position.y, player.position.z);
    failures += !slides;
    const auto movement_check = [&](const char* name, bool ok) {
        std::printf("%s: %s, position %.4f %.4f %.4f\n", name, ok ? "PASS" : "FAIL", player.position.x, player.position.y, player.position.z);
        failures += !ok;
    };
    player.Reset(); player.position = {1, 0, 0};
    player.Move(world, {-10, 0, 2}, 1.0f / 30.0f, 0);
    movement_check("large displacement stays outside wall", player.position.x >= .39f && (original || player.position.z > 1.99f));
    const float slide_start = player.position.z;
    for (int i = 0; i < 120; ++i) player.Move(world, {-.02f, 0, .02f}, 1.0f / 60.0f, 0);
    movement_check("sustained wall slide", player.position.x >= .39f && player.position.z > slide_start + 2.35f);
    player.Move(world, {.2f, 0, 0}, 1.0f / 30.0f, 0);
    movement_check("leave wall contact", player.position.x > .59f);

    std::array<pt::GeomTriangle, 2> corner{};
    corner[0].a = {-20, -2, 3}; corner[0].b = {-20, 5, 3}; corner[0].c = {20, 5, 3};
    corner[1].a = {-20, -2, 3}; corner[1].b = {20, 5, 3}; corner[1].c = {20, -2, 3};
    world.AddTriangles(corner, glm::mat4(1.0f), -1); world.Build();
    player.Reset(); player.position = {1, 0, 0};
    player.Move(world, {-3, 0, 5}, 1.0f / 30.0f, 0);
    movement_check("inside corner blocks both planes", player.position.x >= .39f && player.position.z <= 2.61f && (original || player.position.z > 2.5f));

    // A 0.9 m opening leaves 0.1 m clearance around the 0.8 m player diameter.
    std::array<pt::GeomTriangle, 4> doorway{};
    doorway[0].a = {-.45f, -2, -20}; doorway[0].b = {-.45f, 5, -20}; doorway[0].c = {-.45f, 5, 20};
    doorway[1].a = {-.45f, -2, -20}; doorway[1].b = {-.45f, 5, 20}; doorway[1].c = {-.45f, -2, 20};
    doorway[2].a = {.45f, -2, -20}; doorway[2].b = {.45f, 5, 20}; doorway[2].c = {.45f, 5, -20};
    doorway[3].a = {.45f, -2, -20}; doorway[3].b = {.45f, -2, 20}; doorway[3].c = {.45f, 5, 20};
    world.Clear(); world.AddTriangles(doorway, glm::mat4(1.0f), -1);
    world.AddTriangles(std::span(wall_floor).subspan(2), glm::mat4(1.0f), -1); world.Build();
    player.Reset(); player.position = {0, 0, 0};
    for (int i = 0; i < 120; ++i) player.Move(world, {.01f, 0, .02f}, 1.0f / 60.0f, 0);
    movement_check("narrow opening while pressing jamb", std::abs(player.position.x) <= .061f && player.position.z > 2.35f);
    if (original) {
        // the 0.796 m gap at the f010 stairs (x -11.53, gameplay.md 10.3) is narrower than the 0.8 m sphere: the original
        // passes it by casting a sphere 10 cm inside the nearest plane (0xAFEB60)
        for (pt::GeomTriangle& t : doorway) {
            for (glm::vec3* v : {&t.a, &t.b, &t.c}) v->x = v->x < 0.0f ? -.398f : .398f;
        }
        world.Clear(); world.AddTriangles(doorway, glm::mat4(1.0f), -1);
        world.AddTriangles(std::span(wall_floor).subspan(2), glm::mat4(1.0f), -1); world.Build();
        player.Reset(); player.position = {0, 0, 0};
        for (int i = 0; i < 120; ++i) player.Move(world, {0, 0, .02f}, 1.0f / 60.0f, 0);
        movement_check("stair gap narrower than the sphere", player.position.z > 2.35f);
    }
    // Finite door jambs exercise the rounded entry corners, unlike the parallel-wall test above.
    std::vector<pt::GeomTriangle> jambs;
    const auto quad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d) {
        pt::GeomTriangle t{}; t.a=a; t.b=b; t.c=c; jambs.push_back(t);
        t.a=a; t.b=c; t.c=d; jambs.push_back(t);
    };
    for (int side : {-1, 1}) {
        const float inner=side*.45f, outer=side*4.0f;
        const float lo=std::min(inner,outer), hi=std::max(inner,outer);
        quad({lo,-2,-.2f},{lo,5,-.2f},{hi,5,-.2f},{hi,-2,-.2f});
        quad({lo,-2,.2f},{hi,-2,.2f},{hi,5,.2f},{lo,5,.2f});
        if (side < 0) quad({inner,-2,-.2f},{inner,5,-.2f},{inner,5,.2f},{inner,-2,.2f});
        else quad({inner,-2,-.2f},{inner,-2,.2f},{inner,5,.2f},{inner,5,-.2f});
    }
    world.Clear(); world.AddTriangles(jambs,glm::mat4(1),-1);
    world.AddTriangles(std::span(wall_floor).subspan(2),glm::mat4(1),-1); world.Build();
    for (float x : {-.3f,-.15f,0.15f,.3f}) {
        player.Reset(); player.position={x,0,-2};
        float max_forward=0, max_step=0; int stalled=0;
        for (int i=0;i<240;++i) {
            const glm::vec3 before=player.position;
            const float dx=player.position.z < -.65f ? -x*.004f : 0.0f;
            player.Move(world,{dx,0,.02f},1.0f/60,0);
            const float dz=player.position.z-before.z;
            max_step=std::max(max_step,glm::length(player.position-before));
            max_forward=std::max(max_forward,dz); stalled+=dz<.001f;
        }
        const bool ok=player.position.z>2.0f && max_forward<=.035f && max_step<=.035f;
        std::printf("finite jamb entry %.2f: %s; max forward %.5f, max step %.5f, stalled %d, end %.3f %.3f\n",x,ok?"PASS":"FAIL",max_forward,max_step,stalled,player.position.x,player.position.z);
        failures+=!ok;
    }
    // Two angled opposing jamb contacts from the captured f060 correction.
    std::vector<pt::GeomTriangle> angled;
    for(glm::vec3 n : {glm::normalize(glm::vec3(-.2377f,0,.9713f)),glm::normalize(glm::vec3(-.4068f,0,-.9135f))}) {
        const glm::vec3 t=glm::vec3(n.z,0,-n.x)*10.0f;
        const glm::vec3 c=-n*.41f;
        const glm::vec3 a=c-t+glm::vec3(0,-2,0), b=c+t+glm::vec3(0,-2,0);
        const glm::vec3 d=c-t+glm::vec3(0,5,0), e=c+t+glm::vec3(0,5,0);
        pt::GeomTriangle g{};g.a=a;g.b=b;g.c=e;angled.push_back(g);
        g.a=a;g.b=e;g.c=d;angled.push_back(g);
    }
    world.Clear();world.AddTriangles(angled,glm::mat4(1),-1);
    world.AddTriangles(std::span(wall_floor).subspan(2),glm::mat4(1),-1);world.Build();
    player.Reset();player.position={0,0,0};
    float largest=0;
    for(int i=0;i<120;++i) {
        const auto before=player.position;
        player.Move(world,{.015f,0,.023f},1.0f/60,0);
        largest=std::max(largest,glm::length(player.position-before));
    }
    std::printf("angled opposing jamb motion: %s, maximum step %.5f\n",largest<=.05f?"PASS":"FAIL",largest);
    failures+=largest>.05f;
    std::printf("reflection collision: %d failures\n", failures);
    return failures ? 1 : 0;
}
