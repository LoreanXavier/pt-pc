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
    // the end of the ending's credits (GameController.FinishEndingRestartGame)
    void FinishEnding();
    void SetStep(int step);
    int Step() const { return current_; }
    int RequestedStep() const { return requested_; }
    // the steps that wait for the stages to load or unload (boot and the reset after a game over): the speedrun's game time
    // leaves them out
    bool LoadStep() const { return (current_ >= 0 && current_ <= 5) || (current_ >= 16 && current_ <= 20); }
    bool InGame() const { return current_ == 15; }
    void SaveGame() { save_pending_ = true; }
    void DisableOption() { disable_option_pending_ = true; }
    void SetPadEnablePending(bool enable) { pad_enable_pending_ = enable; }
    // the ending's restart: the gimmicks start over after the stage unload (step 20)
    void RequestObjectsReset() { restart_objects_pending_ = true; }
    // a loop browser pick or a progress reset: besides the gimmicks, everything a session keeps (Lisa's killed flag, the
    // player's body groups, the screen effects) starts over after the stage unload, as at boot (Game::ResetSessionState)
    void RequestNewSession() { restart_objects_pending_ = new_session_pending_ = true; }
    // drops the pad lock of GameController.SetPadEnable and any request of it, which a reset left in place would carry over
    void ClearPadEnable();
    // the close press clears the pause byte in the pause menu's update (0x9202E0, User group), which runs after the controller's
    // (0x90F200) in a frame, so step 7 (0x921D00) sees it in the next game frame
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
