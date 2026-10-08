#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

struct whisper_context;
struct whisper_vad_context;

namespace pt {

class VoiceRecognizer {
public:
    static constexpr int kSampleRate = 16000;

    struct Settings {
        float start_probability = 0.5f;
        float end_probability = 0.35f;
        float start_seconds = 0.064f;
        float end_seconds = 0.35f;
        float preroll_seconds = 0.3f;
        float min_speech_seconds = 0.1f;
        float max_segment_seconds = 5.0f;
        float target_floor_db = -62.0f;
        float max_gain_db = 30.0f;
        int max_words = 12;
        float jack_token_probability = 0.04f;
        int threads = 0;
        int priority = 0;
        bool bridge_onset = true;
        int beam = 0;
        std::string prompt;
        bool suppress_nst = true;
        int rescue = 4;
        std::string rescue_model;
        float rescue_jack_probability = 0.04f;
        float rescue_probability = 0.0f;
        int rescue_words = 3;
        float rescue_seconds = 3.0f;
    };

    enum class State { Off, Loading, Ready, Failed };
    enum class Failure { None, Files, Runtime, Cpu, Model };

    struct Result {
        std::string text;
        float jack_probability = 0.0f;
        float seconds = 0.0f;
        float decode_ms = 0.0f;
        bool detected = false;
    };

    VoiceRecognizer();
    VoiceRecognizer(const VoiceRecognizer&) = delete;
    VoiceRecognizer& operator=(const VoiceRecognizer&) = delete;
    ~VoiceRecognizer();

    bool Init(const std::filesystem::path& model_dir, const std::string& keyword);
    void Shutdown();
    void Reset();
    bool Feed(std::span<const int16_t> samples);
    bool Finish();
    bool Drain();

    State GetState() const { return state_.load(); }
    Failure GetFailure() const { return failure_.load(); }
    const std::string& Keyword() const { return keyword_; }
    std::string LastHypothesis() const;
    uint32_t Utterances() const { return utterances_.load(); }
    std::vector<Result> TakeResults();

    static bool MatchesKeyword(std::string_view text, int max_words, int* word_count = nullptr);

    Settings settings;

private:
    void Run(std::filesystem::path model_dir);
    bool LoadModels(const std::filesystem::path& model_dir);
    void ProcessChunk(const float* chunk);
    void CloseSegment(bool forced);
    struct Decoded {
        std::string text;
        float probability = 0.0f;
        bool ok = false;
    };
    Decoded Decode(whisper_context* ctx, const std::vector<int>& jack_tokens, const std::vector<float>& audio, const char* prompt, int beam, float temperature, int best_of);
    Result Transcribe(std::vector<float> audio);
    void FreeModels();

    std::string keyword_;
    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::vector<int16_t> input_;
    bool stop_ = false;
    bool reset_ = false;
    bool flush_ = false;
    bool busy_ = false;
    uint64_t fed_ = 0;
    uint64_t done_ = 0;
    std::atomic<State> state_{State::Off};
    std::atomic<Failure> failure_{Failure::None};
    std::atomic<bool> abort_{false};
    std::atomic<uint32_t> utterances_{0};
    std::atomic<uint32_t> detections_{0};
    uint32_t reported_detections_ = 0;
    std::string last_hypothesis_;
    std::vector<Result> results_;

    whisper_context* whisper_ = nullptr;
    whisper_context* whisper2_ = nullptr;
    std::vector<int> jack_tokens2_;
    whisper_vad_context* vad_ = nullptr;
    int threads_ = 1;
    std::vector<int> jack_tokens_;
    std::vector<float> pending_;
    std::vector<float> preroll_;
    std::vector<float> segment_;
    std::deque<float> level_history_;
    float noise_db_ = -90.0f;
    float gain_ = 1.0f;
    bool in_speech_ = false;
    int speech_chunks_ = 0;
    int speech_gap_chunks_ = 0;
    int candidate_voiced_chunks_ = 0;
    int silence_chunks_ = 0;
    int voiced_chunks_ = 0;
};

}
