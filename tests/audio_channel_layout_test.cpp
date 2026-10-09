#include "engine/audio/channel_layout.h"
#include "engine/audio/audio_engine.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

int main() {
    using namespace pt::audio;
    if (AudioOutputProbeOrder(2, true) != std::array<uint32_t, 3>{8, 6, 2} ||
        AudioOutputProbeOrder(6, true) != std::array<uint32_t, 3>{6, 8, 2} ||
        AudioOutputProbeOrder(8, true) != std::array<uint32_t, 3>{8, 6, 2} ||
        AudioOutputProbeOrder(8, false) != std::array<uint32_t, 3>{2, 0, 0} ||
        AudioOutputChannelsForDevice(8, true) != 8 || AudioOutputChannelsForDevice(7, true) != 6 ||
        AudioOutputChannelsForDevice(6, true) != 6 || AudioOutputChannelsForDevice(2, true) != 2 ||
        AudioOutputChannelsForDevice(8, false) != 2) {
        std::puts("audio output negotiation selection failed");
        return 1;
    }

    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::puts("dummy audio driver did not initialize");
        return 1;
    }
    AudioOutput output;
    SDL_AudioDeviceID forced_device = 0;
    const auto fail_audio_output = [&](const char* message) {
        std::puts(message);
        output.Close();
        if (forced_device != 0) SDL_CloseAudioDevice(forced_device);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return 1;
    };
    const auto render_silence = [](float* samples, uint32_t frames, uint32_t channels) {
        std::fill_n(samples, static_cast<size_t>(frames) * channels, 0.0f);
    };
    const SDL_AudioSpec force_8_channel_device{SDL_AUDIO_F32, 8, 48000};
    forced_device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &force_8_channel_device);
    SDL_AudioSpec forced_format{};
    if (!forced_device || !SDL_GetAudioDeviceFormat(forced_device, &forced_format, nullptr) || forced_format.channels != 8) {
        return fail_audio_output("dummy audio driver did not open an 8-channel device");
    }
    if (!output.Open(48000, false, render_silence) || output.Channels() != 2) {
        return fail_audio_output("stereo mode did not keep a 2-channel renderer on an 8-channel device");
    }
    output.Close();
    SDL_CloseAudioDevice(forced_device);
    forced_device = 0;
    if (!output.Open(48000, true, render_silence) || output.Channels() != 8) {
        return fail_audio_output("surround mode did not use the actual 8-channel device format");
    }
    output.Close();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);

    if (ChooseOutputLayout(8, false) != OutputLayout::Stereo || ChooseOutputLayout(8, true) != OutputLayout::Surround71 ||
        ChooseOutputLayout(6, true) != OutputLayout::Surround51 || ChooseOutputLayout(2, true) != OutputLayout::Stereo ||
        ChooseOutputLayout(1, true) != OutputLayout::Stereo) {
        std::puts("surround layout selection failed");
        return 1;
    }

    std::array<std::array<float, 2>, 8> channels{};
    std::array<const float*, 8> source{};
    for (size_t channel = 0; channel < channels.size(); ++channel) {
        channels[channel] = {static_cast<float>(channel + 10), static_cast<float>(channel + 20)};
        source[channel] = channels[channel].data();
    }
    float out51[12]{};
    DownmixWwiseToSdl51(source, out51, 2);
    constexpr float kRearFold = 0.70710678f;
    const float expected51[] = {10, 11, 12, 0, kRearFold * (13 + 15), kRearFold * (14 + 16),
                                20, 21, 22, 0, kRearFold * (23 + 25), kRearFold * (24 + 26)};
    for (size_t i = 0; i < std::size(expected51); ++i) {
        if (std::abs(out51[i] - expected51[i]) > 1e-6f) {
            std::puts("5.1 speaker order failed");
            return 1;
        }
    }
    float out71[16]{};
    ReorderSpeakers(source, out71, 2, kWwiseToSdl71);
    constexpr float expected71[] = {10, 11, 12, 17, 13, 14, 15, 16, 20, 21, 22, 27, 23, 24, 25, 26};
    for (size_t i = 0; i < std::size(expected71); ++i) {
        if (out71[i] != expected71[i]) {
            std::puts("7.1 speaker order failed");
            return 1;
        }
    }
    return 0;
}
