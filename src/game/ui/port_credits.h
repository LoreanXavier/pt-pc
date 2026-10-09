#pragma once

#include <span>
#include <string_view>
#include <vector>

// The port's own credits page (docs/gameplay.md, port credits): shown when the ending's credits end (GameController::FinishEnding,
// controller step 33), before the street question or the ending's restart. Its cards play as the original's staff pages do:
// sys_ending's UI_sys_end_crdt_setin_short.uia (239 frames: alpha 0 to 1 by frame 29, held to 149, 0 at 239) and
// UI_sys_end_crdt_setin.uia (329 frames: 0 to 1 by 59, held to 209, 0 at 329), at 60 frames per second, with the scale growing
// from 1 to 1.2 over the card. The texts are dark on the ending's off-white fade (SetFadeColor 0.93, 0.94, 0.93).
namespace pt::game::port_credits {

struct Card {
    // a line that starts with "pc_" is a PC text key (PcText); the others (names, product names, the licences' notices) are shown
    // as they are
    std::vector<std::string_view> lines;
    bool short_setin = false;
    float size = 1.0f;
};

struct View {
    int card = -1;
    float alpha = 0.0f;
    float scale = 1.0f;
};

std::span<const Card> Cards();
// the whole page in seconds
float Duration();
View At(float seconds);

}
