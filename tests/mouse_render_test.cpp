#include "game/render_mouse.h"
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <vector>
#include "engine/fs/vfs.h"
int main(int argc,char** argv){
 int failures=0;
 for(int hz:{60,120,144,240}){
  float simulated=0, pending=0, shown=0;float accum=0;const float tick=1.0f/60;
  for(int frame=0;frame<hz*2;++frame){
   pending+=0.6f/hz;accum+=1.0f/hz;
   while(accum+1e-7f>=tick){simulated-=pending;pending=0;accum-=tick;}
   pt::Camera blended,current;blended.yaw=shown;current.yaw=simulated;
   auto camera=pt::game::RenderMouseLook(blended,current,{pending,0},false,false);
   if(std::abs((shown-camera.yaw)-0.6f/hz)>1e-5f)++failures;
   shown=camera.yaw;
  }
 }
 pt::Camera a,b;a.position={1,2,3};a.fov_y=.4f;b.yaw=.3f;b.pitch=.2f;
 auto c=pt::game::RenderMouseLook(a,b,{.1f,.05f},true,true);
 if(std::abs(c.yaw-.4f)>1e-6f || std::abs(c.pitch-.25f)>1e-6f || c.position!=a.position || c.fov_y!=a.fov_y)++failures;
 if(argc>1){
  pt::Vfs vfs;if(!vfs.Mount(argv[1]))return 2;vfs.LoadPackage("/Assets/sh/level/common/resident.fpk");vfs.LoadPackage("/Assets/sh/level/common/resident.fpkd");if(!pt::game::LoadPlayerAnimation(vfs))return 2;
  pt::game::Player player;player.Spawn(glm::mat4(1));pt::CollisionWorld world;pt::InputState input;input.left_stick={0,1};pt::game::PlayerFrameContext context;
  for(int i=0;i<120;++i)player.Update(1.0f/60,input,world,context);
  // Keyboard movement takes the camera's heading on every tick (Player::Update sets motion_.heading from the tick's stick
  // heading), while the character controller moves once per original 29.97 fps frame with the frame's summed displacement
  // (Player::UpdateBody, fc3975f). With the mouse turning the same way on every tick, the frame's step must point strictly
  // between the camera headings of the ticks it sums: a heading taken once per frame would put it on the arc's edge, a step a
  // frame late outside it. A step of a single tick (PT_CONTROLLER_TICK=1) points along that tick's heading. The pre-loop tick
  // may be part of the first step, so its heading opens the first arc.
  int steering_failures=0,steps=0;std::vector<glm::vec2> headings;
  auto heading=[&]{auto f=player.CameraForward();return glm::normalize(glm::vec2(f.x,f.z));};
  headings.push_back(heading());
  for(int i=0;i<120;++i){
   input.mouse_look={.01f,0};const auto before=player.Feet();player.Update(1.0f/60,input,world,context);
   headings.push_back(heading());
   auto delta=player.Feet()-before;delta.y=0;
   if(glm::length(delta)>1e-5f){
    ++steps;const glm::vec2 s=glm::normalize(glm::vec2(delta.x,delta.z));float lo=0,hi=0;
    for(const auto& h:headings){const float ang=std::atan2(s.x*h.y-s.y*h.x,glm::dot(s,h));lo=std::min(lo,ang);hi=std::max(hi,ang);}
    const float spread=hi-lo;
    // inside the arc, by a quarter of it on either side (the ticks' distances differ a little), and the arc is the turn's size
    // the first step may or may not take the pre-loop tick (the frame's phase): it is not judged
    if(steps>1 && (headings.size()>=2 ? (lo>-.25f*spread || hi<.25f*spread || spread>.01f*(headings.size()-1)+1e-4f) : (lo<-1e-4f || hi>1e-4f)))++steering_failures;
    headings.clear();
   }
  }
  if(steps<30)++steering_failures;
  printf("keyboard steering follows camera every tick: %d failures (%d steps)\n",steering_failures,steps);failures+=steering_failures;
 }
 printf("mouse display cadence/inversion: %d failures\n",failures);return failures?1:0;
}
