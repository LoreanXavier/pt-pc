#include "engine/platform/input.h"
#include "engine/platform/virtual_pad.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <string_view>
#include <utility>
int main(){
    pt::VirtualPads::UseOnlyVirtualDevices();if(!SDL_Init(SDL_INIT_GAMEPAD))return 2;
    pt::InputDevice input;input.settings.rumble=false;input.Init();pt::VirtualPads pads;int failed=0;
    auto poll=[&](){SDL_UpdateGamepads();SDL_Event e;while(SDL_PollEvent(&e))input.ProcessEvent(e);return input.Poll(false,pt::MouseUse::Look);};
    auto check=[&](bool ok,const char* label){std::printf("%s %s\n",ok?"PASS":"FAIL",label);failed+=!ok;};
    input.InjectKey(SDL_SCANCODE_X,true);check((poll().pressed&pt::kPadGouge)!=0,"keyboard X gouges");check((poll().pressed&pt::kPadGouge)==0,"holding X does not repeat");input.InjectKey(SDL_SCANCODE_X,false);poll();
    input.InjectMouseButton(SDL_BUTTON_LEFT,true);check((poll().pressed&pt::kPadGouge)==0,"left mouse does not gouge");input.InjectMouseButton(SDL_BUTTON_LEFT,false);poll();
    const std::pair<const char*,const char*> kinds[]={{"ps5","cross"},{"xbox","square"},{"switch","triangle"}};
    for(const auto& [kind,gouge]:kinds){
        check(pads.Attach(0,kind),"virtual pad attaches");poll();
        for(const char* face:{"cross","circle","square","triangle"}){
            pads.SetButton(0,face,true);
            const bool expected=std::string_view(face)==gouge;
            const bool got=(poll().pressed&pt::kPadGouge)!=0;
            std::printf("  %s %s: gouge %d\n",kind,face,got?1:0);
            check(got==expected,expected?"the X button gouges":"another face button does not gouge");
            pads.SetButton(0,face,false);poll();
        }
        pads.Detach(0);poll();
    }
    pads.DetachAll();input.Shutdown();SDL_Quit();return failed?1:0;
}
