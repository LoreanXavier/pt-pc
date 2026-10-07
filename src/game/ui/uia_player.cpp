#include "game/ui/uia_player.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

#include "engine/ui/uif.h"

namespace pt::game {
namespace {

bool Covered(const ui::UiaAnimation& older, const ui::UiaAnimation& newer) {
    std::set<std::pair<uint32_t, uint32_t>> channels;
    for (const ui::UiaNode& n : newer.Nodes()) {
        for (const ui::UiaTrack& t : n.tracks) {
            channels.insert({n.node, t.unit});
        }
    }
    for (const ui::UiaNode& n : older.Nodes()) {
        for (const ui::UiaTrack& t : n.tracks) {
            if (!channels.contains({n.node, t.unit})) {
                return false;
            }
        }
    }
    return true;
}

}

void UiaPlayers::Play(const ui::UiaAnimation* animation, float speed, bool loop) {
    if (!animation) {
        return;
    }
    std::erase_if(players_, [&](const Player& p) { return p.animation == animation || Covered(*p.animation, *animation); });
    Player player;
    player.animation = animation;
    player.speed = speed;
    player.loop = loop;
    players_.push_back(player);
}

void UiaPlayers::Hold(const ui::UiaAnimation* animation, float frame) {
    if (!animation) {
        return;
    }
    for (Player& p : players_) {
        if (p.animation == animation) {
            p.held = true;
            p.frame = frame;
            return;
        }
    }
    std::erase_if(players_, [&](const Player& p) { return Covered(*p.animation, *animation); });
    Player player;
    player.animation = animation;
    player.held = true;
    player.frame = frame;
    players_.push_back(player);
}

void UiaPlayers::Stop(const ui::UiaAnimation* animation) {
    std::erase_if(players_, [&](const Player& player) { return player.animation == animation; });
}

void UiaPlayers::Update(float dt) {
    for (Player& p : players_) {
        if (!p.held) {
            p.time += dt;
        }
    }
}

float UiaPlayers::Frame(const Player& p) const {
    if (p.held) {
        return p.frame;
    }
    const float frames = static_cast<float>(p.animation->Frames());
    const float frame = p.time * ui::UiaAnimation::kFramesPerSecond * p.speed;
    return p.loop && frames > 0.0f ? std::fmod(frame, frames) : std::min(frame, frames);
}

bool UiaPlayers::Playing(const ui::UiaAnimation* animation) const {
    for (const Player& p : players_) {
        if (p.animation == animation) {
            return !p.held && (p.loop || p.time * ui::UiaAnimation::kFramesPerSecond * p.speed < static_cast<float>(p.animation->Frames()));
        }
    }
    return false;
}

void UiaPlayers::Apply(UifView& view) const {
    const ui::UifModel* model = view.Model();
    if (!model) {
        return;
    }
    for (const Player& p : players_) {
        const float frame = Frame(p);
        for (const ui::UiaNode& node : p.animation->Nodes()) {
            const auto range = view.HashIndex().equal_range(node.node);
            for (auto it = range.first; it != range.second; ++it) {
                UifNodeState& state = view.StateAt(it->second);
                for (const ui::UiaTrack& track : node.tracks) {
                    const glm::vec4 v = ui::UiaAnimation::Sample(track, frame);
                    if (track.unit == ui::UiaAnimation::kTranslate) {
                        state.anim_translate = glm::vec3(v);
                    } else if (track.unit == ui::UiaAnimation::kScale) {
                        state.anim_scale = glm::vec3(v);
                    } else if (track.unit == ui::UiaAnimation::kColor) {
                        state.anim_color = v;
                    } else if (const int slot = ui::UifParamSlot(track.unit); slot >= 0) {
                        if (!state.params) {
                            state.params = model->Nodes()[it->second].material.params;
                        }
                        (*state.params)[slot] = v.x;
                    }
                }
            }
            const auto points = view.PointIndex().equal_range(node.node);
            for (auto it = points.first; it != points.second; ++it) {
                for (const ui::UiaTrack& track : node.tracks) {
                    if (track.unit == ui::UiaAnimation::kTranslate) {
                        const glm::vec4 v = ui::UiaAnimation::Sample(track, frame);
                        view.StateAt(it->second.first).anim_points.emplace_back(it->second.second, glm::vec2(v));
                    }
                }
            }
        }
    }
}

}
