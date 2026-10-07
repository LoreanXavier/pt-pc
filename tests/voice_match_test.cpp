#include <cstdio>
#include <string_view>

#include "engine/voice/voice_recognizer.h"

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
    return failures ? 1 : 0;
}
