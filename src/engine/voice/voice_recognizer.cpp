#include "engine/voice/voice_recognizer.h"

#include <whisper.h>
#include <ggml-backend-impl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <utility>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <dlfcn.h>
#include <pthread.h>
#else
#include <dlfcn.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "engine/core/log.h"
#include "engine/platform/os.h"

namespace pt {
namespace {

constexpr int kChunk = 512;
constexpr const char* kDefaultWhisperModel = "ggml-base.en-q5_1.bin";
constexpr const char* kDefaultRescueModel = "ggml-small.en-q5_1.bin";
constexpr const char* kVadModel = "ggml-silero-v6.2.0.bin";

std::string WhisperModelName() {
    const char* name = std::getenv("PT_VOICE_MODEL");
    return name && name[0] ? name : kDefaultWhisperModel;
}

bool VoiceDiagnosticsEnabled() {
    static const bool enabled = [] {
#ifdef _WIN32
        char* value = nullptr;
        size_t length = 0;
        _dupenv_s(&value, &length, "PT_VOICE_DIAGNOSTICS");
        const bool present = value != nullptr;
        std::free(value);
        return present;
#else
        return std::getenv("PT_VOICE_DIAGNOSTICS") != nullptr;
#endif
    }();
    return enabled;
}

void WhisperLog(ggml_log_level level, const char* text, void*) {
    if (level != GGML_LOG_LEVEL_ERROR && level != GGML_LOG_LEVEL_WARN) return;
    std::string_view line(text ? text : "");
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
    if (!line.empty()) LogWarn("voice: whisper: {}", line);
}

struct MemoryLoader {
    std::vector<char> data;
    size_t offset = 0;

    static size_t Read(void* context, void* output, size_t size) {
        auto* self = static_cast<MemoryLoader*>(context);
        const size_t n = std::min(size, self->data.size() - self->offset);
        std::memcpy(output, self->data.data() + self->offset, n);
        self->offset += n;
        return n;
    }
    static bool Eof(void* context) {
        auto* self = static_cast<MemoryLoader*>(context);
        return self->offset >= self->data.size();
    }
    static void Close(void*) {}

    bool Open(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return false;
        data.resize(static_cast<size_t>(file.tellg()));
        file.seekg(0);
        file.read(data.data(), static_cast<std::streamsize>(data.size()));
        offset = 0;
        return static_cast<bool>(file);
    }
    whisper_model_loader Loader() { return {this, Read, Eof, Close}; }
};

struct WhisperApi {
#define PT_WHISPER_FUNCTIONS(X)                                                                      \
    X(whisper_context_default_params) X(whisper_free) X(whisper_full) X(whisper_full_default_params)     \
    X(whisper_full_get_segment_text) X(whisper_full_n_segments) X(whisper_init_with_params) X(whisper_log_set)  \
    X(whisper_n_vocab) X(whisper_tokenize) X(whisper_vad_default_context_params) X(whisper_vad_detect_speech_no_reset) \
    X(whisper_vad_free) X(whisper_vad_init_with_params) X(whisper_vad_n_probs) X(whisper_vad_probs) X(whisper_vad_reset_state)
#define PT_WHISPER_POINTER(name) decltype(&::name) name = nullptr;
    PT_WHISPER_FUNCTIONS(PT_WHISPER_POINTER)
#undef PT_WHISPER_POINTER
    bool ready = false;
    std::string cpu;
    void* cpu_backend = nullptr;
    decltype(&::ggml_backend_register) register_backend = nullptr;
};
WhisperApi g_api;
std::mutex g_api_mutex;

#ifdef _WIN32
using Library = HMODULE;
constexpr const char* kLibraryPrefix = "";
constexpr const char* kLibraryExtension = ".dll";
constexpr const char* kModuleExtension = ".dll";
Library LoadNear(const std::filesystem::path& path) {
    return LoadLibraryExW(std::filesystem::absolute(path).c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
}
void* Symbol(Library library, const char* name) { return reinterpret_cast<void*>(GetProcAddress(library, name)); }
void Unload(Library library) { FreeLibrary(library); }
std::string LoadError() {
    const DWORD code = GetLastError();
    char text[256] = {};
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, text, sizeof(text) - 1, nullptr);
    while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == '\r' || text[n - 1] == ' ')) text[--n] = 0;
    return n > 0 ? std::format("Windows error {}: {}", code, text) : std::format("Windows error {}", code);
}
#else
using Library = void*;
constexpr const char* kLibraryPrefix = "lib";
#ifdef __APPLE__
/* whisper and ggml are shared libraries (.dylib); ggml's CPU variants are CMake MODULE libraries, .so on macOS as well */
constexpr const char* kLibraryExtension = ".dylib";
#else
constexpr const char* kLibraryExtension = ".so";
#endif
constexpr const char* kModuleExtension = ".so";
Library LoadNear(const std::filesystem::path& path) { return dlopen(std::filesystem::absolute(path).c_str(), RTLD_NOW | RTLD_GLOBAL); }
void* Symbol(Library library, const char* name) { return dlsym(library, name); }
void Unload(Library library) { dlclose(library); }
std::string LoadError() {
    const char* error = dlerror();
    return error ? error : "unknown error";
}
#endif

std::string LibraryName(const char* name) { return std::string(kLibraryPrefix) + name + kLibraryExtension; }

bool LoadRuntime(const std::filesystem::path& dir, VoiceRecognizer::Failure& failure) {
    std::lock_guard lock(g_api_mutex);
    if (g_api.ready) return true;
    failure = VoiceRecognizer::Failure::Runtime;
    const Library base = LoadNear(dir / LibraryName("ggml-base"));
    std::string error = base ? "" : LoadError();
    const Library ggml = base ? LoadNear(dir / LibraryName("ggml")) : nullptr;
    if (base && !ggml) error = LoadError();
    const Library whisper = ggml ? LoadNear(dir / LibraryName("whisper")) : nullptr;
    if (ggml && !whisper) error = LoadError();
    if (!whisper) {
        LogError("voice: cannot load {} from {} ({})", LibraryName(!base ? "ggml-base" : !ggml ? "ggml" : "whisper"), os::PathToUtf8(dir), error);
        return false;
    }
    bool complete = true;
#define PT_WHISPER_RESOLVE(name)                                                          \
    g_api.name = reinterpret_cast<decltype(&::name)>(Symbol(whisper, #name));        \
    complete = complete && g_api.name != nullptr;
    PT_WHISPER_FUNCTIONS(PT_WHISPER_RESOLVE)
#undef PT_WHISPER_RESOLVE
    g_api.register_backend = reinterpret_cast<decltype(g_api.register_backend)>(Symbol(ggml, "ggml_backend_register"));
    if (!complete || !g_api.register_backend) {
        LogError("voice: missing export in {} or {}", LibraryName("whisper"), LibraryName("ggml"));
        return false;
    }
    g_api.whisper_log_set(WhisperLog, nullptr);
    const char* forced = std::getenv("PT_VOICE_CPU");
    std::filesystem::path best;
    bool registered = false;
    int best_score = 0;
    int candidates = 0;
    int loaded = 0;
    std::string load_error;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = os::PathToUtf8(entry.path().filename());
        const std::string prefix = std::string(kLibraryPrefix) + "ggml-cpu-";
        if (!name.starts_with(prefix) || entry.path().extension() != kModuleExtension) continue;
        const std::string variant = name.substr(prefix.size(), name.size() - prefix.size() - std::strlen(kModuleExtension));
        if (forced && variant != forced) continue;
        ++candidates;
        const Library module = LoadNear(entry.path());
        if (!module) {
            load_error = std::format("{}: {}", name, LoadError());
            continue;
        }
        ++loaded;
        using Score = int (*)();
        const auto score = reinterpret_cast<Score>(Symbol(module, "ggml_backend_score"));
        const int value = score ? score() : 0;
        Unload(module);
        if (value > best_score) {
            best_score = value;
            best = entry.path();
            g_api.cpu = variant;
        }
    }
    if (!best.empty()) {
        g_api.cpu_backend = LoadNear(best);
        using InitBackend = ggml_backend_reg_t (*)();
        const auto init_backend = g_api.cpu_backend
            ? reinterpret_cast<InitBackend>(Symbol(reinterpret_cast<Library>(g_api.cpu_backend), "ggml_backend_init"))
            : nullptr;
        if (init_backend) {
            if (const auto registry = init_backend(); registry && registry->api_version == GGML_BACKEND_API_VERSION) {
                g_api.register_backend(registry);
                registered = true;
            }
        }
    }
    if (!registered) {
        if (g_api.cpu_backend) {
            Unload(reinterpret_cast<Library>(g_api.cpu_backend));
            g_api.cpu_backend = nullptr;
        }
        const std::string where = os::PathToUtf8(dir);
        const std::string note = forced ? std::format(" (PT_VOICE_CPU={})", forced) : "";
        if (candidates == 0) {
            LogError("voice: no {}ggml-cpu-*{} library in {}{}", kLibraryPrefix, kModuleExtension, where, note);
        } else if (loaded == 0) {
            LogError("voice: none of the {} ggml-cpu libraries in {} could be loaded ({}); a path or blocked or missing DLL problem, not the CPU{}",
                     candidates, where, load_error, note);
        } else if (best_score == 0) {
            LogError("voice: this CPU runs none of the {} ggml-cpu variants in {}{}", loaded, where, note);
            failure = VoiceRecognizer::Failure::Cpu;
        } else {
            LogError("voice: ggml-cpu-{} in {} loaded but did not register (ggml backend API version mismatch or no ggml_backend_init)", g_api.cpu, where);
        }
        return false;
    }
    failure = VoiceRecognizer::Failure::None;
    g_api.ready = true;
    LogInfo("voice: whisper.cpp CPU code ggml-cpu-{} (score {})", g_api.cpu, best_score);
    return true;
}

void WriteSegment(const std::filesystem::path& folder, const std::vector<float>& audio) {
    static std::atomic<int> count{0};
    std::ofstream file(folder / std::format("voice_segment_{:04}.wav", count++), std::ios::binary);
    const uint32_t bytes = static_cast<uint32_t>(audio.size() * 2);
    auto u32 = [&](uint32_t v) { file.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { file.write(reinterpret_cast<const char*>(&v), 2); };
    file.write("RIFF", 4);
    u32(36 + bytes);
    file.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(1);
    u32(16000);
    u32(32000);
    u16(2);
    u16(16);
    file.write("data", 4);
    u32(bytes);
    for (const float s : audio) u16(static_cast<uint16_t>(static_cast<int16_t>(std::lround(std::clamp(s, -1.0f, 1.0f) * 32767.0f))));
}

std::string Lower(std::string_view text) {
    static constexpr std::pair<std::string_view, std::string_view> kFold[] = {
        {"\xC3\xA0", "a"}, {"\xC3\xA1", "a"}, {"\xC3\xA2", "a"}, {"\xC3\xA3", "a"}, {"\xC3\xA4", "a"}, {"\xC3\xA5", "a"},
        {"\xC3\x84", "a"}, {"\xC3\xA6", "ae"}, {"\xC3\x86", "ae"}, {"\xC3\xA7", "c"}, {"\xC3\x87", "c"}, {"\xC3\xA8", "e"},
        {"\xC3\xA9", "e"}, {"\xC3\xAA", "e"}, {"\xC3\xAB", "e"}, {"\xC3\x89", "e"}, {"\xC3\xB6", "o"}, {"\xC3\xBC", "u"},
        {"\xC4\xB1", "i"}, {"\xC4\x9F", "g"}, {"\xC5\x9F", "s"}, {"\xC5\xBE", "z"}, {"\xC5\xBD", "z"}, {"\xC4\x8D", "c"},
        {"\xC4\x8C", "c"}, {"\xC4\xB5", "j"}, {"\xC4\xB4", "j"},
    };
    std::string out;
    for (size_t i = 0; i < text.size();) {
        bool folded = false;
        for (const auto& [from, to] : kFold) {
            if (text.substr(i).starts_with(from)) {
                out += to;
                i += from.size();
                folded = true;
                break;
            }
        }
        if (folded) continue;
        const char c = text[i++];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out += c;
        else if (c >= 'A' && c <= 'Z') out += static_cast<char>(c - 'A' + 'a');
        else if (c == '\'') continue;
        else out += ' ';
    }
    return out;
}

bool SoundsLikeJack(std::string_view w) {
    auto take = [&](std::initializer_list<std::string_view> options) {
        for (const std::string_view o : options) {
            if (w.starts_with(o)) {
                w.remove_prefix(o.size());
                return true;
            }
        }
        return false;
    };
    if (!take({"dzh", "dzj", "dj", "dz", "zh", "j"}) || !take({"ae", "ah", "eh", "aa", "a", "e"})) return false;
    return w == "k" || w == "c" || w == "ck" || w == "kk" || w == "q" || w == "cq" || w == "kh";
}

enum class Spelling { None, Word, Short };

/* Only "Jack" counts: the original's grammar holds that one word and its check is a strcmp (0x927090), so "Jarith" misses there too. */
Spelling KeywordSpelling(std::string_view w) {
    for (const char* word : {"jack", "jacks", "jacked", "jak", "jac", "jaq", "jakk", "jacc", "jaque", "jaques", "jacque", "jacques"}) {
        if (w == word) return Spelling::Word;
    }
    if (w.starts_with("jack") || w.starts_with("jacq") || w == "jock" || w == "jake" || w == "jakes" || SoundsLikeJack(w)) return Spelling::Short;
    return Spelling::None;
}

struct FirstStep {
    const std::vector<int>* tokens = nullptr;
    float probability = 0.0f;
    bool done = false;
};

void FirstStepLogits(whisper_context* ctx, whisper_state*, const whisper_token_data*, int n_tokens, float* logits, void* user) {
    auto* step = static_cast<FirstStep*>(user);
    if (n_tokens != 0 || step->done) return;
    step->done = true;
    const int n = g_api.whisper_n_vocab(ctx);
    float top = -INFINITY;
    for (int i = 0; i < n; ++i) top = std::max(top, logits[i]);
    double sum = 0.0;
    for (int i = 0; i < n; ++i) sum += std::exp(static_cast<double>(logits[i] - top));
    double p = 0.0;
    for (const int token : *step->tokens) p += std::exp(static_cast<double>(logits[token] - top));
    step->probability = sum > 0.0 ? static_cast<float>(p / sum) : 0.0f;
}

}

VoiceRecognizer::VoiceRecognizer() = default;

VoiceRecognizer::~VoiceRecognizer() {
    Shutdown();
}

bool VoiceRecognizer::MatchesKeyword(std::string_view text, int max_words, int* word_count) {
    const std::string lower = Lower(text);
    std::vector<std::string_view> words;
    Spelling found = Spelling::None;
    size_t start = 0;
    while (start < lower.size()) {
        const size_t end = std::min(lower.find(' ', start), lower.size());
        if (end > start) {
            const std::string_view word = std::string_view(lower).substr(start, end - start);
            if (std::find(words.begin(), words.end(), word) == words.end()) words.push_back(word);
            found = std::max(found, KeywordSpelling(word));
        }
        start = end + 1;
    }
    const int count = static_cast<int>(words.size());
    if (word_count) *word_count = count;
    return (found == Spelling::Word && count <= max_words) || (found == Spelling::Short && count <= 3);
}

static void ApplyTune(VoiceRecognizer::Settings& s) {
    const char* text = std::getenv("PT_VOICE_TUNE");
    if (!text) return;
    std::string_view rest(text);
    while (!rest.empty()) {
        const size_t comma = std::min(rest.find(','), rest.size());
        const std::string_view item = rest.substr(0, comma);
        rest.remove_prefix(std::min(comma + 1, rest.size()));
        const size_t eq = item.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string name(item.substr(0, eq));
        const float v = std::strtof(std::string(item.substr(eq + 1)).c_str(), nullptr);
        if (name == "start") s.start_seconds = v;
        else if (name == "end") s.end_seconds = v;
        else if (name == "startp") s.start_probability = v;
        else if (name == "endp") s.end_probability = v;
        else if (name == "preroll") s.preroll_seconds = v;
        else if (name == "minspeech") s.min_speech_seconds = v;
        else if (name == "maxsegment") s.max_segment_seconds = v;
        else if (name == "floor") s.target_floor_db = v;
        else if (name == "maxgain") s.max_gain_db = v;
        else if (name == "maxwords") s.max_words = static_cast<int>(v);
        else if (name == "jackp") s.jack_token_probability = v;
        else if (name == "threads") s.threads = static_cast<int>(v);
        else if (name == "prio") s.priority = static_cast<int>(v);
        else if (name == "bridge") s.bridge_onset = v != 0.0f;
        else if (name == "beam") s.beam = static_cast<int>(v);
        else if (name == "rescue") s.rescue = static_cast<int>(v);
        else if (name == "rescuep") s.rescue_probability = v;
        else if (name == "rescuewords") s.rescue_words = static_cast<int>(v);
        else if (name == "rescuesec") s.rescue_seconds = v;
        else if (name == "rescuejackp") s.rescue_jack_probability = v;
        else if (name == "nst") s.suppress_nst = v != 0.0f;
        else continue;
        LogInfo("voice: PT_VOICE_TUNE {}={}", name, v);
    }
}

bool VoiceRecognizer::Init(const std::filesystem::path& model_dir, const std::string& keyword) {
    Shutdown();
    keyword_ = keyword;
    ApplyTune(settings);
    if (const char* prompt = std::getenv("PT_VOICE_PROMPT")) settings.prompt = prompt;
    if (const char* model = std::getenv("PT_VOICE_RESCUE_MODEL")) settings.rescue_model = model;
    for (const std::string& name : {WhisperModelName(), std::string(kVadModel), LibraryName("whisper")}) {
        if (!std::filesystem::is_regular_file(model_dir / name)) {
            LogError("voice: {} is missing from {}", name, os::PathToUtf8(model_dir));
            failure_ = Failure::Files;
            state_ = State::Failed;
            return false;
        }
    }
    stop_ = false;
    failure_ = Failure::None;
    state_ = State::Loading;
    worker_ = std::thread(&VoiceRecognizer::Run, this, model_dir);
    return true;
}

void VoiceRecognizer::Shutdown() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    abort_ = true;
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard lock(mutex_);
    stop_ = false;
    abort_ = false;
    input_.clear();
    fed_ = done_ = 0;
    reset_ = flush_ = busy_ = false;
    failure_ = Failure::None;
    state_ = State::Off;
}

void VoiceRecognizer::Reset() {
    {
        std::lock_guard lock(mutex_);
        fed_ -= input_.size();
        input_.clear();
        reset_ = true;
        last_hypothesis_.clear();
    }
    wake_.notify_all();
}

bool VoiceRecognizer::Feed(std::span<const int16_t> samples) {
    if (state_ == State::Off || state_ == State::Failed) return false;
    if (!samples.empty()) {
        size_t dropped = 0;
        size_t queued = 0;
        {
            std::lock_guard lock(mutex_);
            input_.insert(input_.end(), samples.begin(), samples.end());
            fed_ += samples.size();
            const size_t limit = 8 * kSampleRate;
            if (input_.size() > limit) {
                const size_t drop = input_.size() - limit;
                input_.erase(input_.begin(), input_.begin() + static_cast<ptrdiff_t>(drop));
                fed_ -= drop;
                reset_ = true;
                dropped = drop;
                queued = input_.size();
            }
        }
        if (dropped != 0) {
            LogWarn("voice: microphone backlog dropped {:.2f} s of audio (kept {:.2f} s queued); resetting VAD segment",
                    static_cast<double>(dropped) / kSampleRate, static_cast<double>(queued) / kSampleRate);
        }
        wake_.notify_one();
    }
    const uint32_t detections = detections_.load();
    const bool heard = detections != reported_detections_;
    reported_detections_ = detections;
    return heard;
}

bool VoiceRecognizer::Drain() {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [&] {
        const State state = state_.load();
        return state == State::Failed || state == State::Off || (state == State::Ready && !busy_ && input_.empty() && !flush_ && !reset_);
    });
    lock.unlock();
    return Feed({});
}

bool VoiceRecognizer::Finish() {
    {
        std::lock_guard lock(mutex_);
        flush_ = true;
    }
    wake_.notify_one();
    return Drain();
}

std::string VoiceRecognizer::LastHypothesis() const {
    std::lock_guard lock(mutex_);
    return last_hypothesis_;
}

std::vector<VoiceRecognizer::Result> VoiceRecognizer::TakeResults() {
    std::lock_guard lock(mutex_);
    return std::exchange(results_, {});
}

bool VoiceRecognizer::LoadModels(const std::filesystem::path& model_dir) {
    const auto started = std::chrono::steady_clock::now();
    Failure failure = Failure::None;
    if (!LoadRuntime(model_dir, failure)) {
        failure_ = failure;
        return false;
    }
    failure_ = Failure::Model;
    MemoryLoader file;
    if (!file.Open(model_dir / kVadModel)) {
        LogError("voice: cannot read {}", os::PathToUtf8(model_dir / kVadModel));
        return false;
    }
    whisper_vad_context_params vad_params = g_api.whisper_vad_default_context_params();
    vad_params.n_threads = 1;
    vad_params.use_gpu = false;
    whisper_model_loader loader = file.Loader();
    vad_ = g_api.whisper_vad_init_with_params(&loader, vad_params);
    if (!vad_) {
        LogError("voice: VAD model load failed");
        return false;
    }
    /* whisper_vad_init leaves the LSTM state uninitialised; without this reset the VAD sometimes returns a constant 0.349 for every window. */
    g_api.whisper_vad_reset_state(vad_);
    if (!file.Open(model_dir / WhisperModelName())) {
        LogError("voice: cannot read {}", os::PathToUtf8(model_dir / WhisperModelName()));
        return false;
    }
    whisper_context_params params = g_api.whisper_context_default_params();
    params.use_gpu = false;
    params.flash_attn = true;
    loader = file.Loader();
    whisper_ = g_api.whisper_init_with_params(&loader, params);
    if (!whisper_) {
        LogError("voice: speech model load failed");
        return false;
    }
    jack_tokens_.clear();
    for (const char* spelling : {" Jack", " jack", " JACK"}) {
        whisper_token tokens[8];
        if (g_api.whisper_tokenize(whisper_, spelling, tokens, 8) == 1 &&
            std::find(jack_tokens_.begin(), jack_tokens_.end(), tokens[0]) == jack_tokens_.end()) {
            jack_tokens_.push_back(tokens[0]);
        }
    }
    if (settings.rescue == 4) {
        const std::string name = settings.rescue_model.empty() ? std::string(kDefaultRescueModel) : settings.rescue_model;
        MemoryLoader second_file;
        if (second_file.Open(model_dir / name)) {
            whisper_model_loader second_loader = second_file.Loader();
            whisper2_ = g_api.whisper_init_with_params(&second_loader, params);
        }
        if (!whisper2_) {
            LogWarn("voice: second opinion model {} not loaded, short utterances are decoded once", name);
        } else {
            for (const char* spelling : {" Jack", " jack", " JACK"}) {
                whisper_token tokens[8];
                if (g_api.whisper_tokenize(whisper2_, spelling, tokens, 8) == 1 &&
                    std::find(jack_tokens2_.begin(), jack_tokens2_.end(), tokens[0]) == jack_tokens2_.end()) {
                    jack_tokens2_.push_back(tokens[0]);
                }
            }
            LogInfo("voice: second opinion {}", name);
        }
    }
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    threads_ = settings.threads > 0 ? settings.threads : static_cast<int>(std::clamp(cores / 4, 1u, 4u));
    failure_ = Failure::None;
    LogInfo("voice: {} and {} loaded in {:.0f} ms, {} threads", WhisperModelName(), kVadModel,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(), threads_);
    return true;
}

void VoiceRecognizer::FreeModels() {
    if (whisper_) g_api.whisper_free(whisper_);
    if (whisper2_) g_api.whisper_free(whisper2_);
    whisper2_ = nullptr;
    if (vad_) g_api.whisper_vad_free(vad_);
    whisper_ = nullptr;
    vad_ = nullptr;
}

void VoiceRecognizer::Run(std::filesystem::path model_dir) {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), settings.priority < 0 ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL);
#elif defined(__APPLE__)
    pthread_set_qos_class_self_np(settings.priority < 0 ? QOS_CLASS_UTILITY : QOS_CLASS_USER_INITIATED, 0);
#else
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), settings.priority < 0 ? 10 : 0);
#endif
    const bool loaded = LoadModels(model_dir);
    if (!loaded) FreeModels();
    {
        std::lock_guard lock(mutex_);
        state_ = loaded ? State::Ready : State::Failed;
    }
    idle_.notify_all();
    if (!loaded) return;
    std::vector<int16_t> local;
    for (;;) {
        bool reset = false;
        bool flush = false;
        {
            std::unique_lock lock(mutex_);
            idle_.notify_all();
            wake_.wait(lock, [&] { return stop_ || reset_ || flush_ || !input_.empty(); });
            if (stop_) break;
            local.swap(input_);
            input_.clear();
            reset = std::exchange(reset_, false);
            flush = std::exchange(flush_, false);
            busy_ = true;
        }
        if (reset) {
            pending_.clear();
            preroll_.clear();
            segment_.clear();
            level_history_.clear();
            gain_ = 1.0f;
            in_speech_ = false;
            speech_chunks_ = speech_gap_chunks_ = candidate_voiced_chunks_ = silence_chunks_ = voiced_chunks_ = 0;
            g_api.whisper_vad_reset_state(vad_);
        }
        for (const int16_t s : local) pending_.push_back(static_cast<float>(s) / 32768.0f);
        size_t used = 0;
        while (pending_.size() - used >= kChunk) {
            ProcessChunk(pending_.data() + used);
            used += kChunk;
        }
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<ptrdiff_t>(used));
        if (flush) {
            if (!pending_.empty()) {
                pending_.resize(kChunk, 0.0f);
                ProcessChunk(pending_.data());
                pending_.clear();
            }
            if (in_speech_) CloseSegment(true);
        }
        {
            std::lock_guard lock(mutex_);
            done_ += local.size();
            busy_ = false;
        }
        local.clear();
    }
    FreeModels();
}

void VoiceRecognizer::ProcessChunk(const float* raw) {
    double energy = 0.0;
    for (int i = 0; i < kChunk; ++i) energy += static_cast<double>(raw[i]) * raw[i];
    const float db = static_cast<float>(10.0 * std::log10(energy / kChunk + 1e-12));
    if (!in_speech_) {
        level_history_.push_back(db);
        if (level_history_.size() > static_cast<size_t>(3 * kSampleRate / kChunk)) level_history_.pop_front();
        std::vector<float> sorted(level_history_.begin(), level_history_.end());
        const size_t at = sorted.size() / 10;
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<ptrdiff_t>(at), sorted.end());
        noise_db_ = std::max(sorted[at], -100.0f);
        gain_ = std::pow(10.0f, std::clamp(settings.target_floor_db - noise_db_, 0.0f, settings.max_gain_db) / 20.0f);
    }
    float lifted[kChunk];
    for (int i = 0; i < kChunk; ++i) lifted[i] = std::clamp(raw[i] * gain_, -1.0f, 1.0f);
    float p = 0.0f;
    if (g_api.whisper_vad_detect_speech_no_reset(vad_, lifted, kChunk) && g_api.whisper_vad_n_probs(vad_) > 0) p = g_api.whisper_vad_probs(vad_)[0];
    if (VoiceDiagnosticsEnabled()) {
        LogInfo("voice: VAD p={:.3f} gain={:.2f} speech={} start={}/{} silence={}/{} voiced={:.3f}s",
                p, gain_, in_speech_ ? 1 : 0, speech_chunks_,
                std::max(1, static_cast<int>(std::lround(settings.start_seconds * kSampleRate / kChunk))),
                silence_chunks_, std::max(1, static_cast<int>(std::lround(settings.end_seconds * kSampleRate / kChunk))),
                static_cast<float>(voiced_chunks_) * kChunk / kSampleRate);
    }
    const int start_chunks = std::max(1, static_cast<int>(std::lround(settings.start_seconds * kSampleRate / kChunk)));
    const int end_chunks = std::max(1, static_cast<int>(std::lround(settings.end_seconds * kSampleRate / kChunk)));
    const float voiced_probability = settings.start_probability * 0.5f;
    if (!in_speech_) {
        preroll_.insert(preroll_.end(), raw, raw + kChunk);
        const size_t keep = static_cast<size_t>(settings.preroll_seconds * kSampleRate) + kChunk;
        if (preroll_.size() > keep) preroll_.erase(preroll_.begin(), preroll_.end() - static_cast<ptrdiff_t>(keep));
        if (p >= settings.start_probability) {
            if (speech_chunks_ == 0) candidate_voiced_chunks_ = 0;
            ++speech_chunks_;
            ++candidate_voiced_chunks_;
            speech_gap_chunks_ = 0;
        } else if (settings.bridge_onset && speech_chunks_ > 0 && speech_gap_chunks_ == 0 && p >= voiced_probability) {
            ++speech_gap_chunks_;
            ++candidate_voiced_chunks_;
        } else {
            speech_chunks_ = speech_gap_chunks_ = candidate_voiced_chunks_ = 0;
        }
        if (speech_chunks_ >= start_chunks) {
            in_speech_ = true;
            segment_ = std::move(preroll_);
            preroll_.clear();
            silence_chunks_ = 0;
            voiced_chunks_ = candidate_voiced_chunks_;
            speech_gap_chunks_ = 0;
        }
        return;
    }
    segment_.insert(segment_.end(), raw, raw + kChunk);
    if (p >= settings.end_probability) {
        silence_chunks_ = 0;
        ++voiced_chunks_;
    } else {
        ++silence_chunks_;
    }
    if (silence_chunks_ >= end_chunks) {
        CloseSegment(false);
    } else if (segment_.size() >= static_cast<size_t>(settings.max_segment_seconds * kSampleRate)) {
        CloseSegment(true);
    }
}

void VoiceRecognizer::CloseSegment(bool forced) {
    in_speech_ = false;
    speech_chunks_ = speech_gap_chunks_ = candidate_voiced_chunks_ = 0;
    std::vector<float> audio = std::move(segment_);
    segment_.clear();
    const float voiced = static_cast<float>(voiced_chunks_) * kChunk / kSampleRate;
    const int min_voiced_chunks = std::max(1, static_cast<int>(std::floor(settings.min_speech_seconds * kSampleRate / kChunk)));
    if (voiced_chunks_ < min_voiced_chunks) {
        if (VoiceDiagnosticsEnabled()) {
            LogInfo("voice: discarded short VAD segment ({:.3f}s voiced < {:.3f}s, {:.3f}s captured, {} silence chunks{})",
                    voiced, static_cast<float>(min_voiced_chunks) * kChunk / kSampleRate,
                    static_cast<float>(audio.size()) / kSampleRate, silence_chunks_,
                    forced ? ", forced close" : "");
        }
        return;
    }
    if (!forced) {
        const size_t tail = static_cast<size_t>(silence_chunks_) * kChunk;
        const size_t keep_tail = static_cast<size_t>(0.2f * kSampleRate);
        if (tail > keep_tail && audio.size() > tail) audio.resize(audio.size() - (tail - keep_tail));
    }
    Result result = Transcribe(std::move(audio));
    if (result.text.empty() && result.decode_ms == 0.0f) return;
    const uint32_t count = ++utterances_;
    LogInfo("voice: utterance {} ({:.2f} s{}) heard '{}', p(jack) {:.3f}, decoded in {:.0f} ms{}", count, result.seconds,
            forced ? ", cut" : "", result.text, result.jack_probability, result.decode_ms, result.detected ? ": the word" : "");
    if (result.detected) ++detections_;
    std::lock_guard lock(mutex_);
    if (!result.text.empty()) last_hypothesis_ = result.text;
    results_.push_back(std::move(result));
    if (results_.size() > 64) results_.erase(results_.begin());
}

VoiceRecognizer::Decoded VoiceRecognizer::Decode(whisper_context* ctx, const std::vector<int>& jack_tokens, const std::vector<float>& audio, const char* prompt, int beam, float temperature, int best_of) {
    Decoded out;
    whisper_full_params params = g_api.whisper_full_default_params(beam > 1 ? WHISPER_SAMPLING_BEAM_SEARCH : WHISPER_SAMPLING_GREEDY);
    if (beam > 1) params.beam_search.beam_size = beam;
    params.n_threads = threads_;
    params.no_context = true;
    params.no_timestamps = true;
    params.single_segment = true;
    params.print_special = params.print_progress = params.print_realtime = params.print_timestamps = false;
    params.max_tokens = 16;
    params.language = "en";
    params.detect_language = false;
    params.suppress_blank = true;
    params.suppress_nst = settings.suppress_nst;
    params.temperature = temperature;
    params.temperature_inc = 0.0f;
    params.greedy.best_of = best_of;
    if (prompt && prompt[0]) params.initial_prompt = prompt;
    params.audio_ctx = std::clamp(static_cast<int>(audio.size() * 50 / kSampleRate) + 64, 384, 1500);
    FirstStep first;
    first.tokens = &jack_tokens;
    params.logits_filter_callback = FirstStepLogits;
    params.logits_filter_callback_user_data = &first;
    params.abort_callback = [](void* self) { return static_cast<VoiceRecognizer*>(self)->abort_.load(); };
    params.abort_callback_user_data = this;
    if (g_api.whisper_full(ctx, params, audio.data(), static_cast<int>(audio.size())) != 0) {
        if (!abort_) LogWarn("voice: whisper_full failed");
        return out;
    }
    for (int i = 0; i < g_api.whisper_full_n_segments(ctx); ++i) out.text += g_api.whisper_full_get_segment_text(ctx, i);
    while (!out.text.empty() && out.text.front() == ' ') out.text.erase(out.text.begin());
    out.probability = first.probability;
    out.ok = true;
    return out;
}

VoiceRecognizer::Result VoiceRecognizer::Transcribe(std::vector<float> audio) {
    Result result;
    result.seconds = static_cast<float>(audio.size()) / kSampleRate;
    const auto started = std::chrono::steady_clock::now();
    std::vector<float> magnitude(audio.size());
    for (size_t i = 0; i < audio.size(); ++i) magnitude[i] = std::abs(audio[i]);
    const size_t at = std::min(magnitude.size() - 1, magnitude.size() * 999 / 1000);
    std::nth_element(magnitude.begin(), magnitude.begin() + static_cast<ptrdiff_t>(at), magnitude.end());
    const float peak = magnitude[at];
    if (peak < 1e-5f) return result;
    const float scale = std::min(0.7f / peak, 3000.0f);
    for (float& s : audio) s = std::clamp(s * scale, -1.0f, 1.0f);
    /* whisper_full drops input under 1 s; pad to 1.5 s with silence, which the model expects after speech anyway. */
    const size_t minimum = static_cast<size_t>(1.5f * kSampleRate);
    if (audio.size() < minimum) audio.resize(minimum, 0.0f);

    if (const char* dump = std::getenv("PT_VOICE_DUMP")) WriteSegment(std::filesystem::path(dump), audio);
    Decoded first = Decode(whisper_, jack_tokens_, audio, settings.prompt.c_str(), settings.beam, 0.0f, 1);
    if (!first.ok) return result;
    result.text = first.text;
    result.jack_probability = first.probability;
    int words = 0;
    result.detected = MatchesKeyword(result.text, settings.max_words, &words);
    if (!result.detected && words <= 3 && result.jack_probability >= settings.jack_token_probability) result.detected = true;
    if (!result.detected && settings.rescue > 0 && words <= settings.rescue_words && result.seconds <= settings.rescue_seconds &&
        result.jack_probability >= settings.rescue_probability) {
        Decoded second;
        switch (settings.rescue) {
        case 1: second = Decode(whisper_, jack_tokens_, audio, "Jack.", 0, 0.0f, 1); break;
        case 2: second = Decode(whisper_, jack_tokens_, audio, settings.prompt.c_str(), 0, 0.4f, 5); break;
        case 4:
            if (whisper2_) second = Decode(whisper2_, jack_tokens2_, audio, settings.prompt.c_str(), 0, 0.0f, 1);
            break;
        default: second = Decode(whisper_, jack_tokens_, audio, settings.prompt.c_str(), 5, 0.0f, 1); break;
        }
        if (second.ok) {
            int words2 = 0;
            bool hit = MatchesKeyword(second.text, 3, &words2);
            if (!hit && words2 <= 3 && second.probability >= settings.rescue_jack_probability) hit = true;
            if (VoiceDiagnosticsEnabled() || std::getenv("PT_VOICE_RESCUE_LOG")) {
                LogInfo("voice: second look '{}' -> '{}' p {:.3f}{}", result.text, second.text, second.probability, hit ? ": the word" : "");
            }
            if (hit) {
                result.detected = true;
                result.text += " | " + second.text;
            }
        }
    }
    result.decode_ms = static_cast<float>(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
    return result;
}

}
