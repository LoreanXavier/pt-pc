#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace pt {
class LiveSplitClient;
}

namespace pt::game {

// One segment of a run: the floor and pass played (the loop the original counts, FloorLevel's loop count), its times and the run's
// times when it ended
struct SpeedrunSplit {
    std::string floor;
    int pass = 1;
    double real = 0.0;
    double game = 0.0;
    double real_total = 0.0;
    double game_total = 0.0;
    std::string Key() const;
};

// The speedrun timer (PC extra, Extras > Speedrun timer, pt.ini [extras] speedrun; docs/gameplay.md, speedrun mode). Off by
// default; when off nothing here runs and nothing is written.
// - A run starts when the player gets control (controller step 14, StartGame) and no run is going, ends when the ending starts
//   (step 21, GotoEnding), and splits at every NextFloor (each loop the original counts, a repeated pass included).
// - Two clocks are kept: real time (wall clock from the start, everything included: the pause menu, loads, demos) and game time
//   (the game's 60 Hz ticks of unpaused play outside the controller's load steps 0 to 5 and 16 to 20, so the pause menu, a
//   paused unfocused window and the stage unload and load after a game over do not count). The overlay shows the one picked.
// - A run is "full" when it starts on f000 with no puzzle solved; only full runs set records. A loop browser pick, a progress
//   reset or turning the timer off abandons a run.
class SpeedrunTimer {
public:
    enum class State { Idle, Running, Finished };

    // 0 off, 1 the overlay and records show real time, 2 game time
    void SetMode(int mode);
    int Mode() const { return mode_; }
    bool Enabled() const { return mode_ != 0; }
    bool ShowsGameTime() const { return mode_ == 2; }
    // where the records go (the user data folder): speedrun.ini, speedrun_history.txt, speedrun_pb.lss; empty writes nothing
    void SetRecordDirectory(const std::filesystem::path& directory);
    void SetLiveSplit(LiveSplitClient* client) { livesplit_ = client; }

    // a game tick of unpaused play (Game::Update); `loading` excludes it from game time
    void Tick(float dt, bool loading);
    void Start(std::string_view floor, bool full);
    void Split(std::string_view floor, int pass);
    void Finish(std::string_view floor, int pass);
    void Abandon(std::string_view why);
    // once per frame: LiveSplit's game time
    void Poll();

    State GetState() const { return state_; }
    bool FullRun() const { return full_; }
    double Real() const;
    double Game() const { return game_; }
    double SegmentReal() const { return Real() - segment_real_start_; }
    double SegmentGame() const { return game_ - segment_game_start_; }
    double Shown() const { return ShowsGameTime() ? Game() : Real(); }
    double ShownSegment() const { return ShowsGameTime() ? SegmentGame() : SegmentReal(); }
    const std::string& Floor() const { return floor_; }
    int Pass() const { return pass_; }
    const std::vector<SpeedrunSplit>& Splits() const { return splits_; }
    // the record of the shown clock before this run finished (0: none), and whether the finished run beat it
    double PreviousBest() const { return previous_best_; }
    bool NewBest() const { return new_best_; }
    double BestTotal() const;
    // the last split against the record run's time at the same split (shown clock); false when there is nothing to compare
    bool LastSplitDelta(double& delta) const;
    // real seconds since the last split or the finish
    double SinceSplit() const;
    // the run as text for the log and the history file
    std::string Describe() const;

    // m:ss.cc, or h:mm:ss.cc from an hour
    static std::string Format(double seconds);
    // the loop browser's numbering of a floor and pass: 0 the start room, 1 to 15 the loops (f050's passes 1 and 2 are 7 and 8);
    // `repeat` is the pass when the floor was played again past that (f160's second pass: 15, 2), else 0; -1 for an unknown floor
    static int LoopNumber(std::string_view floor, int pass, int& repeat);
    // "Start room", "Loop 7", "Loop 15 (2)" in a menu language (pc_speedrun_start_room, pc_speedrun_loop)
    static std::string SplitName(const SpeedrunSplit& split, int language);
    static std::string FloorName(std::string_view floor, int pass, int language);

private:
    struct Record {
        double total = 0.0;
        std::vector<SpeedrunSplit> splits;
    };
    void LoadRecords();
    void SaveRecords();
    void AppendHistory();
    void WriteLss() const;
    void SendGameTime();
    const Record& Best(bool game) const { return game ? best_game_ : best_real_; }

    int mode_ = 0;
    State state_ = State::Idle;
    bool full_ = false;
    std::chrono::steady_clock::time_point start_{};
    std::chrono::steady_clock::time_point last_split_at_{};
    double real_final_ = 0.0;
    double game_ = 0.0;
    double segment_real_start_ = 0.0;
    double segment_game_start_ = 0.0;
    std::string floor_;
    int pass_ = 1;
    std::vector<SpeedrunSplit> splits_;
    std::filesystem::path directory_;
    bool records_loaded_ = false;
    Record best_real_;
    Record best_game_;
    // best segment time per split key, real and game
    std::vector<std::pair<std::string, double>> gold_real_;
    std::vector<std::pair<std::string, double>> gold_game_;
    int attempts_ = 0;
    int finished_runs_ = 0;
    double previous_best_ = 0.0;
    bool new_best_ = false;
    LiveSplitClient* livesplit_ = nullptr;
    std::chrono::steady_clock::time_point livesplit_sent_{};
};

}
