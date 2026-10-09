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

// Listens for the true end's word (gameplay.md 7, formats/voice.md). The caller feeds 16 kHz mono samples from the
// game thread; a worker thread loads the models, cuts utterances with the Silero VAD and transcribes each one with
// whisper.cpp on the CPU. Feed never blocks on the models.
class VoiceRecognizer {
public:
    static constexpr int kSampleRate = 16000;

    struct Settings {
        // Silero speech probability that opens a segment, and the one it must stay under to close it
        float start_probability = 0.5f;
        float end_probability = 0.35f;
        float start_seconds = 0.064f;    // speech this long opens a segment
        float end_seconds = 0.35f;       // silence this long closes it ("Hey, Jack" stays one segment)
        float preroll_seconds = 0.3f;
        float min_speech_seconds = 0.1f; // shorter segments are clicks, not words
        float max_segment_seconds = 5.0f;
        // the gain that lifts a quiet microphone for the VAD: the noise floor is raised toward this level, by at most max_gain_db
        float target_floor_db = -62.0f;
        float max_gain_db = 30.0f;
        // a transcript of more different words than this is a conversation, not the word said to the game; a word said
        // over the radio from the speakers shares its segment with the broadcast, so the limit is not tighter
        int max_words = 12;
        // a transcript of at most 3 words also counts when the model gave " Jack" this probability as its first token,
        // though it wrote another word: "Check", "Chuck", "Yeah" for a devoiced or clipped "Jack". 0.04 is above every
        // common word and the game's audio in tools/voice_check.py (at most 0.002) and takes only rhymes (deck,
        // tack, yak, Zack) as false accepts
        float jack_token_probability = 0.04f;
        int threads = 0; // 0: from the CPU's thread count
        // the worker's thread priority: 0 normal, -1 below normal. ggml's helper threads run at normal priority and wait for
        // each other at a barrier, so a below normal worker stalls the whole decode when the game uses the CPU (formats/voice.md)
        int priority = 0;
        // one moderate VAD frame may bridge the two strong frames that open a segment (a short word's onset)
        bool bridge_onset = true;
        int beam = 0; // beam search width, 0 or 1: greedy
        std::string prompt; // whisper's initial prompt for every decode (PT_VOICE_PROMPT, measurements only)
        bool suppress_nst = true;
        // a second decode of a short utterance (at most rescue_words words and rescue_seconds long, first token probability for
        // the word at least rescue_probability) that did not make the word: 4 a larger model, the second opinion (default:
        // small.en q5_1; the word counts when that model writes it); 0 off; 1 to 3 are experiments of formats/voice.md (prompt
        // "Jack.", sampling, beam search) that did not pay
        int rescue = 4;
        std::string rescue_model; // for rescue 4: the second whisper model file in the voice folder (empty: the default)
        float rescue_jack_probability = 0.04f; // the second model's first-token threshold, like jack_token_probability
        float rescue_probability = 0.0f;
        int rescue_words = 3;
        float rescue_seconds = 3.0f;
    };

    enum class State { Off, Loading, Ready, Failed };
    // why State::Failed: the model files are missing, the whisper.cpp/ggml libraries or every ggml-cpu-* would not load
    // (a path, a blocked or missing DLL), the CPU runs none of the ggml-cpu-* variants, or a model would not load
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

    // Starts the worker, which loads whisper.cpp and the models from model_dir; false when the files are missing.
    bool Init(const std::filesystem::path& model_dir, const std::string& keyword);
    void Shutdown();
    // Drops the queued audio and the segment in progress (a new microphone stream starts)
    void Reset();
    // Queues samples; true when an utterance with the word finished since the last call
    bool Feed(std::span<const int16_t> samples);
    // Closes the open segment and waits until everything queued is decoded (offline tests)
    bool Finish();
    // Waits until the queued audio is processed, without closing the open segment (offline tests)
    bool Drain();

    State GetState() const { return state_.load(); }
    Failure GetFailure() const { return failure_.load(); }
    const std::string& Keyword() const { return keyword_; }
    std::string LastHypothesis() const;
    uint32_t Utterances() const { return utterances_.load(); }
    std::vector<Result> TakeResults();

    // The transcript test (formats/voice.md): true when a word of text is the keyword or one of its accepted spellings
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

    // worker-only state
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
