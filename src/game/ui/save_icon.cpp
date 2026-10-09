#include "game/ui/save_icon.h"

#include <string>

#include "engine/core/log.h"
#include "engine/core/strcode.h"
#include "engine/fs/vfs.h"
#include "engine/ui/uilb.h"
#include "game/ui/ui_assets.h"

namespace pt::game {
namespace {

constexpr const char* kModelPath = "/Assets/sh/ui/ModelAsset/sys_loadicon/Scenes/UI_sys_loadicon.uif";
constexpr const char* kLayoutPath = "/Assets/sh/ui/LayoutAsset/sys_loading/UI_sys_loading.uilb";
// 0x1284E00 looks these up by name in the layout, and the text nodes 0x575A0BDC8C71 and 0x2DB5A5C54ED4 of the model
constexpr const char* kSetin = "UI_sys_loadicon_setin";
constexpr const char* kLoop = "UI_sys_loadicon_lp";
constexpr const char* kSetout = "UI_sys_loadicon_setout";
// 0x1285030: setout waits until the job is done and the icon has shown this long
constexpr float kMinimumShown = 2.0f;
// the UI animations step once per 30 Hz game frame, as the option menu's
constexpr float kGameFrame = 1.0f / 30.0f;

}

bool SaveIcon::Init(UiAssets& assets) {
    model_ = assets.Model(kModelPath);
    if (!model_) {
        LogWarn("ui: save icon model {} not found", kModelPath);
        return false;
    }
    ui::UilbLayout layout;
    std::string error;
    if (auto data = assets.Files().ReadFile(kLayoutPath); !data || !layout.Parse(*data, &error)) {
        LogWarn("ui: save icon layout {}: {}", kLayoutPath, data ? error : std::string("not found"));
    }
    auto load = [&](const std::string& path) -> const ui::UiaAnimation* {
        if (path.empty()) {
            return nullptr;
        }
        auto data = assets.Files().ReadFile(path);
        auto animation = std::make_unique<ui::UiaAnimation>();
        std::string reason;
        if (!data || !animation->Parse(*data, &reason)) {
            LogWarn("ui: save icon animation {}: {}", path, data ? reason : std::string("not found"));
            return nullptr;
        }
        storage_.push_back(std::move(animation));
        return storage_.back().get();
    };
    for (const ui::UilbAnimation& a : layout.Animations()) {
        animations_[a.name] = {load(a.main), load(a.shader), a.speed};
    }
    // the layout places the model: its nodes all sit at the origin
    if (!layout.Models().empty()) {
        root_offset_ = glm::vec2(layout.Models().front().translate);
    }
    auto find = [&](const char* name) -> const Animation* {
        auto it = animations_.find(StrCode64(name));
        return it != animations_.end() ? &it->second : nullptr;
    };
    setin_ = find(kSetin);
    loop_ = find(kLoop);
    setout_ = find(kSetout);
    if (!setin_ || !loop_ || !setout_) {
        LogWarn("ui: save icon layout lacks {}", !setin_ ? kSetin : !loop_ ? kLoop : kSetout);
    }
    view_.Bind(model_, &assets);
    for (const std::string& texture : model_->Textures()) {
        assets.Texture(texture);
    }
    return true;
}

void SaveIcon::Play(const Animation* animation, bool loop) {
    if (!animation) {
        return;
    }
    players_.Play(animation->main, animation->speed, loop);
    players_.Play(animation->shader, animation->speed, loop);
}

bool SaveIcon::Playing(const Animation* animation) const {
    return animation && (players_.Playing(animation->main) || players_.Playing(animation->shader));
}

void SaveIcon::Start(bool loading) {
    if (!model_) {
        return;
    }
    // 0x1284F40: the text nodes get the text of "Saving" or "Loading", keys the language files do not have (theirs are sys_saving and
    // sys_loading), so the captures show the circles alone
    for (size_t i = 0; i < model_->Nodes().size(); ++i) {
        if (model_->Nodes()[i].type == ui::UifNodeType::Text) {
            view_.StateAt(i).text = std::string();
        }
    }
    players_.Clear();
    view_.ClearAnimation();
    Play(setin_, false);
    Play(loop_, true);
    state_ = loading ? State::Loading : State::Saving;
    time_ = 0.0f;
    clock_ = 0.0f;
    LogInfo("ui: save icon shows ({})", loading ? "loading" : "saving");
}

void SaveIcon::Update(float dt) {
    if (state_ == State::Hidden) {
        return;
    }
    clock_ += dt;
    while (clock_ + 1e-4f >= kGameFrame) {
        clock_ -= kGameFrame;
        players_.Update(kGameFrame);
        if (state_ == State::Saving || state_ == State::Loading) {
            time_ += kGameFrame;
            if (time_ > kMinimumShown) {
                Play(setout_, false);
                state_ = State::Leaving;
            }
        } else if (state_ == State::Leaving && !Playing(setout_)) {
            state_ = State::Hidden;
            players_.Clear();
            LogInfo("ui: save icon hidden");
            break;
        }
    }
    players_.Apply(view_);
}

void SaveIcon::Draw(ui::UiBatch& batch, const UiCanvas& canvas) {
    if (state_ == State::Hidden || !model_) {
        return;
    }
    view_.Draw(batch, canvas, 0, 1.0f, root_offset_);
}

}
