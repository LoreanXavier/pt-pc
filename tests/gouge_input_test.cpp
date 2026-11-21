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
    pads.SetButton(0,"south",true);poll();
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
    check(pads.Attach(0,"xbox")&&pads.Attach(1,"ps5"),"two virtual controllers attach");poll();
    pads.SetAxis(0,"r2",1.0f);poll();pads.SetAxis(1,"l2",1.0f);
    check(!poll().voice_keyword_pressed,"triggers on separate controllers do not form a chord");
    pads.SetAxis(0,"r2",0.0f);pads.SetAxis(1,"l2",0.0f);poll();
    pads.DetachAll();poll();
    input.InjectKey(SDL_SCANCODE_X,true);check((poll().pressed&pt::kPadGouge)!=0,"keyboard X gouges");check((poll().pressed&pt::kPadGouge)==0,"holding X does not repeat");input.InjectKey(SDL_SCANCODE_X,false);poll();
    input.InjectMouseButton(SDL_BUTTON_LEFT,true);check((poll().pressed&pt::kPadAction)!=0,"left mouse is the action, which gouges");input.InjectMouseButton(SDL_BUTTON_LEFT,false);poll();
    for(const SDL_Scancode act:{SDL_SCANCODE_E,SDL_SCANCODE_RETURN,SDL_SCANCODE_SPACE}){
        input.InjectKey(act,true);const uint32_t pressed=poll().pressed;check((pressed&pt::kPadAction)!=0&&(pressed&pt::kPadGouge)==0,"an Act key is the action, not the X key");input.InjectKey(act,false);poll();
    }
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
    auto mouse_event=[&](bool down){SDL_Event e{};e.type=down?SDL_EVENT_MOUSE_BUTTON_DOWN:SDL_EVENT_MOUSE_BUTTON_UP;e.button.button=SDL_BUTTON_RIGHT;e.button.down=down;e.button.clicks=1;input.ProcessEvent(e);};
    for(const pt::MouseUse use:{pt::MouseUse::Look,pt::MouseUse::Menu}){
        poll(use);mouse_event(true);mouse_event(false);
        const pt::InputState quick=poll(use);
        check((quick.held&pt::kPadZoom)!=0&&(quick.pressed&pt::kPadZoom)!=0&&(quick.raw_held&pt::kRawR3)!=0,"a right click inside one poll is the zoom held for that poll");
        const pt::InputState after=poll(use);
        check((after.held&pt::kPadZoom)==0,"the quick right click is released at the next poll");
    }
    mouse_event(true);mouse_event(false);
    check((poll(pt::MouseUse::None).held&pt::kPadZoom)==0,"a quick click while the mouse is not in use (a dev window) is dropped");
    check((poll(pt::MouseUse::Menu).held&pt::kPadZoom)==0,"and does not come back once the mouse is in use again");
    input.InjectMouseButton(SDL_BUTTON_RIGHT,true);
    bool held_all=true;for(int i=0;i<120;++i)held_all&=(poll(pt::MouseUse::Menu).held&pt::kPadZoom)!=0;
    check(held_all,"a held right button is the zoom on every poll in the menu");
    input.InjectMouseButton(SDL_BUTTON_RIGHT,false);
    check((poll(pt::MouseUse::Menu).held&pt::kPadZoom)==0,"releasing the right button ends the zoom at the next poll");
    {
        pt::InputState pending;auto tick=[&]{const pt::InputState t=pending;pending.pressed=pending.raw_pressed=0;return t;};
        input.InjectMouseButton(SDL_BUTTON_RIGHT,true);pt::MergeFrameButtons(pending,poll(pt::MouseUse::Menu));
        input.InjectMouseButton(SDL_BUTTON_RIGHT,false);pt::MergeFrameButtons(pending,poll(pt::MouseUse::Menu));
        const pt::InputState first=tick();
        check((first.held&pt::kPadZoom)&&(first.pressed&pt::kPadZoom)&&(first.raw_held&pt::kRawR3),"a one-frame click between two ticks is held at the next tick");
        pt::MergeFrameButtons(pending,poll(pt::MouseUse::Menu));
        check(!(tick().held&pt::kPadZoom),"and released at the tick after");
        input.InjectMouseButton(SDL_BUTTON_RIGHT,true);
        bool held=true;for(int i=0;i<12;++i){pt::MergeFrameButtons(pending,poll(pt::MouseUse::Menu));if(i%4==3)held&=(tick().held&pt::kPadZoom)!=0;}
        check(held,"a hold over many frames is held at every tick");
        input.InjectMouseButton(SDL_BUTTON_RIGHT,false);pt::MergeFrameButtons(pending,poll(pt::MouseUse::Menu));
        check(!(tick().held&pt::kPadZoom),"its release reaches the next tick");
        input.InjectKey(SDL_SCANCODE_X,true);pt::MergeFrameButtons(pending,poll());input.InjectKey(SDL_SCANCODE_X,false);pt::MergeFrameButtons(pending,poll());
        const pt::InputState gouge=tick();
        check((gouge.pressed&pt::kPadGouge)&&!(gouge.held&pt::kPadGouge),"the X key's gouge is a press, never held");
    }
    {
        pt::InputState pending;auto tick=[&]{const pt::InputState t=pending;pt::SpendTickInput(pending);return t;};
        auto frame=[&]{pt::MergeFrameButtons(pending,poll());};
        frame();tick();
        input.InjectMouseButton(SDL_BUTTON_LEFT,true);frame();input.InjectMouseButton(SDL_BUTTON_LEFT,false);frame();
        const pt::InputState click=tick();
        check((click.survival_pressed&pt::kSurvivalFire)&&(click.survival_held&pt::kSurvivalFire),"a fire click is pressed and held at the next tick");
        frame();bool quiet=true;for(int i=0;i<30;++i){frame();const pt::InputState t=tick();quiet&=!(t.survival_pressed&pt::kSurvivalFire)&&!(t.survival_held&pt::kSurvivalFire);}
        check(quiet,"after the click no later tick fires");
        input.InjectMouseButton(SDL_BUTTON_LEFT,true);frame();
        int fire_presses=tick().survival_pressed&pt::kSurvivalFire?1:0;bool held=true;
        for(int i=0;i<20;++i){frame();const pt::InputState t=tick();fire_presses+=t.survival_pressed&pt::kSurvivalFire?1:0;held&=(t.survival_held&pt::kSurvivalFire)!=0;}
        check(fire_presses==1&&held,"a held fire button is one press, then held");
        input.InjectKey(SDL_SCANCODE_R,true);frame();input.InjectKey(SDL_SCANCODE_R,false);frame();
        int reload_presses=0;fire_presses=0;
        for(int i=0;i<20;++i){const pt::InputState t=tick();reload_presses+=t.survival_pressed&pt::kSurvivalReload?1:0;fire_presses+=t.survival_pressed&pt::kSurvivalFire?1:0;frame();}
        check(reload_presses==1&&fire_presses==0,"a reload tapped while fire is held is one press and adds no shot");
        input.InjectMouseButton(SDL_BUTTON_LEFT,false);frame();tick();frame();
        check(!(tick().survival_held&pt::kSurvivalFire),"releasing fire ends it at the next tick");
    }
    pads.DetachAll();input.Shutdown();SDL_Quit();return failed?1:0;
}
