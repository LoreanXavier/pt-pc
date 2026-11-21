#include "engine/audio/audio_engine.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <string>

#include "engine/audio/channel_layout.h"
#include "engine/audio/playback_endpoints.h"
#include "engine/audio/spatial_output.h"
#include "engine/core/log.h"
#include "engine/platform/device_start.h"
#include "engine/platform/sdl_diag.h"

namespace pt::audio {

AudioOutput::AudioOutput() = default;

AudioOutput::~AudioOutput() {
    Close();
}

bool AudioOutput::Open(uint32_t sample_rate, bool surround, RenderFunction render, ObjectRenderFunction object_render) {
    Close();
    if (devices::AudioStarting()) {
        LogWarn("audio: the audio devices are still starting (surround requested: {})", surround);
        return false;
    }
    if (!devices::EnsureAudio()) {
        LogError("audio: SDL audio init failed (surround requested: {}): {} (audio drivers built in: {})", surround,
                 SDL_GetError(), SdlCompiledAudioDrivers());
        return false;
    }
    render_ = std::move(render);
    std::vector<uint32_t> sdl_ids;
    const std::vector<PlaybackEndpoint> endpoints = ListPlaybackEndpoints(&sdl_ids);
    const OutputSelection selection = SelectPlaybackEndpoint(endpoints, preferred_device_);
    pick_ = selection.pick;
    LogInfo("audio: playback devices: {}", DescribePlaybackEndpoints(endpoints));
    if (selection.pick == OutputPick::ChosenMissing) {
        LogWarn("audio: the output device '{}' chosen in the settings is not connected; the system default plays until it is back",
                preferred_device_);
    }
    const PlaybackEndpoint* chosen = selection.endpoint ? &endpoints[*selection.endpoint] : nullptr;
    const SDL_AudioDeviceID target = selection.endpoint ? sdl_ids[*selection.endpoint] : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    chosen_physical_ = selection.endpoint ? sdl_ids[*selection.endpoint] : 0;
    default_at_open_ = SystemDefaultPlaybackName();
    if (prefer_spatial_) {
        if (chosen && chosen->id.empty()) {
            LogInfo("audio: spatial sound: '{}' has no single Windows endpoint of that name, so it plays through the ordinary output",
                    chosen->name);
        } else {
            auto spatial = std::make_unique<SpatialOutput>();
            if (spatial->Open(sample_rate, render_, std::move(object_render), chosen ? chosen->id : std::string())) {
                spatial_ = std::move(spatial);
                channels_ = 8;
                device_name_ = spatial_->DeviceName();
                device_endpoint_id_ = spatial_->DeviceId();
                LogOutput("Windows spatial sound");
                return true;
            }
        }
    }
    SDL_AudioSpec preferred_spec{};
    const bool queried = SDL_GetAudioDeviceFormat(target, &preferred_spec, nullptr);
    const int preferred_channels = queried ? preferred_spec.channels : -1;
    if (!queried) {
        LogWarn("audio: could not query preferred playback format (surround requested: {}): {}", surround, SDL_GetError());
    }
    LogInfo("audio: opening playback (surround requested: {}, preferred channels: {})", surround, preferred_channels);

    std::string last_error;
    const auto probes = AudioOutputProbeOrder(preferred_channels, surround);
    for (const uint32_t probe_channels : probes) {
        if (probe_channels == 0) break;

        const SDL_AudioSpec requested_spec{SDL_AUDIO_F32, static_cast<int>(probe_channels), static_cast<int>(sample_rate)};
        const SDL_AudioDeviceID device = SDL_OpenAudioDevice(target, &requested_spec);
        if (!device) {
            last_error = SDL_GetError();
            LogWarn("audio: {}-channel device open failed (surround requested: {}, preferred channels: {}): {}",
                    probe_channels, surround, preferred_channels, last_error);
            continue;
        }

        SDL_AudioSpec opened_spec{};
        if (!SDL_GetAudioDeviceFormat(device, &opened_spec, nullptr)) {
            last_error = SDL_GetError();
            LogWarn("audio: could not query opened {}-channel device (surround requested: {}): {}", probe_channels,
                    surround, last_error);
            SDL_CloseAudioDevice(device);
            continue;
        }

        const uint32_t app_channels = AudioOutputChannelsForDevice(opened_spec.channels, surround);
        if (surround && probe_channels > 2 && app_channels == 2) {
            LogInfo("audio: {}-channel request opened as {} channels; probing next layout", probe_channels,
                    opened_spec.channels);
            SDL_CloseAudioDevice(device);
            continue;
        }

        const SDL_AudioSpec app_spec{SDL_AUDIO_F32, static_cast<int>(app_channels), static_cast<int>(sample_rate)};
        SDL_AudioStream* stream = SDL_CreateAudioStream(&app_spec, nullptr);
        if (!stream) {
            last_error = SDL_GetError();
            LogWarn("audio: could not create {}-channel stream for {}-channel device: {}", app_channels,
                    opened_spec.channels, last_error);
            SDL_CloseAudioDevice(device);
            continue;
        }
        if (!SDL_BindAudioStream(device, stream)) {
            last_error = SDL_GetError();
            LogWarn("audio: could not bind {}-channel stream to {}-channel device: {}", app_channels,
                    opened_spec.channels, last_error);
            SDL_DestroyAudioStream(stream);
            SDL_CloseAudioDevice(device);
            continue;
        }

        SDL_AudioSpec bound_src{};
        SDL_AudioSpec bound_dst{};
        if (!SDL_GetAudioStreamFormat(stream, &bound_src, &bound_dst)) {
            last_error = SDL_GetError();
            LogWarn("audio: could not query bound stream format: {}", last_error);
            SDL_UnbindAudioStream(stream);
            SDL_DestroyAudioStream(stream);
            SDL_CloseAudioDevice(device);
            continue;
        }
        const uint32_t bound_app_channels = AudioOutputChannelsForDevice(bound_dst.channels, surround);
        if (surround && probe_channels > 2 && bound_app_channels == 2) {
            LogInfo("audio: bound {}-channel stream has {} output channels; probing next layout", app_channels,
                    bound_dst.channels);
            SDL_UnbindAudioStream(stream);
            SDL_DestroyAudioStream(stream);
            SDL_CloseAudioDevice(device);
            continue;
        }
        if (bound_src.channels != static_cast<int>(bound_app_channels)) {
            const SDL_AudioSpec adjusted_src{SDL_AUDIO_F32, static_cast<int>(bound_app_channels), static_cast<int>(sample_rate)};
            if (!SDL_SetAudioStreamFormat(stream, &adjusted_src, nullptr)) {
                last_error = SDL_GetError();
                LogWarn("audio: could not match stream input to {} actual output channels: {}", bound_app_channels,
                        last_error);
                SDL_UnbindAudioStream(stream);
                SDL_DestroyAudioStream(stream);
                SDL_CloseAudioDevice(device);
                continue;
            }
        }
        if (!SDL_SetAudioStreamGetCallback(stream, Callback, this)) {
            last_error = SDL_GetError();
            LogWarn("audio: could not install playback callback: {}", last_error);
            SDL_UnbindAudioStream(stream);
            SDL_DestroyAudioStream(stream);
            SDL_CloseAudioDevice(device);
            continue;
        }

        device_id_ = device;
        stream_ = stream;
        channels_ = bound_app_channels;
        if (!SDL_ResumeAudioDevice(device_id_)) {
            last_error = SDL_GetError();
            LogError("audio: could not resume playback device (surround requested: {}, preferred channels: {}, requested channels: {}, actual channels: {}, renderer channels: {}): {}",
                     surround, preferred_channels, probe_channels, bound_dst.channels, channels_, last_error);
            Close();
            LogSdlAudioDevices(false);
            return false;
        }
        LogInfo("audio: playback at {} Hz (surround requested: {}, preferred channels: {}, requested channels: {}, actual channels: {}, renderer channels: {})",
                sample_rate, surround, preferred_channels, probe_channels, bound_dst.channels, channels_);
        LogSdlAudioDevices(false);
        const char* name = SDL_GetAudioDeviceName(device_id_);
        device_name_ = name ? name : "";
        device_endpoint_id_.clear();
        for (const PlaybackEndpoint& endpoint : endpoints) {
            if (endpoint.name == device_name_) device_endpoint_id_ = endpoint.id;
        }
        LogOutput("the ordinary output");
        return true;
    }

    if (chosen) {
        LogWarn("audio: the chosen output device '{}' would not open ({}); the system default plays instead", chosen->name,
                last_error.empty() ? SDL_GetError() : last_error);
        RenderFunction keep = render_;
        const std::string preferred = preferred_device_;
        preferred_device_.clear();
        const bool opened = Open(sample_rate, surround, std::move(keep));
        preferred_device_ = preferred;
        return opened;
    }
    LogError("audio: cannot open playback device (surround requested: {}, preferred channels: {}, last error: {})", surround,
             preferred_channels, last_error.empty() ? SDL_GetError() : last_error);
    render_ = {};
    channels_ = 2;
    LogSdlAudioDevices(false);
    return false;
}

void AudioOutput::LogOutput(const char* path) const {
    LogInfo("audio: main output '{}' (endpoint {}), {}, through {}", device_name_.empty() ? std::string("?") : device_name_,
            device_endpoint_id_.empty() ? std::string("id unknown") : device_endpoint_id_,
            pick_ == OutputPick::Chosen ? std::string("chosen in the settings") : std::string(OutputPickLabel(pick_)) + " device",
            path);
    const ControllerEndpoint kind = ControllerEndpointKind(device_name_);
    if (kind == ControllerEndpoint::None) return;
    if (pick_ == OutputPick::Chosen) {
        LogInfo("audio: the chosen main output is the {} controller's own audio endpoint: the whole game plays on the controller",
                ControllerEndpointLabel(kind));
    } else {
        LogWarn("audio: the system's default playback device is the {} controller's own audio endpoint, so the whole game plays on the "
                "controller; choose headphones or speakers under PC settings > Sound > Output device (the game does not change the "
                "system default)",
                ControllerEndpointLabel(kind));
    }
}

bool AudioOutput::Poll(uint64_t now_ns) {
    if (!IsOpen() || now_ns < next_poll_ns_) return false;
    next_poll_ns_ = now_ns + 1'000'000'000ull;
    const auto listed = [](uint32_t id, const std::string& name) {
        int count = 0;
        SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&count);
        bool found = false;
        for (int i = 0; ids && i < count && !found; ++i) {
            if (id != 0) {
                found = ids[i] == id;
            } else if (const char* n = SDL_GetAudioDeviceName(ids[i])) {
                found = name == n;
            }
        }
        SDL_free(ids);
        return found;
    };
    if (pick_ == OutputPick::ChosenMissing && listed(0, preferred_device_)) {
        LogInfo("audio: the chosen output device '{}' is connected again; the main output moves to it", preferred_device_);
        return true;
    }
    if (pick_ == OutputPick::Chosen && chosen_physical_ != 0 && !listed(chosen_physical_, {})) {
        LogWarn("audio: the chosen output device '{}' went away; the system default plays until it is back", device_name_);
        return true;
    }
    if (spatial_) {
        if (pick_ != OutputPick::Chosen) {
            const std::string now = SystemDefaultPlaybackName();
            if (!now.empty() && now != default_at_open_) {
                LogInfo("audio: the system's default playback device is now '{}'; the spatial output moves to it from '{}'", now,
                        device_name_);
                return true;
            }
        }
        return false;
    }
    const char* name = SDL_GetAudioDeviceName(device_id_);
    if (name && device_name_ != name) {
        const std::string previous = device_name_;
        device_name_ = name;
        device_endpoint_id_.clear();
        for (const PlaybackEndpoint& endpoint : NativePlaybackEndpoints()) {
            if (endpoint.name == device_name_) device_endpoint_id_ = endpoint.id;
        }
        LogInfo("audio: the main output moved from '{}' to '{}' (the system's default playback device changed)", previous, device_name_);
        LogOutput("the ordinary output");
    }
    return false;
}

void AudioOutput::Close() {
    spatial_.reset();
    if (stream_) {
        SDL_UnbindAudioStream(stream_);
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
    if (device_id_ != 0) {
        SDL_CloseAudioDevice(device_id_);
        device_id_ = 0;
    }
    render_ = {};
    channels_ = 2;
    device_name_.clear();
    device_endpoint_id_.clear();
    chosen_physical_ = 0;
    default_at_open_.clear();
    pick_ = OutputPick::SystemDefault;
}

void AudioOutput::Callback(void* user, SDL_AudioStream* stream, int additional, int) {
    auto* output = static_cast<AudioOutput*>(user);
    if (additional <= 0 || !output->render_) {
        return;
    }
    const uint32_t channels = output->channels_;
    const uint32_t frames = static_cast<uint32_t>(additional) / (channels * sizeof(float));
    if (frames == 0) {
        return;
    }
    output->buffer_.resize(static_cast<size_t>(frames) * channels);
    output->render_(output->buffer_.data(), frames, channels);
    SDL_PutAudioStreamData(stream, output->buffer_.data(), static_cast<int>(output->buffer_.size() * sizeof(float)));
}

}
