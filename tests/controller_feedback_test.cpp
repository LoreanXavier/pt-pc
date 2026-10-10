#include "engine/platform/controller_feedback.h"
#include "engine/audio/controller_haptics.h"
#include "engine/audio/controller_pcm_capture.h"

#include <cstdio>
#include <array>
#include <algorithm>
#include <cmath>
#include <span>

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };

    const auto enabled = pt::BuildTriggerRumble(0x80, 0xFF, true, true, true);
    check(enabled.left == 0x8080 && enabled.right == 0xFFFF, "advertised trigger rumble scales bytes to SDL motor range");
    check(enabled.Active(), "nonzero trigger levels produce a command");

    const auto unadvertised = pt::BuildTriggerRumble(0xFF, 0xFF, true, true, false);
    check(!unadvertised.Active(), "unadvertised trigger capability produces no command");
    const auto opt_out = pt::BuildTriggerRumble(0xFF, 0xFF, true, false, true);
    check(!opt_out.Active(), "disabled enhanced mode produces no command");
    const auto rumble_off = pt::BuildTriggerRumble(0xFF, 0xFF, false, true, true);
    check(!rumble_off.Active(), "disabled rumble produces no command");
    const auto left_only = pt::BuildTriggerRumble(0x40, 0, true, true, true);
    check(left_only.left == 0x4040 && left_only.right == 0 && left_only.Active(), "left and right channels remain independent");
    const auto original_profile = pt::FeaturesForRumbleProfile(static_cast<int>(pt::ControllerRumbleProfile::Original), true);
    check(!original_profile.trigger_rumble && !original_profile.dualsense_haptics,
          "Original rumble profile keeps both enhanced feedback paths off");
    const auto enhanced_profile = pt::FeaturesForRumbleProfile(static_cast<int>(pt::ControllerRumbleProfile::Enhanced), true);
    check(enhanced_profile.trigger_rumble && enhanced_profile.dualsense_haptics,
          "Enhanced rumble profile enables both supported feedback paths");
    const auto enhanced_muted = pt::FeaturesForRumbleProfile(static_cast<int>(pt::ControllerRumbleProfile::Enhanced), false);
    check(!enhanced_muted.trigger_rumble && !enhanced_muted.dualsense_haptics,
          "master vibration option disables all Enhanced profile feedback");
    const auto unknown_profile = pt::FeaturesForRumbleProfile(99, true);
    check(!unknown_profile.trigger_rumble && !unknown_profile.dualsense_haptics,
          "unknown rumble profiles fail closed to Original behavior");

    pt::audio::ControllerHapticFilter filter;
    std::array<float, 64> input{};
    std::array<float, 64> output{};
    input[0] = input[1] = 1.0f;
    check(filter.Process(input, output, 32), "haptic filter accepts a complete stereo block");
    check(output[0] == output[1] && std::all_of(output.begin(), output.end(), [](float x) { return std::isfinite(x); }),
          "band-limited haptic PCM is finite and preserves stereo symmetry");
    check(!filter.Process(input, std::span<float>(output).first(4), 32), "haptic filter rejects undersized output buffers");

    auto filtered_rms = [&](float frequency) {
        filter.Reset();
        double squares = 0.0;
        constexpr size_t frames = 4800;
        std::array<float, frames * 2> tone{};
        std::array<float, frames * 2> result{};
        for (size_t i = 0; i < frames; ++i) {
            const float value = 0.1f * std::sin(2.0f * 3.14159265358979323846f * frequency * static_cast<float>(i) / 48000.0f);
            tone[i * 2] = tone[i * 2 + 1] = value;
        }
        filter.Process(tone, result, frames);
        for (size_t i = 4800; i < frames * 2; ++i) {
            squares += static_cast<double>(result[i]) * result[i];
        }
        return std::sqrt(squares / static_cast<double>((frames - 2400) * 2));
    };
    const float low_band = filtered_rms(100.0f);
    const float dc_band = filtered_rms(0.0f);
    const float high_band = filtered_rms(1000.0f);
    check(low_band > 0.03f, "100 Hz authored voice content reaches the haptic band");
    check(dc_band < 0.01f, "DC content is removed before reaching the actuators");
    check(high_band < low_band * 0.4f, "content above the 250 Hz haptic band is attenuated");
    filter.Reset();
    std::array<float, 512> full_scale{};
    std::array<float, 512> capped{};
    for (size_t i = 0; i < full_scale.size() / 2; ++i) {
        full_scale[i * 2] = full_scale[i * 2 + 1] = 100.0f * std::sin(2.0f * 3.14159265358979323846f * 100.0f * static_cast<float>(i) / 48000.0f);
    }
    filter.Process(full_scale, capped, full_scale.size() / 2);
    check(std::all_of(capped.begin(), capped.end(), [](float x) { return std::abs(x) <= 0.8f; }) &&
              std::any_of(capped.begin(), capped.end(), [](float x) { return std::abs(x) > 0.79f; }),
          "actuator PCM is soft limited to 80 percent");

    pt::audio::ControllerPcmQueue queue;
    pt::audio::ControllerPcmBlock block{};
    std::array<float, pt::audio::ControllerPcmBlock::kMaxFrames * 2> capture{};
    capture[0] = 0.25f;
    check(queue.TryPush(capture.data(), 1), "bounded PCM queue accepts one complete frame");
    check(queue.TryPop(block) && block.frames == 1 && block.stereo[0] == 0.25f,
          "bounded PCM queue returns frames in stereo without altering authored samples");
    capture[pt::audio::ControllerPcmBlock::kMaxFrames * 2 - 1] = 0.5f;
    check(queue.TryPush(capture.data(), pt::audio::ControllerPcmBlock::kMaxFrames) && queue.TryPop(block) &&
              block.frames == pt::audio::ControllerPcmBlock::kMaxFrames && block.stereo.back() == 0.5f,
          "bounded PCM queue accepts and preserves one maximum-size render block");
    check(!queue.TryPush(capture.data(), pt::audio::ControllerPcmBlock::kMaxFrames + 1),
          "bounded PCM queue rejects blocks above its fixed storage size");
    for (size_t i = 0; i < pt::audio::ControllerPcmQueue::kCapacity; ++i) {
        capture[0] = static_cast<float>(i);
        check(queue.TryPush(capture.data(), 1), "bounded PCM queue accepts available slot");
    }
    check(!queue.TryPush(capture.data(), 1) && queue.DroppedBlocks() == 1, "full PCM queue drops newest block and counts overflow");
    for (size_t i = 0; i < pt::audio::ControllerPcmQueue::kCapacity; ++i) {
        check(queue.TryPop(block) && block.stereo[0] == static_cast<float>(i), "bounded PCM queue preserves FIFO order");
    }
    check(!queue.TryPop(block), "empty PCM queue reports no block");

    pt::audio::ControllerPcmCapture capture_tap;
    const std::array selected_events{0x12345678u, 0x23456789u};
    capture_tap.SetEvents(selected_events);
    capture_tap.BeginBlock(2);
    check(!capture_tap.Accumulate(0x87654321, 0, 0.75f, 0.75f), "unselected authored event is not captured");
    check(capture_tap.Accumulate(0x12345678, 0, 0.25f, -0.25f), "selected authored event reaches the isolated capture tap");
    check(capture_tap.Accumulate(0x23456789, 0, 0.5f, 0.5f), "voices from selected events aggregate in one render block");
    check(capture_tap.SubmitBlock() && capture_tap.TryPop(block) && block.frames == 2 && block.stereo[0] == 0.75f &&
              block.stereo[1] == 0.25f && block.stereo[2] == 0.0f,
          "event capture submits only its mixed voice PCM and leaves unused frames silent");
    capture_tap.BeginBlock(2);
    check(!capture_tap.SubmitBlock() && !capture_tap.TryPop(block), "capture with no selected voice emits no speaker block");
    {
        pt::audio::ControllerPcmCapture haptic_tap;
        const std::array speaker_events{0x11111111u};
        const std::array haptic_events{0x22222222u};
        haptic_tap.SetEvents(speaker_events);
        haptic_tap.SetHapticEvents(haptic_events);
        haptic_tap.BeginBlock(1);
        check(haptic_tap.Accumulate(0x22222222u, 0, 1.0f, 1.0f) && haptic_tap.Accumulate(0x11111111u, 0, 0.5f, 0.5f) &&
                  haptic_tap.SubmitBlock() && haptic_tap.TryPop(block) && block.stereo[0] == 0.5f && block.haptic[0] > 0.5f &&
                  block.haptic[0] < 1.5f,
              "haptic-only events reach the actuator mix, lighter, and never the speaker mix");
    }
    capture_tap.SetEvents(selected_events);
    capture_tap.BeginBlock(2);
    capture_tap.SetEvent(0x34567890u);
    check(capture_tap.Accumulate(selected_events[0], 0, 0.4f, 0.3f) &&
              !capture_tap.Accumulate(0x34567890u, 0, 0.8f, 0.9f),
          "event selection changes take effect at the next render block without mixing IDs mid-block");
    check(capture_tap.SubmitBlock() && capture_tap.TryPop(block) && block.stereo[0] == 0.4f,
          "selected voice capture is stable for the whole render block");
    capture_tap.BeginBlock(2);
    check(capture_tap.Accumulate(0x34567890u, 0, 0.8f, 0.9f),
          "new selected event is captured at the following render block");
    return failures ? 1 : 0;
}
