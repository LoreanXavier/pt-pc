#pragma once
#include <array>
#include <string_view>
namespace pt::game {
// One entry of the loop browser (gameplay.md, loop browser). `floor` and `pass` are what the entry starts: a hallway loop is entered
// from the start room on `previous` (the floor a save made at the loop's start holds), "ending" plays the ending demo and its credits,
// "street" starts the street walk after the ending. `label` is the menu text key (pc_settings.cpp and the language tables).
struct BrowseLoop {
    std::string_view floor, previous, label;
    int pass = 1;
};
inline constexpr std::array<BrowseLoop, 18> kBrowseLoops{{
    {"f000", "f000", "pc_loop_f000"},
    {"f010", "f000", "pc_loop_f010"},
    {"f005", "f010", "pc_loop_f005"},
    {"f020", "f005", "pc_loop_f020"},
    {"f030", "f020", "pc_loop_f030"},
    {"f040", "f030", "pc_loop_f040"},
    {"f060", "f040", "pc_loop_f060"},
    {"f050", "f060", "pc_loop_f050a"},
    {"f050", "f060", "pc_loop_f050b", 2},
    {"f070", "f050", "pc_loop_f070"},
    {"f080", "f070", "pc_loop_f080"},
    {"f090", "f080", "pc_loop_f090"},
    {"f100", "f090", "pc_loop_f100"},
    {"f110", "f100", "pc_loop_f110"},
    {"f120", "f110", "pc_loop_f120"},
    {"f160", "f120", "pc_loop_f160"},
    {"ending", "f160", "pc_loop_ending"},
    {"street", "f160", "pc_loop_street"},
}};
inline constexpr int kBrowseEnding = 16;
inline constexpr int kBrowseStreet = 17;
// the entry of a floor and pass reached in play (-1: none)
inline constexpr int BrowseIndexOf(std::string_view floor, int pass) {
    if (floor == "f050") return pass >= 2 ? 8 : 7;
    for (int i = 0; i < static_cast<int>(kBrowseLoops.size()); ++i) {
        if (kBrowseLoops[i].floor == floor) return i;
    }
    return -1;
}
}
