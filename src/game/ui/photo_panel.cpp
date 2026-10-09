#include "game/ui/photo_panel.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>

#include "engine/platform/input.h"
#include "game/game.h"
#include "game/ui/pc_settings.h"

namespace pt::game {
namespace {

constexpr float kFocusSteps[] = {0.3f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f, 5.0f, 7.0f, 10.0f, 15.0f};
constexpr float kApertures[] = {1.4f, 2.0f, 2.8f, 4.0f, 5.6f, 8.0f, 11.0f, 16.0f};
constexpr const char* kApertureNames[] = {"f/1.4", "f/2", "f/2.8", "f/4", "f/5.6", "f/8", "f/11", "f/16"};
constexpr int kRollMax = 90;
constexpr int kAngleStep = 5;
constexpr int kExposureSteps = PhotoSettings::kExposureZero * 2 + 1;
constexpr float kRepeatDelay = 0.4f;
constexpr float kRepeatStep = 0.1f;
constexpr const char* kCursorSound = "Play_sys_cursor_01";
constexpr const char* kChangeSound = "Play_sys_change_01";

struct RowText {
    const char* label;
    const char* note;
    const char* section;
};

// the rows in their order, the section a row opens
constexpr RowText kRows[] = {
    {"pc_photo_take", "pc_note_photo_take", "pc_photo_section_camera"},
    {"pc_photo_focal_length", "pc_note_photo_focal_length", nullptr},
    {"pc_photo_roll", "pc_note_photo_roll", nullptr},
    {"pc_photo_speed", "pc_note_photo_speed", nullptr},
    {"pc_photo_reset", "pc_note_photo_reset", nullptr},
    {"pc_dof", "pc_note_photo_dof", "pc_photo_section_lens"},
    {"pc_photo_focus", "pc_note_photo_focus", nullptr},
    {"pc_photo_aperture", "pc_note_photo_aperture", nullptr},
    {"pc_photo_exposure", "pc_note_photo_exposure", nullptr},
    {"pc_bloom", "pc_note_bloom", "pc_photo_section_image"},
    {"pc_lens_distortion", "pc_note_lens_distortion", nullptr},
    {"pc_film_grain", "pc_note_photo_grain", nullptr},
    {"pc_photo_grading", "pc_note_photo_grading", nullptr},
    {"pc_photo_aspect", "pc_note_photo_aspect", nullptr},
    {"pc_photo_resolution", "pc_note_photo_resolution", nullptr},
    {"pc_photo_filter", "pc_note_photo_filter", nullptr},
    {"pc_photo_body", "pc_note_photo_body", nullptr},
    {"pc_photo_flashlight", "pc_note_photo_flashlight", nullptr},
};

std::string Number(float value) {
    std::string text = std::format("{:.2f}", value);
    while (text.back() == '0') text.pop_back();
    if (text.back() == '.') text.pop_back();
    return text;
}

std::string Replace(std::string text, std::string_view token, std::string_view value) {
    if (const size_t at = text.find(token); at != std::string::npos) text.replace(at, token.size(), value);
    return text;
}

}

float PhotoSettings::FocusDistance() const {
    return focus > 0 && focus <= static_cast<int>(std::size(kFocusSteps)) ? kFocusSteps[focus - 1] : 0.0f;
}

float PhotoSettings::Aperture() const {
    return aperture > 0 && aperture <= static_cast<int>(std::size(kApertures)) ? kApertures[aperture - 1] : 0.0f;
}

float PhotoSettings::SpeedScale() const {
    return speed == 0 ? 0.35f : speed == 2 ? 2.5f : 1.0f;
}

float LetterboxAspect(int choice) {
    return choice == 1 ? 2.39f : choice == 2 ? 1.85f : 0.0f;
}

float PhotoSettings::AspectRatio(float source_aspect) const {
    return PhotoAspectRatio(static_cast<PhotoAspectPreset>(aspect), source_aspect);
}

void PhotoPanel::Open(const PhotoSettings& settings) {
    settings_ = settings;
    settings_.focal_length_mm = PhotoFocalLengthForStep(PhotoFocalLengthStep(settings_.focal_length_mm));
    settings_.roll = std::clamp(settings_.roll, -kRollMax, kRollMax);
    settings_.aspect = std::clamp(settings_.aspect, 0, static_cast<int>(PhotoAspectPreset::Count) - 1);
    settings_.resolution = static_cast<PhotoResolution>(std::clamp(static_cast<int>(settings_.resolution), 0,
                                                                    static_cast<int>(PhotoResolution::Count) - 1));
    settings_.filter = static_cast<PhotoColorFilter>(std::clamp(static_cast<int>(settings_.filter), 0,
                                                                 static_cast<int>(PhotoColorFilter::Count) - 1));
    cursor_ = 0;
    held_dirs_ = 0;
    repeat_time_ = 0.0f;
    flash_frame_ = 0.0f;
}

int PhotoPanel::Steps(int row) const {
    switch (row) {
        case kFocalLength: return PhotoFocalLengthStepCount();
        case kRoll: return kRollMax * 2 / kAngleStep + 1;
        case kSpeed: return 3;
        case kFocus: return static_cast<int>(std::size(kFocusSteps)) + 1;
        case kAperture: return static_cast<int>(std::size(kApertures)) + 1;
        case kExposure: return kExposureSteps;
        case kAspect: return static_cast<int>(PhotoAspectPreset::Count);
        case kResolution: return static_cast<int>(PhotoResolution::Count);
        case kFilter: return static_cast<int>(PhotoColorFilter::Count);
        case kTakePhoto:
        case kReset: return 1;
        default: return 2;
    }
}

int PhotoPanel::Value(int row) const {
    const PhotoSettings& s = settings_;
    switch (row) {
        case kFocalLength: return PhotoFocalLengthStep(s.focal_length_mm);
        case kRoll: return (s.roll + kRollMax) / kAngleStep;
        case kSpeed: return s.speed;
        case kDof: return s.depth_of_field ? 1 : 0;
        case kFocus: return s.focus;
        case kAperture: return s.aperture;
        case kExposure: return s.exposure;
        case kBloom: return s.bloom ? 1 : 0;
        case kLens: return s.lens ? 1 : 0;
        case kGrain: return s.grain ? 1 : 0;
        case kGrading: return s.grading ? 1 : 0;
        case kAspect: return std::clamp(s.aspect, 0, static_cast<int>(PhotoAspectPreset::Count) - 1);
        case kResolution: return static_cast<int>(s.resolution);
        case kFilter: return static_cast<int>(s.filter);
        case kBody: return s.body ? 1 : 0;
        case kFlashlight: return s.flashlight ? 1 : 0;
        default: return 0;
    }
}

std::string PhotoPanel::ValueText(int row, int language) const {
    const PhotoSettings& s = settings_;
    auto text = [&](std::string_view key) { return std::string(PcText(key, language)); };
    auto on_off = [&](bool on) { return text(on ? "pc_on" : "pc_off"); };
    switch (row) {
        case kTakePhoto: return text("pc_photo_capture");
        case kReset: return text("pc_reset_value");
        case kFocalLength: return std::format("{} mm", static_cast<int>(std::lround(s.focal_length_mm)));
        case kRoll: return s.roll > 0 ? std::format("+{}", s.roll) : std::to_string(s.roll);
        case kSpeed: return text(s.speed == 0 ? "pc_photo_speed_slow" : s.speed == 2 ? "pc_photo_speed_fast" : "pc_photo_speed_normal");
        case kDof: return on_off(s.depth_of_field);
        case kFocus: return s.focus == 0 ? text("pc_photo_auto") : Replace(text("pc_photo_metres"), "{value}", Number(s.FocusDistance()));
        case kAperture: return s.aperture == 0 ? text("pc_photo_auto") : std::string(kApertureNames[s.aperture - 1]);
        case kExposure: {
            const float ev = s.ExposureEv();
            return std::format("{}{:.1f} EV", ev > 0.0f ? "+" : "", ev);
        }
        case kBloom: return on_off(s.bloom);
        case kLens: return on_off(s.lens);
        case kGrain: return on_off(s.grain);
        case kGrading: return on_off(s.grading);
        case kAspect: {
            switch (static_cast<PhotoAspectPreset>(Value(row))) {
                case PhotoAspectPreset::Cinema239: return "2.39:1";
                case PhotoAspectPreset::Cinema185: return "1.85:1";
                case PhotoAspectPreset::FourByThree: return "4:3";
                case PhotoAspectPreset::ThreeByTwo: return "3:2";
                case PhotoAspectPreset::Square: return "1:1";
                default: return text("pc_off");
            }
        }
        case kResolution: return text(s.resolution == PhotoResolution::FourK ? "pc_photo_resolution_4k" : "pc_photo_resolution_native");
        case kFilter: {
            switch (s.filter) {
                case PhotoColorFilter::BlackAndWhite: return text("pc_photo_filter_bw");
                case PhotoColorFilter::Warm: return text("pc_photo_filter_warm");
                case PhotoColorFilter::Cool: return text("pc_photo_filter_cool");
                case PhotoColorFilter::Muted: return text("pc_photo_filter_muted");
                default: return text("pc_off");
            }
        }
        case kBody: return text(s.body ? "pc_photo_shown" : "pc_photo_hidden");
        case kFlashlight: return on_off(s.flashlight);
        default: return {};
    }
}

void PhotoPanel::Change(GameAudio* audio, int delta) {
    const int n = Steps(cursor_);
    if (n <= 1) {
        return;
    }
    // switches and lists wrap as the PC settings' rows do; numbers stop at their ends
    const bool wrap = n == 2 || cursor_ == kSpeed || cursor_ == kAspect || cursor_ == kResolution || cursor_ == kFilter;
    int value = Value(cursor_) + delta;
    value = wrap ? (value % n + n) % n : std::clamp(value, 0, n - 1);
    if (value == Value(cursor_)) {
        return;
    }
    PhotoSettings& s = settings_;
    switch (cursor_) {
        case kFocalLength: s.focal_length_mm = PhotoFocalLengthForStep(value); break;
        case kRoll: s.roll = value * kAngleStep - kRollMax; break;
        case kSpeed: s.speed = value; break;
        case kDof: s.depth_of_field = value == 1; break;
        case kFocus: s.focus = value; break;
        case kAperture: s.aperture = value; break;
        case kExposure: s.exposure = value; break;
        case kBloom: s.bloom = value == 1; break;
        case kLens: s.lens = value == 1; break;
        case kGrain: s.grain = value == 1; break;
        case kGrading: s.grading = value == 1; break;
        case kAspect: s.aspect = value; break;
        case kResolution: s.resolution = static_cast<PhotoResolution>(value); break;
        case kFilter: s.filter = static_cast<PhotoColorFilter>(value); break;
        case kBody: s.body = value == 1; break;
        case kFlashlight: s.flashlight = value == 1; break;
        default: break;
    }
    if (audio) {
        audio->PostEvent(kCursorSound, nullptr);
    }
}

PhotoPanel::Action PhotoPanel::Update(Game& game, const Input& input, float dt) {
    return Update(game.Audio(), input, dt);
}

PhotoPanel::Action PhotoPanel::Update(GameAudio* audio, const Input& input, float dt) {
    flash_frame_ += dt * 60.0f;
    const uint32_t pressed = input.held_dirs & ~held_dirs_;
    uint32_t act = pressed;
    if (pressed) {
        repeat_time_ = -kRepeatDelay;
    } else if (input.held_dirs) {
        repeat_time_ += dt;
        if (repeat_time_ >= kRepeatStep) {
            repeat_time_ -= kRepeatStep;
            act = input.held_dirs;
        }
    }
    held_dirs_ = input.held_dirs;
    if (act & (kRawUp | kRawDown)) {
        cursor_ = ((cursor_ + ((act & kRawUp) ? -1 : 1)) % kRowCount + kRowCount) % kRowCount;
        flash_frame_ = 0.0f;
        if (audio) {
            audio->PostEvent(kCursorSound, nullptr);
        }
    } else if (act & kRawLeft) {
        Change(audio, -1);
    } else if (act & kRawRight) {
        Change(audio, 1);
    }
    if (input.accept && (cursor_ == kTakePhoto || cursor_ == kReset)) {
        flash_frame_ = 0.0f;
        if (audio) {
            audio->PostEvent(kChangeSound, nullptr);
        }
        return cursor_ == kTakePhoto ? Action::TakePhoto : Action::ResetCamera;
    }
    return Action::None;
}

PhotoPanelView PhotoPanel::View(int language, const std::string& status, float source_aspect) const {
    PhotoPanelView view;
    view.language = language;
    view.flash_frame = flash_frame_;
    view.crop = PhotoCropForAspect(source_aspect, settings_.AspectRatio(source_aspect));
    for (int row = 0; row < kRowCount; ++row) {
        if (kRows[row].section) {
            PhotoPanelRow header;
            header.label = PcText(kRows[row].section, language);
            header.section = true;
            view.rows.push_back(std::move(header));
        }
        PhotoPanelRow line;
        line.label = PcText(kRows[row].label, language);
        line.value = ValueText(row, language);
        const int n = Steps(row);
        const int value = Value(row);
        const bool wrap = n == 2 || row == kSpeed || row == kAspect || row == kResolution || row == kFilter;
        line.adjustable = n > 1;
        line.can_decrease = wrap || value > 0;
        line.can_increase = wrap || value + 1 < n;
        line.selected = row == cursor_;
        view.rows.push_back(std::move(line));
    }
    view.note = status.empty() ? std::string(PcText(kRows[cursor_].note, language)) : status;
    view.hint = PcText("pc_photo_hint", language);
    return view;
}

}
