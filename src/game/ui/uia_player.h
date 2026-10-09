#pragma once

#include <vector>

#include "engine/ui/uia.h"
#include "game/ui/uif_view.h"

namespace pt::game {

class UiaPlayers {
public:
    void Play(const ui::UiaAnimation* animation, float speed = 1.0f, bool loop = false);
    void Hold(const ui::UiaAnimation* animation, float frame);
    void Stop(const ui::UiaAnimation* animation);
    void Update(float dt);
    void Clear() { players_.clear(); }
    bool Playing(const ui::UiaAnimation* animation) const;
    bool AnyPlaying() const;
    void Apply(UifView& view) const;

private:
    struct Player {
        const ui::UiaAnimation* animation = nullptr;
        float time = 0.0f;
        float speed = 1.0f;
        bool loop = false;
        bool held = false;
        float frame = 0.0f;
    };

    float Frame(const Player& player) const;

    std::vector<Player> players_;
};

}
