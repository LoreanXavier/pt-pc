#pragma once

#include <span>
#include <string_view>
#include <vector>

namespace pt::game::port_credits {

struct Card {
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
float Duration();
View At(float seconds);

}
