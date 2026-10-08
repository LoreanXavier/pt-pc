#ifdef NDEBUG
#undef NDEBUG
#endif
#include "game/ui/photo_preset.h"

#include <cassert>
#include <cmath>

using namespace pt::game;

namespace {
bool Near(float a, float b, float epsilon = 1.0e-4f) { return std::abs(a - b) <= epsilon; }
}

int main() {
    assert(PhotoFocalLengthStepCount() == 12);
    assert(Near(PhotoFocalLengthForStep(-1), 8.0f));
    assert(Near(PhotoFocalLengthForStep(99), 70.0f));
    assert(PhotoFocalLengthStep(34.0f) == 9);
    assert(PhotoFocalLengthStep(60.0f) == 10);

    const float wide_fov = PhotoFovYDegrees(8.0f);
    const float normal_fov = PhotoFovYDegrees(35.0f);
    assert(wide_fov > normal_fov);
    assert(Near(normal_fov, 21.8f, 0.2f));
    assert(PhotoFovYDegrees(0.0f) == 0.0f);
    assert(PhotoFovYDegrees(35.0f, 0.0f) == 0.0f);
    assert(Near(PhotoFocalLengthFromFovYDegrees(60.0f), 13.5f / (2.0f * std::tan(3.14159265f / 6.0f))));
    assert(PhotoFocalLengthFromFovYDegrees(0.0f) == 0.0f);

    const PhotoCropRect off = PhotoCropForAspect(16.0f / 9.0f, PhotoAspectRatio(PhotoAspectPreset::Off, 16.0f / 9.0f));
    assert(Near(off.x, 0.0f) && Near(off.y, 0.0f) && Near(off.width, 1.0f) && Near(off.height, 1.0f));
    const PhotoCropRect four_by_three = PhotoCropForAspect(16.0f / 9.0f, PhotoAspectRatio(PhotoAspectPreset::FourByThree, 16.0f / 9.0f));
    assert(Near(four_by_three.width, 0.75f) && Near(four_by_three.height, 1.0f));
    assert(Near(four_by_three.x, 0.125f) && Near(four_by_three.y, 0.0f));
    const PhotoCropRect three_by_two = PhotoCropForAspect(16.0f / 9.0f, PhotoAspectRatio(PhotoAspectPreset::ThreeByTwo, 16.0f / 9.0f));
    assert(Near(three_by_two.width, 0.84375f));
    const PhotoCropRect cinema = PhotoCropForAspect(16.0f / 9.0f, PhotoAspectRatio(PhotoAspectPreset::Cinema239, 16.0f / 9.0f));
    assert(Near(cinema.height, (16.0f / 9.0f) / 2.39f));
    const PhotoCropRect cinema185 = PhotoCropForAspect(16.0f / 9.0f, PhotoAspectRatio(PhotoAspectPreset::Cinema185, 16.0f / 9.0f));
    assert(Near(cinema185.height, (16.0f / 9.0f) / 1.85f));
    const float cinema_fov = PhotoCropFovYDegrees(60.0f, cinema);
    assert(cinema_fov < 60.0f);
    assert(Near(std::tan(cinema_fov * 3.14159265f / 360.0f) * 2.39f,
                std::tan(60.0f * 3.14159265f / 360.0f) * (16.0f / 9.0f)));
    assert(Near(PhotoCropFovYDegrees(60.0f, four_by_three), 60.0f));
    const PhotoCropRect square = PhotoCropForAspect(16.0f / 9.0f, PhotoAspectRatio(PhotoAspectPreset::Square, 16.0f / 9.0f));
    assert(Near(square.width, 9.0f / 16.0f));
    const PhotoCropRect portrait = PhotoCropForAspect(16.0f / 9.0f, 9.0f / 16.0f);
    assert(Near(portrait.width, 81.0f / 256.0f) && Near(portrait.x, 175.0f / 512.0f));
    assert(Near(PhotoAspectRatio(PhotoAspectPreset::Count, 16.0f / 9.0f), 16.0f / 9.0f));
    assert(PhotoCropForAspect(0.0f, 1.0f).width == 1.0f);

    const PhotoExtent native = PhotoCaptureExtent(1600, 900, PhotoAspectPreset::Off, PhotoResolution::Native);
    assert(native.width == 1600 && native.height == 900);
    const PhotoExtent native_crop = PhotoCaptureExtent(1600, 900, PhotoAspectPreset::FourByThree, PhotoResolution::Native);
    assert(native_crop.width == 1200 && native_crop.height == 900);
    const PhotoExtent highres = PhotoCaptureExtent(1600, 900, PhotoAspectPreset::Off, PhotoResolution::FourK);
    assert(highres.width == 3840 && highres.height == 2160);
    const PhotoExtent highres_cinema = PhotoCaptureExtent(1600, 900, PhotoAspectPreset::Cinema239, PhotoResolution::FourK);
    assert(highres_cinema.width == 3840 && highres_cinema.height == 1607);
    const PhotoExtent highres_square = PhotoCaptureExtent(1600, 900, PhotoAspectPreset::Square, PhotoResolution::FourK);
    assert(highres_square.width == 3840 && highres_square.height == 3840);
    const PhotoExtent highres_portrait = PhotoCaptureExtent(900, 1600, PhotoAspectPreset::Off, PhotoResolution::FourK);
    assert(highres_portrait.width == 2160 && highres_portrait.height == 3840);
    const PhotoExtent invalid = PhotoCaptureExtent(0, 0, PhotoAspectPreset::Off, PhotoResolution::FourK);
    assert(invalid.width == 0 && invalid.height == 0);
}
