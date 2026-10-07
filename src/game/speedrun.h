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

struct SpeedrunSplit {
    std::string floor;
    int pass = 1;
    double real = 0.0;
    double game = 0.0;
    double real_total = 0.0;
    double game_total = 0.0;
    std::string Key() const;
};

class SpeedrunTimer {
public:
    enum class State { Idle, Running, Finished };

    void SetMode(int mode);
    int Mode() const { return mode_; }
    bool Enabled() const { return mode_ != 0; }
    bool ShowsGameTime() const { return mode_ == 2; }
    void SetRecordDirectory(const std::filesystem::path& directory);
    void SetLiveSplit(LiveSplitClient* client) { livesplit_ = client; }

    void Tick(float dt, bool loading);
    void Start(std::string_view floor, bool full);
    void Split(std::string_view floor, int pass);
    void Finish(std::string_view floor, int pass);
    void Abandon(std::string_view why);
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
    double PreviousBest() const { return previous_best_; }
    bool NewBest() const { return new_best_; }
    double BestTotal() const;
    bool LastSplitDelta(double& delta) const;
    double SinceSplit() const;
    std::string Describe() const;

    static std::string Format(double seconds);
    static int LoopNumber(std::string_view floor, int pass, int& repeat);
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
