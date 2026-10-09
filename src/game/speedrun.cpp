#include "game/speedrun.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <format>
#include <fstream>
#include <sstream>

#include "engine/core/log.h"
#include "engine/platform/livesplit.h"
#include "game/ui/pc_settings.h"

namespace pt::game {
namespace {

constexpr const char* kRecordsFile = "speedrun.ini";
constexpr const char* kHistoryFile = "speedrun_history.txt";
constexpr const char* kLssFile = "speedrun_pb.lss";

double Seconds(std::chrono::steady_clock::duration d) {
    return std::chrono::duration<double>(d).count();
}

// LiveSplit's time spans: hh:mm:ss.fffffff
std::string LssTime(double seconds) {
    seconds = std::max(0.0, seconds);
    const auto ticks = static_cast<long long>(std::llround(seconds * 1e7));
    const long long h = ticks / 36000000000LL;
    const long long m = ticks / 600000000LL % 60;
    const long long s = ticks / 10000000LL % 60;
    const long long f = ticks % 10000000LL;
    return std::format("{:02}:{:02}:{:02}.{:07}", h, m, s, f);
}

std::string Escape(std::string_view text) {
    std::string out;
    for (char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

// the segment's name in the LiveSplit file: English, as the overlay names it in English
std::string SegmentName(const SpeedrunSplit& s) {
    return SpeedrunTimer::SplitName(s, 0);
}

std::string LiveSplitTime(double seconds) {
    seconds = std::max(0.0, seconds);
    const int h = static_cast<int>(seconds / 3600.0);
    const int m = static_cast<int>(seconds / 60.0) % 60;
    const double s = seconds - h * 3600.0 - m * 60.0;
    return std::format("{}:{:02}:{:06.3f}", h, m, s);
}

}

int SpeedrunTimer::LoopNumber(std::string_view floor, int pass, int& repeat) {
    static constexpr std::pair<std::string_view, int> kNumbers[] = {
        {"f000", 0}, {"f010", 1}, {"f005", 2}, {"f020", 3}, {"f030", 4}, {"f040", 5}, {"f060", 6}, {"f050", 7},
        {"f070", 9}, {"f080", 10}, {"f090", 11}, {"f100", 12}, {"f110", 13}, {"f120", 14}, {"f160", 15}};
    repeat = 0;
    for (const auto& [name, number] : kNumbers) {
        if (floor != name) continue;
        if (floor == "f050") {
            if (pass > 2) repeat = pass;
            return pass >= 2 ? 8 : 7;
        }
        if (pass > 1) repeat = pass;
        return number;
    }
    return -1;
}

std::string SpeedrunTimer::FloorName(std::string_view floor, int pass, int language) {
    int repeat = 0;
    const int number = LoopNumber(floor, pass, repeat);
    std::string name;
    if (number < 0) {
        name = std::string(floor);
    } else if (number == 0) {
        name = std::string(PcText("pc_speedrun_start_room", language));
    } else {
        name = std::string(PcText("pc_speedrun_loop", language));
        if (const size_t at = name.find("{1}"); at != std::string::npos) name.replace(at, 3, std::to_string(number));
    }
    return repeat > 0 ? std::format("{} ({})", name, repeat) : name;
}

std::string SpeedrunTimer::SplitName(const SpeedrunSplit& split, int language) {
    return FloorName(split.floor, split.pass, language);
}

std::string SpeedrunSplit::Key() const {
    return pass > 1 ? std::format("{}#{}", floor, pass) : floor;
}

std::string SpeedrunTimer::Format(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) seconds = 0.0;
    const auto centi = static_cast<long long>(seconds * 100.0);
    const long long h = centi / 360000;
    const long long m = centi / 6000 % 60;
    const long long s = centi / 100 % 60;
    const long long c = centi % 100;
    return h > 0 ? std::format("{}:{:02}:{:02}.{:02}", h, m, s, c) : std::format("{}:{:02}.{:02}", m, s, c);
}

void SpeedrunTimer::SetMode(int mode) {
    mode = std::clamp(mode, 0, 2);
    if (mode == mode_) return;
    if (mode == 0 && state_ == State::Running) Abandon("timer turned off");
    mode_ = mode;
    if (mode_ == 0) state_ = State::Idle;
    LogInfo("speedrun: timer {}", mode_ == 0 ? "off" : mode_ == 1 ? "on (real time)" : "on (game time)");
}

void SpeedrunTimer::SetRecordDirectory(const std::filesystem::path& directory) {
    directory_ = directory;
    records_loaded_ = false;
}

double SpeedrunTimer::Real() const {
    if (state_ == State::Running) return Seconds(std::chrono::steady_clock::now() - start_);
    return real_final_;
}

double SpeedrunTimer::SinceSplit() const {
    return Seconds(std::chrono::steady_clock::now() - last_split_at_);
}

void SpeedrunTimer::Tick(float dt, bool loading) {
    if (state_ != State::Running || loading) return;
    game_ += dt;
}

void SpeedrunTimer::Start(std::string_view floor, bool full) {
    if (!Enabled() || state_ == State::Running) return;
    if (!records_loaded_) LoadRecords();
    state_ = State::Running;
    full_ = full;
    start_ = last_split_at_ = std::chrono::steady_clock::now();
    real_final_ = 0.0;
    game_ = segment_real_start_ = segment_game_start_ = 0.0;
    floor_ = std::string(floor);
    pass_ = 1;
    splits_.clear();
    previous_best_ = 0.0;
    new_best_ = false;
    if (full_) {
        ++attempts_;
        SaveRecords();
    }
    if (livesplit_) {
        livesplit_->Send("reset");
        livesplit_->Send("initgametime");
        livesplit_->Send("starttimer");
        // LiveSplit's game time moves only with the game's own (setgametime)
        livesplit_->Send("pausegametime");
        livesplit_sent_ = start_;
    }
    LogInfo("speedrun: run started on {} ({})", floor_, full_ ? "full run" : "not from the start, no records");
}

void SpeedrunTimer::Split(std::string_view floor, int pass) {
    if (state_ != State::Running) return;
    SpeedrunSplit split;
    split.floor = std::string(floor);
    split.pass = std::max(1, pass);
    split.real_total = Real();
    split.game_total = game_;
    split.real = split.real_total - segment_real_start_;
    split.game = split.game_total - segment_game_start_;
    splits_.push_back(split);
    segment_real_start_ = split.real_total;
    segment_game_start_ = split.game_total;
    last_split_at_ = std::chrono::steady_clock::now();
    if (livesplit_) {
        SendGameTime();
        livesplit_->Send("split");
    }
    LogInfo("speedrun: split {} {} real {} game {} (run {} / {})", splits_.size(), split.Key(), Format(split.real), Format(split.game),
            Format(split.real_total), Format(split.game_total));
}

void SpeedrunTimer::Finish(std::string_view floor, int pass) {
    if (state_ != State::Running) return;
    if (floor == "ending" && !splits_.empty()) {
        // the final loop's door already split it (NextFloor f160 -> ending): the few ticks to GotoEnding belong to it
        SpeedrunSplit& last = splits_.back();
        const double real = Real();
        last.real += real - last.real_total;
        last.game += game_ - last.game_total;
        last.real_total = segment_real_start_ = real;
        last.game_total = segment_game_start_ = game_;
        if (livesplit_) SendGameTime();
    } else {
        Split(floor, pass);
    }
    real_final_ = splits_.back().real_total;
    state_ = State::Finished;
    floor_.clear();
    const bool game_clock = ShowsGameTime();
    previous_best_ = Best(game_clock).total;
    if (full_) {
        ++finished_runs_;
        const double shown = game_clock ? game_ : real_final_;
        new_best_ = previous_best_ <= 0.0 || shown < previous_best_;
        auto update = [&](Record& best, double total) {
            if (best.total <= 0.0 || total < best.total) {
                best.total = total;
                best.splits = splits_;
            }
        };
        update(best_real_, real_final_);
        update(best_game_, game_);
        auto gold = [&](std::vector<std::pair<std::string, double>>& golds, bool game) {
            for (const SpeedrunSplit& s : splits_) {
                const double t = game ? s.game : s.real;
                auto it = std::find_if(golds.begin(), golds.end(), [&](const auto& g) { return g.first == s.Key(); });
                if (it == golds.end()) golds.emplace_back(s.Key(), t);
                else if (t < it->second) it->second = t;
            }
        };
        gold(gold_real_, false);
        gold(gold_game_, true);
        SaveRecords();
        WriteLss();
    }
    AppendHistory();
    LogInfo("speedrun: run finished: real {} game {}{}", Format(real_final_), Format(game_),
            full_ ? (new_best_ ? ", new personal best" : "") : " (not from the start)");
}

void SpeedrunTimer::Abandon(std::string_view why) {
    if (state_ != State::Running) return;
    real_final_ = Real();
    state_ = State::Idle;
    splits_.clear();
    if (livesplit_) livesplit_->Send("reset");
    LogInfo("speedrun: run abandoned ({})", why);
}

void SpeedrunTimer::SendGameTime() {
    if (!livesplit_) return;
    livesplit_->Send("setgametime " + LiveSplitTime(game_));
    livesplit_sent_ = std::chrono::steady_clock::now();
}

void SpeedrunTimer::Poll() {
    if (!livesplit_) return;
    if (state_ == State::Running && Seconds(std::chrono::steady_clock::now() - livesplit_sent_) >= 0.5) SendGameTime();
    livesplit_->Poll();
}

double SpeedrunTimer::BestTotal() const {
    return Best(ShowsGameTime()).total;
}

bool SpeedrunTimer::LastSplitDelta(double& delta) const {
    if (splits_.empty() || !full_) return false;
    // after the finish the record may be this run: the total against the record it replaced
    if (state_ == State::Finished) {
        if (previous_best_ <= 0.0) return false;
        delta = (ShowsGameTime() ? game_ : real_final_) - previous_best_;
        return true;
    }
    const Record& best = Best(ShowsGameTime());
    const size_t i = splits_.size() - 1;
    if (i >= best.splits.size() || best.splits[i].Key() != splits_[i].Key()) return false;
    const SpeedrunSplit& now = splits_[i];
    delta = ShowsGameTime() ? now.game_total - best.splits[i].game_total : now.real_total - best.splits[i].real_total;
    return true;
}

std::string SpeedrunTimer::Describe() const {
    std::string text;
    for (const SpeedrunSplit& s : splits_) {
        if (!text.empty()) text += " | ";
        text += std::format("{} {} / {}", s.Key(), Format(s.real), Format(s.game));
    }
    return text;
}

// speedrun.ini: [stats] attempts and finished full runs, [best_real] and [best_game] (total and the record run's splits as
// key:real_total:game_total), [gold_real] and [gold_game] (best segment per key). Seconds.
void SpeedrunTimer::LoadRecords() {
    records_loaded_ = true;
    best_real_ = best_game_ = {};
    gold_real_.clear();
    gold_game_.clear();
    attempts_ = finished_runs_ = 0;
    if (directory_.empty()) return;
    std::ifstream in(directory_ / kRecordsFile);
    if (!in) return;
    std::string section;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == ';') continue;
        if (line[0] == '[') {
            section = line.substr(1, line.find(']') - 1);
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            s.erase(0, s.find_first_not_of(" \t"));
            s.erase(s.find_last_not_of(" \t") + 1);
            return s;
        };
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        try {
            if (section == "stats" && key == "attempts") attempts_ = std::stoi(value);
            else if (section == "stats" && key == "finished") finished_runs_ = std::stoi(value);
            else if (section == "best_real" || section == "best_game") {
                Record& r = section == "best_real" ? best_real_ : best_game_;
                if (key == "total") r.total = std::stod(value);
                if (key == "splits") {
                    std::istringstream items(value);
                    std::string item;
                    while (std::getline(items, item, ',')) {
                        const size_t a = item.find(':');
                        const size_t b = item.find(':', a + 1);
                        if (a == std::string::npos || b == std::string::npos) continue;
                        SpeedrunSplit s;
                        const std::string k = item.substr(0, a);
                        const size_t hash = k.find('#');
                        s.floor = k.substr(0, hash);
                        s.pass = hash == std::string::npos ? 1 : std::stoi(k.substr(hash + 1));
                        s.real_total = std::stod(item.substr(a + 1, b - a - 1));
                        s.game_total = std::stod(item.substr(b + 1));
                        s.real = s.real_total - (r.splits.empty() ? 0.0 : r.splits.back().real_total);
                        s.game = s.game_total - (r.splits.empty() ? 0.0 : r.splits.back().game_total);
                        r.splits.push_back(s);
                    }
                }
            } else if (section == "gold_real") gold_real_.emplace_back(key, std::stod(value));
            else if (section == "gold_game") gold_game_.emplace_back(key, std::stod(value));
        } catch (...) {
            LogWarn("speedrun: {} line '{}' ignored", kRecordsFile, line);
        }
    }
    LogInfo("speedrun: records read ({} attempts, {} finished, best real {}, best game {})", attempts_, finished_runs_,
            best_real_.total > 0.0 ? Format(best_real_.total) : "none", best_game_.total > 0.0 ? Format(best_game_.total) : "none");
}

void SpeedrunTimer::SaveRecords() {
    if (directory_.empty()) return;
    std::ostringstream text;
    text << "; P.T. port speedrun records (Extras > Speedrun timer), seconds. Real time: the wall clock from getting control to\n"
         << "; the ending, everything included. Game time: unpaused play without the stage loads after a game over.\n\n"
         << "[stats]\nattempts = " << attempts_ << "\nfinished = " << finished_runs_ << "\n";
    auto record = [&](const char* name, const Record& r) {
        text << "\n[" << name << "]\ntotal = " << std::format("{:.3f}", r.total) << "\nsplits = ";
        for (size_t i = 0; i < r.splits.size(); ++i) {
            text << (i ? "," : "") << std::format("{}:{:.3f}:{:.3f}", r.splits[i].Key(), r.splits[i].real_total, r.splits[i].game_total);
        }
        text << "\n";
    };
    record("best_real", best_real_);
    record("best_game", best_game_);
    for (const auto& [name, golds] : {std::pair{"gold_real", &gold_real_}, std::pair{"gold_game", &gold_game_}}) {
        text << "\n[" << name << "]\n";
        for (const auto& [key, t] : *golds) text << key << " = " << std::format("{:.3f}", t) << "\n";
    }
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    std::ofstream out(directory_ / kRecordsFile, std::ios::binary);
    out << text.str();
    if (!out) LogWarn("speedrun: cannot write {}", (directory_ / kRecordsFile).string());
}

void SpeedrunTimer::AppendHistory() {
    if (directory_.empty()) return;
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char stamp[32] = {};
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
    std::ofstream out(directory_ / kHistoryFile, std::ios::binary | std::ios::app);
    out << std::format("{} {} real {} game {}  {}\n", stamp, full_ ? "full" : "partial", Format(real_final_), Format(game_), Describe());
}

// speedrun_pb.lss: the record run of the shown clock as a LiveSplit splits file (its segments named as the overlay names
// them), with both clocks of that run as the personal best and the best segments
void SpeedrunTimer::WriteLss() const {
    if (directory_.empty()) return;
    const Record& pb = Best(ShowsGameTime());
    if (pb.splits.empty()) return;
    std::ostringstream x;
    x << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Run version=\"1.7.0\">\n  <GameIcon />\n  <GameName>P.T.</GameName>\n"
      << "  <CategoryName>Any% (PC port)</CategoryName>\n  <Metadata>\n    <Run id=\"\" />\n"
      << "    <Platform usesEmulator=\"False\">PC</Platform>\n    <Region>\n    </Region>\n    <Variables />\n  </Metadata>\n"
      << "  <Offset>00:00:00</Offset>\n  <AttemptCount>" << attempts_ << "</AttemptCount>\n  <AttemptHistory />\n  <Segments>\n";
    auto gold = [](const std::vector<std::pair<std::string, double>>& golds, const std::string& key) {
        for (const auto& [k, t] : golds) if (k == key) return t;
        return 0.0;
    };
    for (const SpeedrunSplit& s : pb.splits) {
        x << "    <Segment>\n      <Name>" << Escape(SegmentName(s)) << "</Name>\n      <Icon />\n      <SplitTimes>\n"
          << "        <SplitTime name=\"Personal Best\">\n          <RealTime>" << LssTime(s.real_total) << "</RealTime>\n"
          << "          <GameTime>" << LssTime(s.game_total) << "</GameTime>\n        </SplitTime>\n      </SplitTimes>\n"
          << "      <BestSegmentTime>\n        <RealTime>" << LssTime(gold(gold_real_, s.Key())) << "</RealTime>\n"
          << "        <GameTime>" << LssTime(gold(gold_game_, s.Key())) << "</GameTime>\n      </BestSegmentTime>\n"
          << "      <SegmentHistory />\n    </Segment>\n";
    }
    x << "  </Segments>\n  <AutoSplitterSettings />\n</Run>\n";
    std::ofstream out(directory_ / kLssFile, std::ios::binary);
    out << x.str();
}

}
