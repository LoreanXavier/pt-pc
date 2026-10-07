#include "game/follower_shadow.h"
#include <cstdio>
int main(){
 std::vector<pt::SceneLight> lights(2);lights[0].type=pt::LightType::Point;lights[0].position={0,2,-1};lights[0].outer_range=8;lights[0].intensity={10,8,6};lights[0].id=7;
 lights[1].position={100,2,0};lights[1].outer_range=2;lights[1].intensity={100,100,100};
 pt::game::ApplyFollowerShadow(lights,{0,0,0});
 bool ok=lights.size()==3;
 if(ok)ok=lights[2].character_shadow_only && lights[2].cast_shadow && lights[2].shadow_strength==1 && glm::length(lights[0].intensity-glm::vec3(10,8,6))<1e-5f && lights[1].intensity.x==100 && lights[2].id!=7 && lights[2].outer_range>=20 && lights[2].diffuse_scale==0 && lights[2].specular_scale==0;
 std::printf("%s: nearby follower caster receives strong shadow without changing unoccluded lighting\n",ok?"PASS":"FAIL");
 std::vector<pt::SceneLight> far(1);far[0].position={100,0,0};far[0].outer_range=2;pt::game::ApplyFollowerShadow(far,{0,0,0});bool none=far.size()==1;std::printf("%s: distant lights remain unchanged\n",none?"PASS":"FAIL");
 return ok&&none?0:1;
}
