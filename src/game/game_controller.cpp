#include "game/game_controller.h"

#include "engine/core/log.h"
#include "game/game.h"
#include "game/ui/game_ui.h"
#include "game/loop_browser.h"

namespace pt::game {
namespace {

constexpr const char* kPauseFlag = "S_DISABLE_GAME_PAUSE";
constexpr const char* kPauseHolder = "ResetGame";
constexpr float kGameFrame = 1.0f / 30.0f;

bool Continuous(int step) {
    return step == 9 || step == 13 || step == 15 || step == 28 || step == 31 || step == 32 || step == 33;
}

}

void GameController::SetStep(int step) {
    if (requested_ != step) {
        LogInfo("controller: step {} -> {}", requested_, step);
        if (ModLua* mods = game_.ModScripts()) {
            mods->StepChange(requested_, step);
        }
    }
    requested_ = step;
}

void GameController::ChangeGameStep(std::string_view name) {
    LogInfo("controller: ChangeGameStep({})", name);
    if (name == "Init") {
        SetStep(0);
    } else if (name == "EndPreface") {
        SetStep(11);
    } else if (name == "StartGame") {
        SetStep(14);
    } else if (name == "ResetGame") {
        SetStep(17);
    } else if (name == "Endf120") {
        if (game_.LoopReloadPending()) {
            LogInfo("controller: Endf120 ignored during loop browser reset");
            return;
        }
        SetStep(17);
        replay_preface = true;
        game_.EndBrowseSuppression("Endf120 checkpoint");
        game_.Floor().SetSaveFloorName("f120");
        game_.RequestSave();
    } else if (name == "GotoGameOver") {
        SetStep(16);
    } else if (name == "GotoEnding") {
        SetStep(21);
    } else if (name == "FinishEndingRestartGame") {
        if (current_ == 28 && !game_.BrowseSaveSuppressed()) game_.EnableGamePlus();
        if (current_ == 31) game_.EndStreetWalk();
        SetStep(29);
    } else {
        LogWarn("controller: unknown game step {}", name);
    }
}

void GameController::FinishEnding() {
    if (game_.LoopReloadPending()) {
        return;
    }
    if (current_ == 28 && !game_.BrowseSaveSuppressed()) {
        game_.NoteGameFinished();
        game_.EnableGamePlus();
    }
    if (current_ == 28) {
        game_.StartPortCredits();
        SetStep(33);
        return;
    }
    SetStep(29);
}

void GameController::Lock(bool lock) {
    PadLocks& locks = game_.GetPlayer().locks;
    for (char set : {'A', 'B', 'C'}) {
        if (lock) {
            locks.Add(set, "StartLock", 0xFFFFFFFFu);
        } else {
            locks.Remove(set, "StartLock");
        }
    }
}

void GameController::ClearPadEnable() {
    for (char set : {'A', 'B', 'C'}) {
        game_.GetPlayer().locks.Remove(set, "GameController");
    }
    pad_enable_pending_.reset();
}

void GameController::FloorTick(float dt) {
    game_.Floor().ProcessPending();
    game_.Floor().Update(dt);
}

void GameController::Update(float dt) {
    dt_ = dt;
    if (save_pending_) {
        game_.RequestSave();
        save_pending_ = false;
    }
    if (disable_option_pending_) {
        game_.Status().Acquire(kPauseFlag, kPauseHolder);
        disable_option_pending_ = false;
        LogInfo("controller: DisableOption, pause menu blocked (no save)");
    }
    if (current_ >= 0 && requested_ != current_) {
        step_wait_ += dt;
        if (step_wait_ + 0.001f < kGameFrame) {
            if (Continuous(current_)) {
                RunStep(current_);
            }
            return;
        }
    }
    step_wait_ = 0.0f;
    current_ = requested_;
    RunStep(current_);
}

void GameController::RunStep(int step) {
    Game& g = game_;
    switch (step) {
    case 0:
        g.Status().Acquire(kPauseFlag, kPauseHolder);
        if (!init_started_) {
            g.Scripts().CallStateFunction("OnInit");
            g.ResetSaveBlocks();
            g.CaptureBootEffects();
            init_started_ = true;
            return;
        }
        init_started_ = false;
        g.RequestLoad();
        Lock(true);
        SetStep(1);
        return;
    case 1:
        if (g.SaveBusy()) {
            return;
        }
        SetStep(2);
        return;
    case 2:
        g.Scripts().CallStateFunction("OnLoadStart");
        Lock(true);
        SetStep(3);
        return;
    case 3:
        if (!g.Stages().IsActive("current")) {
            return;
        }
        SetStep(4);
        return;
    case 4:
        Lock(true);
        g.Scripts().CallStateFunction("OnLoadHallway");
        SetStep(5);
        return;
    case 5:
        if (!g.Stages().IsLoadedInactive("next")) {
            return;
        }
        SetStep(6);
        return;
    case 6:
        g.Scripts().CallStateFunction("OnStartOption");
        g.Floor().RelocateGimmicks();
        if ((g.TakeOptionsAfterRestart() || first_boot) && g.ShowOptionMenuOnFirstBoot()) {
            g.Status().Release(kPauseFlag, kPauseHolder);
            option_menu_open_ = g.OptionsUiAvailable();
            first_boot = false;
            replay_preface = true;
            Lock(false);
            SetStep(7);
            return;
        }
        first_boot = false;
        SetStep(8);
        return;
    case 7:
        g.Status().Acquire(kPauseFlag, kPauseHolder);
        if (option_menu_open_) {
            return;
        }
        if (close_wait_ > 0.0f) {
            close_wait_ -= dt_;
            if (close_wait_ > 1e-4f) {
                return;
            }
        }
        Lock(true);
        SetStep(8);
        return;
    case 8:
        g.Scripts().CallStateFunction("OnStartLogo");
        Lock(true);
        g.Floor().RelocateGimmicks();
        g.Effects().subtitles_enabled = true;
        if (replay_preface) {
            g.Demos().Play("gc_p02_500");
            SetStep(9);
        } else {
            SetStep(11);
        }
        replay_preface = false;
        return;
    case 9:
        Lock(true);
        FloorTick(dt_);
        return;
    case 10:
        SetStep(11);
        return;
    case 11:
        Lock(true);
        g.Effects().subtitles_enabled = false;
        g.Scripts().CallStateFunction("OnPreOpeningDemo");
        g.SendControllerMessage("SetupOpeningOcho");
        g.SendControllerMessage("SetupStartRoomDoor");
        SetStep(12);
        return;
    case 12:
        Lock(true);
        if (g.BrowseEntryPending()) {
            g.Messages().PostDemoMessage("gc_p00_020", "hideOcho");
            LogInfo("controller: loop browser skips the opening demo");
            SetStep(14);
            return;
        }
        if (g.Config().theater && !g.Theater().opening) {
            g.Messages().PostDemoMessage("gc_p00_020", "hideOcho");
            SetStep(14);
            return;
        }
        g.SendControllerMessage(g.Floor().IsCurrentFloorName("f000") ? "PlayFirstOpeningDemo" : "PlayOpeningDemo");
        SetStep(13);
        return;
    case 13:
        FloorTick(dt_);
        return;
    case 14:
        if (g.Floor().IsCurrentFloorName("f000")) g.OnFloorReached("f000", 1);
        g.Scripts().CallStateFunction("OnPreGame");
        g.Floor().OnStartGame();
        g.SpeedrunStartGame();
        g.Status().Release(kPauseFlag, kPauseHolder);
        Lock(false);
        SetStep(15);
        return;
    case 15:
        if (requested_ == 15 && g.BrowseEntryPending()) g.BrowseEnterHallway();
        if (pad_enable_pending_) {
            for (char set : {'A', 'B', 'C'}) {
                if (*pad_enable_pending_) {
                    g.GetPlayer().locks.Add(set, "GameController", 0xFFFFFFFFu);
                } else {
                    g.GetPlayer().locks.Remove(set, "GameController");
                }
            }
            pad_enable_pending_.reset();
        }
        FloorTick(dt_);
        g.Nazo().Update(dt_);
        return;
    case 16:
        g.Scripts().CallStateFunction("OnGameOver");
        SetStep(17);
        return;
    case 17:
        g.Status().Acquire(kPauseFlag, kPauseHolder);
        g.Scripts().CallStateFunction("OnResetGameStopGame");
        SetStep(18);
        return;
    case 18:
        SetStep(19);
        return;
    case 19:
        g.Scripts().CallStateFunction("OnResetGameUnloadStage");
        SetStep(20);
        return;
    case 20:
        if (!g.Stages().IsAllUnloaded()) {
            return;
        }
        if (g.Audio() && g.Audio()->IsEventPlaying("Set_state_game_over")) {
            return;
        }
        if (restart_objects_pending_) {
            g.Demos().Gimmicks().ResetSession();
            g.Objects().Reset();
            if (new_session_pending_) {
                g.Objects().Ocho().Setup();
                g.ResetSessionState();
            } else {
                g.Objects().Ocho().ResetState();
            }
            LogInfo("controller: {} gimmicks reset after stage unload", new_session_pending_ ? "new session" : "new game");
            restart_objects_pending_ = new_session_pending_ = false;
        }
        if (g.ApplyBrowseReload()) return;
        SetStep(2);
        return;
    case 21:
        g.SpeedrunEnding();
        g.Scripts().CallStateFunction("OnPreEndingStopGame");
        if (GameUi* ui = GameUi::Active(); ui && ui->MenuOpen() && !g.Config().theater) {
            ui->CloseMenu();
            LogInfo("controller: GotoEnding closes the pause menu");
        }
        option_menu_open_ = false;
        g.Status().Acquire(kPauseFlag, kPauseHolder);
        SetStep(22);
        return;
    case 22:
        SetStep(23);
        return;
    case 23:
        g.Scripts().CallStateFunction("OnPreEndingUnLoadStage");
        SetStep(24);
        return;
    case 24:
        if (!g.Stages().IsAllUnloaded()) {
            return;
        }
        if (g.Audio() && g.Audio()->IsEventPlaying("Set_state_game_over")) {
            return;
        }
        SetStep(25);
        return;
    case 25:
        g.Scripts().CallStateFunction("OnLoadEnding");
        SetStep(26);
        return;
    case 26:
        if (!g.Stages().IsActive("current")) {
            return;
        }
        SetStep(27);
        return;
    case 27:
        g.Effects().subtitles_enabled = true;
        g.Scripts().CallStateFunction("OnEnding");
        if (g.Floor().IsCurrentFloorName("ending") && !g.BrowseSaveSuppressed()) {
            g.NoteBrowseReached(kBrowseEnding);
        }
        if (g.StreetPending() && g.StartStreetWalk()) {
            return;
        }
        SetStep(28);
        return;
    case 28:
        FloorTick(dt_);
        return;
    case 29:
        g.Effects().subtitles_enabled = false;
        replay_preface = true;
        SetStep(30);
        return;
    case 30:
        restart_objects_pending_ = true;
        g.Scripts().CallStateFunction("OnRestartGame");
        g.Floor().SetFloorLevel("f000");
        g.Floor().ResetLoopCount();
        g.Nazo().ResetAllStates();
        g.RequestSave();
        SetStep(17);
        return;
    case 31:
        g.Status().Release(kPauseFlag, kPauseHolder);
        Lock(false);
        g.UpdateStreetWalk(dt_);
        return;
    case 32:
        g.UpdateStreetOffer();
        return;
    case 33:
        if (requested_ == 33 && g.UpdatePortCredits(dt_)) {
            SetStep(g.OfferStreetWalk() ? 32 : 29);
        }
        return;
    default:
        LogError("controller: bad step {}", step);
        SetStep(0);
        return;
    }
}

}
