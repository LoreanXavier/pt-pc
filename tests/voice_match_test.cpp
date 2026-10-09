#include <cstdio>
#include <chrono>
#include <filesystem>
#include <thread>
#include <string_view>

#include "engine/voice/voice_recognizer.h"
#include "engine/core/resource_path.h"

int main() {
    struct Case {
        std::string_view text;
        bool expected;
    };
    static constexpr Case kCases[] = {
        {"Jack", true}, {"Jack.", true}, {"Jack!", true}, {"Hey, Jack.", true}, {"Jack, where are you?", true},
        {"Jacked", true}, {"Jak", true}, {"Jacques", true}, {"Jock", true}, {"Jacket", true},
        {"Jek", true}, {"jek.", true}, {"Jeck", true}, {"Hey Jek", true}, {"Djack", true}, {"Dzhek", true}, {"Dzek", true},
        {"J\xC3\xA4k", true}, {"D\xC5\xBE" "ek", true}, {"\xC4\xB4" "ek", true}, {"Jaek", true}, {"Jake", true}, {"Jake.", true}, {"Hey Jake.", true},
        {"Jarith", false}, {"Jareth.", false}, {"Jerith", false}, {"Gareth", false}, {"Jared", false},
        {"Check", false}, {"Chuck", false}, {"Deck", false}, {"Zack", false}, {"Zach", false}, {"Jerk", false},
        {"Jet", false}, {"Jeff", false}, {"Yeah", false}, {"Yak", false}, {"Yek.", false}, {"Yuck", false}, {"Jag", false}, {"Geek", false},
        {"Neck", false}, {"Back", false}, {"Black", false}, {"Shake", false}, {"Hello", false}, {"Thank you.", false}, {"", false},
        {"I think the jek was over there by the door", false}, {"I think Jake was over there by the door", false}, {"I think Jack was over there by the door", true},
        {"Congressional debate over gun control flares up yet again. We regret to report the murder of", false},
        {"Yeah, this is a way to say Jorif diga.", false}, {"Possessed, okay, shk back.", false}, {"but uh, JARUS!", false},
        {"How about the social zone, Brad? Jorith.", false}, {"Jourissa", false}, {"J-r-est", false}, {"J", false}, {"J-", false},
        {"Lee Soutnich possessed. Lee Soutnich possessed.", false}, {"That's Jeff.", false}, {"Hey you", false}, {"Mmm", false},
        {"James", false}, {"Hura, yeah there's a...", false},
    };
    int failures = 0;
    for (const Case& c : kCases) {
        const bool got = pt::VoiceRecognizer::MatchesKeyword(c.text, 12);
        if (got != c.expected) {
            std::printf("FAIL '%.*s': %s, expected %s\n", static_cast<int>(c.text.size()), c.text.data(), got ? "the word" : "not the word",
                        c.expected ? "the word" : "not the word");
            ++failures;
        }
    }
    std::printf("%s: %d of %zu cases\n", failures ? "FAIL" : "PASS", static_cast<int>(std::size(kCases)) - failures, std::size(kCases));

    const std::filesystem::path source = pt::ResourceDir("voice", PT_VOICE_MODEL_DIR);
    const std::filesystem::path unicode_dir = std::filesystem::temp_directory_path() /
        std::filesystem::path(u8"pt-voice-\u0160\u00e7\u00c7\u011f\u011e\u0131\u0130\u00f6\u00d6\u015f\u015e\u00fc\u00dc-\u65E5\u672C\u8A9E-\u0416\u0438-\u0627\u0639-\u03a9-\u05d0-\u0915-\ud55c-\u0e01-\U0001F600") /
        ("run-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    std::filesystem::create_directories(unicode_dir, ec);
    if (ec) {
        std::fprintf(stderr, "FAIL could not create Unicode voice test directory: %s\n", ec.message().c_str());
        return 1;
    }
    for (const auto& entry : std::filesystem::directory_iterator(source)) {
        const std::string name = entry.path().filename().string();
        const bool model = name == "ggml-base.en-q5_1.bin" || name == "ggml-small.en-q5_1.bin" || name == "ggml-silero-v6.2.0.bin";
        const bool runtime = (name.starts_with("whisper") || name.starts_with("ggml") || name.starts_with("libwhisper") || name.starts_with("libggml")) &&
                             (entry.path().extension() == ".dll" || entry.path().extension() == ".so");
        if (model || runtime) {
            std::filesystem::create_hard_link(entry.path(), unicode_dir / entry.path().filename(), ec);
            if (ec) std::filesystem::copy_file(entry.path(), unicode_dir / entry.path().filename(), ec);
            if (ec) {
                std::fprintf(stderr, "FAIL could not hard-link voice test asset: %s\n", ec.message().c_str());
                return 1;
            }
        }
    }
    pt::VoiceRecognizer recognizer;
    if (!recognizer.Init(unicode_dir, "jack")) {
        std::fprintf(stderr, "FAIL voice Init rejected Unicode model directory\n");
        return 1;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (recognizer.GetState() == pt::VoiceRecognizer::State::Loading && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (recognizer.GetState() != pt::VoiceRecognizer::State::Ready) {
        std::fprintf(stderr, "FAIL voice models did not load from Unicode path (state %d)\n", static_cast<int>(recognizer.GetState()));
        recognizer.Shutdown();
        return 1;
    }
    recognizer.Shutdown();
    std::puts("PASS voice models load from Unicode path");

    pt::VoiceRecognizer missing;
    if (missing.Init(unicode_dir / "missing", "jack") || missing.GetState() != pt::VoiceRecognizer::State::Failed) {
        std::fprintf(stderr, "FAIL missing Unicode voice directory was not rejected\n");
        return 1;
    }
    std::puts("PASS missing Unicode voice path reports an error without terminating");
    return failures ? 1 : 0;
}
