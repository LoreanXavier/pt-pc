#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pt::game {

class Game;

float LetterboxAspect(int choice);

struct PhotoSettings {
    static constexpr int kExposureZero = 6;

    int fov = 60;
    int roll = 0;
    int speed = 1;
    bool depth_of_field = true;
    int focus = 0;
    int aperture = 0;
    int exposure = kExposureZero;
    bool lens = true;
    bool bloom = true;
    bool grain = true;
    bool grading = true;
    int letterbox = 0;
    bool body = true;
    bool flashlight = true;

    float FocusDistance() const;
    float Aperture() const;
    float ExposureEv() const { return static_cast<float>(exposure - kExposureZero) * 0.5f; }
    float SpeedScale() const;
    float LetterboxAspect() const;
};

struct PhotoPanelRow {
    std::string label;
    std::string value;
    bool section = false;
    bool adjustable = false;
    bool can_decrease = false;
    bool can_increase = false;
    bool selected = false;
};

struct PhotoPanelView {
    bool panel = false;
    float letterbox = 0.0f;
    int language = 0;
    std::vector<PhotoPanelRow> rows;
    std::string note;
    std::string hint;
    float flash_frame = 1000.0f;
};

class PhotoPanel {
public:
    enum class Action { None, TakePhoto, ResetCamera };
    struct Input {
        uint32_t held_dirs = 0;
        bool accept = false;
    };

    void Open(const PhotoSettings& settings);
    Action Update(Game& game, const Input& input, float dt);
    PhotoSettings& Settings() { return settings_; }
    const PhotoSettings& Settings() const { return settings_; }
    PhotoPanelView View(int language, const std::string& status) const;

private:
    enum Row { kTakePhoto, kFov, kRoll, kSpeed, kReset, kDof, kFocus, kAperture, kExposure, kBloom, kLens, kGrain, kGrading,
               kLetterbox, kBody, kFlashlight, kRowCount };

    void Change(Game& game, int delta);
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
