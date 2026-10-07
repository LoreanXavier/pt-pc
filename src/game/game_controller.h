#pragma once

#include <optional>
#include <string_view>

namespace pt::game {

class Game;

class GameController {
public:
    explicit GameController(Game& game) : game_(game) {}

    void Update(float dt);
    void ChangeGameStep(std::string_view name);
    void FinishEnding();
    void SetStep(int step);
    int Step() const { return current_; }
    int RequestedStep() const { return requested_; }
    bool LoadStep() const { return (current_ >= 0 && current_ <= 5) || (current_ >= 16 && current_ <= 20); }
    bool InGame() const { return current_ == 15; }
    void SaveGame() { save_pending_ = true; }
    void DisableOption() { disable_option_pending_ = true; }
    void SetPadEnablePending(bool enable) { pad_enable_pending_ = enable; }
    void RequestObjectsReset() { restart_objects_pending_ = true; }
    void RequestNewSession() { restart_objects_pending_ = new_session_pending_ = true; }
    void ClearPadEnable();
    void CloseOptionMenu() {
        option_menu_open_ = false;
        close_wait_ = 1.0f / 30.0f;
    }
    bool OptionMenuOpen() const { return option_menu_open_; }

    bool replay_preface = false;
    bool first_boot = true;

private:
    void RunStep(int step);
    void Lock(bool lock);
    void FloorTick(float dt);

    Game& game_;
    int requested_ = 0;
    int current_ = -1;
    bool init_started_ = false;
    bool restart_objects_pending_ = false;
    bool new_session_pending_ = false;
    bool save_pending_ = false;
    bool disable_option_pending_ = false;
    bool option_menu_open_ = false;
    std::optional<bool> pad_enable_pending_;
    float dt_ = 0.0f;
    float step_wait_ = 0.0f;
    float close_wait_ = 0.0f;
};

}
