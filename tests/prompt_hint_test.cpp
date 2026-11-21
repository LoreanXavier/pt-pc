#include "game/ui/pc_settings.h"
#include "game/ui/prompt_hint.h"

#include <cstdio>
#include <string>
#include <string_view>

namespace {

int failed = 0;

void Check(bool ok, const char* what, int language = -1, std::string_view key = {}) {
    if (!ok) {
        std::printf("FAIL: %s (language %d, key %.*s)\n", what, language, static_cast<int>(key.size()), key.data());
        ++failed;
    }
}

}

int main() {
    using pt::game::ExpandPromptHint;
    using pt::game::HasPromptTokens;
    using pt::game::PromptButton;

    const pt::game::PromptLine keyboard = ExpandPromptHint("{columns}: switch column", true);
    const pt::game::PromptLine pad = ExpandPromptHint("{columns}: switch column", false);
    Check(keyboard.pictures.size() == 1 && keyboard.text == "\xEE\x80\x80: switch column", "keyboard shows the Tab key alone");
    Check(pad.pictures.size() == 2 && pad.pictures[0].button == PromptButton::L1 && pad.pictures[1].button == PromptButton::R1, "a pad shows L1 and R1");
    const std::string model = "{kb}{walk}{/kb}{pad}left stick{/pad}: move. {kb}{fast}{/kb}{pad}L3{/pad}: faster. {back}: back.";
    const pt::game::PromptLine model_keyboard = ExpandPromptHint(model, true);
    const pt::game::PromptLine model_pad = ExpandPromptHint(model, false);
    Check(model_keyboard.pictures.size() == 4 + 1 + 1 && model_keyboard.text.find("stick") == std::string::npos && model_keyboard.text.find("L3") == std::string::npos,
          "the keyboard line names no pad control");
    Check(model_pad.pictures.size() == 1 && model_pad.text.find("left stick") != std::string::npos && model_pad.text.find("L3") != std::string::npos,
          "the pad line names no key");
    Check(ExpandPromptHint("Press {1} and {accept}", false).text.find("{1}") != std::string::npos, "an argument token stays for PcNoteText");
    Check(HasPromptTokens("{accept}: open") && HasPromptTokens("{kb}x{/kb}") && !HasPromptTokens("Takes effect at the next start. {1}"), "token detection");

    const std::string_view keys[] = {"pc_nav_columns", "pc_loop_start_hint", "pc_archive_hint", "pc_archive_view_hint", "pc_archive_demo_hint",
                                     "pc_archive_model_hint", "pc_photo_hint", "pc_ending_skip_hold", "pc_note_archive_image", "pc_note_archive_voice",
                                     "pc_note_archive_photo", "pc_note_archive_demo", "pc_note_archive_model", "pc_note_freecam", "pc_note_photo_mode",
                                     "pc_note_photo_speed"};
    const std::string_view devices[] = {"Esc /", "Échap /", "Enter /", "Entrée /", "Invio /", "Tab /", "L1 /", "/ L1", "/ R1", "Cross", "Circle", "Square",
                                        "Triangle", "D-pad", "Options / Start", "Esc /"};
    for (int language = 0; language < 14; ++language) {
        for (const std::string_view key : keys) {
            const std::string text(pt::game::PcText(key, language));
            Check(HasPromptTokens(text), "the hint has a button token", language, key);
            for (const std::string_view device : devices) Check(text.find(device) == std::string::npos, "the hint lists a device", language, key);
            for (const bool keyboard_device : {true, false}) {
                const pt::game::PromptLine line = ExpandPromptHint(text, keyboard_device);
                Check(line.text.find('{') == std::string::npos, "a token was left in the line", language, key);
                size_t codes = 0;
                for (size_t at = line.text.find("\xEE\x80\x80"); at != std::string::npos; at = line.text.find("\xEE\x80\x80", at + 3)) ++codes;
                Check(codes == line.pictures.size(), "a picture per U+E000", language, key);
                Check(!line.pictures.empty() || (!keyboard_device && key == "pc_note_photo_speed"), "the line shows a picture", language, key);
            }
            size_t opens = 0, closes = 0;
            for (size_t at = text.find("{kb}"); at != std::string::npos; at = text.find("{kb}", at + 1)) ++opens;
            for (size_t at = text.find("{/kb}"); at != std::string::npos; at = text.find("{/kb}", at + 1)) ++closes;
            Check(opens == closes, "unbalanced {kb} blocks", language, key);
        }
    }
    const pt::game::PromptLine fire_pad = ExpandPromptHint("{fire} {aim}", false);
    Check(fire_pad.pictures.size() == 2 && fire_pad.pictures[0].button == PromptButton::R2 && fire_pad.pictures[1].button == PromptButton::L2, "a pad fires with R2 and aims with L2");
    Check(ExpandPromptHint("{picks}", true).pictures.size() == 3 && ExpandPromptHint("{picks}", false).pictures.size() == 2, "the radio's offers: three keys, the D-pad");
    const std::string_view night[] = {"pc_note_survival", "pc_survival_pickup_pipe", "pc_survival_hint_shove", "pc_survival_pickup_flare"};
    const std::string_view named[] = {"R1", "L1", "R2", "L2", "L3", "Tab", "Shift", " V ", " G ", " E ", " Q", "1, 2, 3", "(View)"};
    for (int language = 0; language < 14; ++language) {
        for (const std::string_view key : night) {
            const std::string text(pt::game::PcText(key, language));
            Check(HasPromptTokens(text), "the night's line has a button token", language, key);
            for (const std::string_view name : named) Check(text.find(name) == std::string::npos, "the night's line names a key in words", language, key);
            for (const bool keyboard_device : {true, false}) {
                const pt::game::PromptLine line = ExpandPromptHint(text, keyboard_device);
                Check(line.text.find('{') == std::string::npos, "a token was left in the night's line", language, key);
                Check(line.pictures.size() == (key == "pc_note_survival" ? (keyboard_device ? 12u : 11u) : 1u), "the night's line shows its pictures", language, key);
            }
        }
    }
    std::printf("%s prompt hints\n", failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
