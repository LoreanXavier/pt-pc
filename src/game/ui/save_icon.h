#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "engine/ui/uia.h"
#include "game/ui/uia_player.h"
#include "game/ui/uif_view.h"

namespace pt::game {

class UiAssets;

// The save and load icon (UI_sys_loading.uilb: UI_sys_loadicon.uif, three circles low on the right), the component 0x1284B50 the
// UI common data keeps at +0x80. The save job posts SaveUiDisp or LoadUiDisp when it starts writing or reading (Start, 0x1284F40):
// setin and the loop play; once the job reports the write or read done and the icon has shown for 2 s, setout plays (0x1285030), and
// the icon hides when it ends. Its layout is drawn at STRONG_PAUSE_ICON, priority 209.
class SaveIcon {
public:
    static constexpr int kPriority = 209;

    bool Init(UiAssets& assets);
    void Start(bool loading);
    // one 30 Hz game frame of the icon; the port's saves and loads are done when they start
    void Update(float dt);
    void Draw(ui::UiBatch& batch, const UiCanvas& canvas);
    bool Visible() const { return state_ != State::Hidden; }

private:
    enum class State { Hidden, Saving, Loading, Leaving };
    struct Animation {
        const ui::UiaAnimation* main = nullptr;
        const ui::UiaAnimation* shader = nullptr;
        float speed = 1.0f;
    };

    void Play(const Animation* animation, bool loop);
    bool Playing(const Animation* animation) const;

    const ui::UifModel* model_ = nullptr;
    UifView view_;
    UiaPlayers players_;
    std::vector<std::unique_ptr<ui::UiaAnimation>> storage_;
    std::unordered_map<uint64_t, Animation> animations_;
    const Animation* setin_ = nullptr;
    const Animation* loop_ = nullptr;
    const Animation* setout_ = nullptr;
    glm::vec2 root_offset_{0.0f};
    State state_ = State::Hidden;
    float time_ = 0.0f;
    float clock_ = 0.0f;
};

}
