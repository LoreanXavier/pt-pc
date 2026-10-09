#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "game/ui/photo_preset.h"

namespace pt::game {

class Game;
class GameAudio;

// the letterbox choices of the photo mode and of the PC settings page (pt.ini [display] letterbox): 0 off, 1 2.39:1, 2 1.85:1;
// the frame's aspect ratio between the black bars, 0 for none
float LetterboxAspect(int choice);

// What the photo mode changes while it runs (main.cpp applies it to the free camera, the renderer's toggles and the frame's
// lighting, and puts everything back when the photo mode ends)
struct PhotoSettings {
    static constexpr int kExposureZero = 6;

    float focal_length_mm = 12.0f;
    int roll = 0;  // degrees, -90 to 90
    int speed = 1;  // 0 slow, 1 normal, 2 fast
    bool depth_of_field = true;
    int focus = 0;  // 0 auto, else a step of FocusDistance
    int aperture = 0;  // 0 auto, else a step of Aperture
    int exposure = kExposureZero;  // half EV steps around kExposureZero
    bool lens = true;
    bool bloom = true;
    bool grain = true;
    bool grading = true;
    int aspect = static_cast<int>(PhotoAspectPreset::Off);
    PhotoResolution resolution = PhotoResolution::Native;
    PhotoColorFilter filter = PhotoColorFilter::Off;
    bool body = true;
    bool flashlight = true;

    // 0 for auto
    float FocusDistance() const;
    // the lens' f-number, 0 for auto
    float Aperture() const;
    float ExposureEv() const { return static_cast<float>(exposure - kExposureZero) * 0.5f; }
    float SpeedScale() const;
    float AspectRatio(float source_aspect) const;
};

// One line of the panel: a section header (the label over the option screen's header line) or a row of a label and a value,
// each a PcText key or the text itself
struct PhotoPanelRow {
    std::string label;
    std::string value;
    bool section = false;
    bool adjustable = false;
    bool can_decrease = false;
    bool can_increase = false;
    bool selected = false;
};

// What GameUi::RecordPhotoMode draws over the frame: the letterbox bars, and the panel unless it is hidden (it never is in a
// photo)
struct PhotoPanelView {
    bool panel = false;
    float letterbox = 0.0f;
    PhotoCropRect crop;
    int language = 0;
    std::vector<PhotoPanelRow> rows;
    std::string note;
    std::string hint;
    float flash_frame = 1000.0f;
};

// The photo mode's side panel, laid out and navigated as the PC settings page: up and down choose a row, left and right change
// its value, accept runs an action row (take the photo, reset the camera)
class PhotoPanel {
public:
    enum class Action { None, TakePhoto, ResetCamera };
    struct Input {
        uint32_t held_dirs = 0;  // kRawUp, kRawDown, kRawLeft, kRawRight
        bool accept = false;
    };

    void Open(const PhotoSettings& settings);
    Action Update(Game& game, const Input& input, float dt);
    Action Update(GameAudio* audio, const Input& input, float dt);
    PhotoSettings& Settings() { return settings_; }
    const PhotoSettings& Settings() const { return settings_; }
    // the panel's lines; status (a saved photo's message, already in the shown language) takes the note's place while set
    PhotoPanelView View(int language, const std::string& status, float source_aspect = 16.0f / 9.0f) const;

private:
    enum Row { kTakePhoto, kFocalLength, kRoll, kSpeed, kReset, kDof, kFocus, kAperture, kExposure, kBloom, kLens, kGrain,
               kGrading, kAspect, kResolution, kFilter, kBody, kFlashlight, kRowCount };

    void Change(GameAudio* audio, int delta);
    // the value's step, the number of steps (1 for an action row) and the shown value
    int Value(int row) const;
    int Steps(int row) const;
    std::string ValueText(int row, int language) const;

    PhotoSettings settings_;
    int cursor_ = 0;
    uint32_t held_dirs_ = 0;
    float repeat_time_ = 0.0f;
    float flash_frame_ = 0.0f;
};

}
