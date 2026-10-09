#include "engine/platform/input.h"
#include "engine/platform/virtual_pad.h"
#include "game/ui/ui_icons.h"
#include <SDL3/SDL.h>
#include <array>
#include <cstdio>
#include <string_view>
#include <utility>
int main(){
    pt::VirtualPads::UseOnlyVirtualDevices();if(!SDL_Init(SDL_INIT_GAMEPAD))return 2;
    pt::InputDevice input;input.settings.rumble=false;input.Init();pt::VirtualPads pads;int failed=0;
    auto poll=[&](pt::MouseUse mouse=pt::MouseUse::Look){SDL_UpdateGamepads();SDL_Event e;while(SDL_PollEvent(&e))input.ProcessEvent(e);return input.Poll(false,mouse);};
    auto check=[&](bool ok,const char* label){std::printf("%s %s\n",ok?"PASS":"FAIL",label);failed+=!ok;};
    constexpr std::array<uint16_t,13> steam_products={0x1101,0x1102,0x1105,0x1106,0x1142,0x11ff,0x1201,0x1202,0x1205,0x1302,0x1303,0x1304,0x1305};
    bool steam_ids=true;for(const uint16_t product:steam_products)steam_ids&=pt::IsSteamGamepadId(0x28de,product);
    check(steam_ids&&pt::IsSteamGamepadId(0x0f0d,0x01ab)&&pt::IsSteamGamepadId(0x0f0d,0x0196),"Valve Steam and both Hori Steam ID families are recognized");
    check(!pt::IsSteamGamepadId(0x045e,0x0b13)&&!pt::IsSteamGamepadId(0x0f0d,0x0124),"ordinary Xbox and Hori gamepads keep their existing prompt family");
    pt::PromptStyle steam_style;steam_style.device=pt::PromptDevice::Steam;steam_style.faces={'A','B','X','Y'};
    check(pt::PromptDeviceName(steam_style.device)==std::string_view("Steam")&&
          pt::game::SteamPromptGlyphName({pt::game::PromptButton::Cross},steam_style)=="steam:face:A"&&
          pt::game::SteamPromptGlyphName({pt::game::PromptButton::R1},steam_style)=="steam:bumper:R1"&&
          pt::game::SteamPromptGlyphName({pt::game::PromptButton::Options},steam_style)=="steam:menu",
          "Steam prompts use project-owned face, shoulder and menu glyphs");
    check(pads.Attach(0,"steam"),"virtual Steam Deck controller attaches");poll();
    check(input.Prompts().device==pt::PromptDevice::Steam&&input.Prompts().faces==std::array<char,4>{'A','B','X','Y'},"Steam controller selects Steam prompts with A/B/X/Y labels");
    SDL_Event mouse_motion{};mouse_motion.type=SDL_EVENT_MOUSE_MOTION;mouse_motion.motion.xrel=20;input.ProcessEvent(mouse_motion);
    poll(pt::MouseUse::Look);
    check(input.Prompts().device==pt::PromptDevice::Steam,"mouse-look motion does not replace controller prompts during play");
    pads.SetButton(0,"south",true);poll(); // process the controller press before the newer key press
    input.InjectKey(SDL_SCANCODE_W,true);const pt::InputState mixed=poll();
    check(input.Prompts().device==pt::PromptDevice::Keyboard&&mixed.left_stick.y>0.5f&&(mixed.raw_held&pt::kRawCross),"keyboard and Steam controller input work together while prompts follow the key press");
    input.InjectKey(SDL_SCANCODE_W,false);pads.SetButton(0,"south",false);poll();pads.Detach(0);poll();
    check(pads.Attach(0,"steam_virtual"),"virtual Steam Input gamepad attaches");poll();
    check(input.Prompts().device==pt::PromptDevice::Steam,"Steam Input virtual controller selects Steam prompts");pads.Detach(0);poll();
    check(pads.Attach(0,"hori_steam"),"virtual HORIPAD for Steam attaches");poll();
    check(input.Prompts().device==pt::PromptDevice::Steam,"HORIPAD for Steam selects Steam prompts");pads.Detach(0);poll();
    pt::KeyPressLatch voice_key;
    SDL_Event key_event{};key_event.type=SDL_EVENT_KEY_DOWN;key_event.key.scancode=SDL_SCANCODE_J;
    voice_key.ProcessEvent(key_event,SDL_SCANCODE_J);
    key_event.type=SDL_EVENT_KEY_UP;voice_key.ProcessEvent(key_event,SDL_SCANCODE_J);
    check(voice_key.Consume(),"quick configured key tap survives down and up before consume");
    check(!voice_key.Consume(),"configured key press is consumed once");
    key_event.type=SDL_EVENT_KEY_DOWN;key_event.key.repeat=true;voice_key.ProcessEvent(key_event,SDL_SCANCODE_J);
    check(!voice_key.Consume(),"held key repeat does not retrigger");
    key_event.key.repeat=false;voice_key.ProcessEvent(key_event,SDL_SCANCODE_J);voice_key.Discard();
    check(!voice_key.Consume(),"hidden or frozen frame discards queued key press before resume");
    // Jack's controller fallback is the two triggers together. Exercise the SDL gamepad axes, not a synthetic InputState.
    for(const char* kind:{"xbox","ps5","switch"}){
        check(pads.Attach(0,kind),"virtual controller for voice chord attaches");poll();
        pads.SetAxis(0,"r2",0.49f);pads.SetAxis(0,"l2",1.0f);
        check(!poll().voice_keyword_pressed,"trigger values below half do not complete the chord");
        pads.SetAxis(0,"r2",1.0f);check(poll().voice_keyword_pressed,"R2 completes an L2-first chord");
        check(!poll().voice_keyword_pressed,"held trigger chord fires once");
        pads.SetAxis(0,"l2",0.0f);check(!poll().voice_keyword_pressed,"releasing one trigger does not call Jack");
        pads.SetAxis(0,"l2",1.0f);check(poll().voice_keyword_pressed,"repressing a released trigger rearms the chord");
        pads.SetAxis(0,"r2",0.0f);check(!poll().voice_keyword_pressed,"one released trigger rearms without firing");
        pads.SetAxis(0,"l2",0.0f);poll();pads.SetAxis(0,"r2",1.0f);
        check(!poll().voice_keyword_pressed,"R2 alone does not call Jack");
        pads.SetAxis(0,"l2",1.0f);check(poll().voice_keyword_pressed,"L2 completes an R2-first chord");
        pads.SetAxis(0,"r2",0.0f);poll();pads.SetAxis(0,"l2",0.0f);poll();
        pads.SetAxis(0,"r2",1.0f);pads.SetAxis(0,"l2",1.0f);
        check(poll().voice_keyword_pressed,"active chord fires immediately before a controller disconnect");
        pads.SetAxis(0,"r2",0.0f);pads.SetAxis(0,"l2",0.0f);poll();
        pads.SetAxis(0,"r2",1.0f);pads.SetAxis(0,"l2",1.0f);SDL_UpdateGamepads();
        check(!input.Poll(false,pt::MouseUse::None,false).voice_keyword_pressed,"disabled controls discard a new chord");
        check(!poll().voice_keyword_pressed,"held chord does not fire on controls resume");
        pads.SetAxis(0,"r2",0.0f);poll();pads.SetAxis(0,"r2",1.0f);
        check(poll().voice_keyword_pressed,"release after resume rearms the chord");
        pads.Detach(0);check(!poll().voice_keyword_pressed,"disconnecting a held chord clears its edge state");
    }
    // The chord must come from one physical controller, regardless of the order of connected pads.
    check(pads.Attach(0,"xbox")&&pads.Attach(1,"ps5"),"two virtual controllers attach");poll();
    pads.SetAxis(0,"r2",1.0f);poll();pads.SetAxis(1,"l2",1.0f);
    check(!poll().voice_keyword_pressed,"triggers on separate controllers do not form a chord");
    pads.SetAxis(0,"r2",0.0f);pads.SetAxis(1,"l2",0.0f);poll();
    pads.DetachAll();poll();
    input.InjectKey(SDL_SCANCODE_X,true);check((poll().pressed&pt::kPadGouge)!=0,"keyboard X gouges");check((poll().pressed&pt::kPadGouge)==0,"holding X does not repeat");input.InjectKey(SDL_SCANCODE_X,false);poll();
    // the original gouges with its interact button (`Action`, TrapSystem::GougePressed reads kPadGouge or kPadAction): the Act keys press that bit
    input.InjectMouseButton(SDL_BUTTON_LEFT,true);check((poll().pressed&pt::kPadAction)!=0,"left mouse is the action, which gouges");input.InjectMouseButton(SDL_BUTTON_LEFT,false);poll();
    for(const SDL_Scancode act:{SDL_SCANCODE_E,SDL_SCANCODE_RETURN,SDL_SCANCODE_SPACE}){
        input.InjectKey(act,true);const uint32_t pressed=poll().pressed;check((pressed&pt::kPadAction)!=0&&(pressed&pt::kPadGouge)==0,"an Act key is the action, not the X key");input.InjectKey(act,false);poll();
    }
    // the gouge is X on every device: PlayStation cross (south), Xbox X (west), Switch X (north); every other face button is not
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
