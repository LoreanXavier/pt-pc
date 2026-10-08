#include "engine/platform/controller_speaker.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstddef>
#include <format>
#include <vector>

#include "engine/core/log.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <SetupAPI.h>
#include <audioclient.h>
#include <devpkey.h>
#include <hidclass.h>
#include <propvarutil.h>
#include <wrl/client.h>
#endif

namespace pt {

namespace {

constexpr uint16_t kSonyVendor = 0x054C;
constexpr uint16_t kDualShock4 = 0x05C4;
constexpr uint16_t kDualShock4Slim = 0x09CC;
constexpr uint16_t kDualSense = 0x0CE6;
constexpr uint16_t kDualSenseEdge = 0x0DF2;
constexpr int kPcmRate = 48000;
#if defined(_WIN32)
constexpr GUID kHidDeviceInterfaceGuid{0x4D1E55B2L, 0xF16F, 0x11CF, {0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};
constexpr DEVPROPKEY kDeviceContainerId{{0x8C7ED206L, 0x3F8A, 0x4827, {0xB3, 0xAB, 0xAE, 0x9E, 0x1F, 0xAE, 0xFC, 0x6C}}, 2};
#endif

void SetReason(std::string* reason, std::string text) {
    if (reason) {
        *reason = std::move(text);
    }
}

#if defined(_WIN32)

using Microsoft::WRL::ComPtr;

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), count) != count) {
        return {};
    }
    return out;
}

std::string WideToUtf8(const wchar_t* text) {
    if (!text || !*text) {
        return {};
    }
    const int length = static_cast<int>(std::wcslen(text));
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, length, nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, length, out.data(), count, nullptr, nullptr) != count) {
        return {};
    }
    return out;
}

bool HidContainerId(std::string_view path, GUID* out) {
    const std::wstring wanted = Utf8ToWide(path);
    if (wanted.empty()) {
        return false;
    }
    HDEVINFO devices = SetupDiGetClassDevsW(&kHidDeviceInterfaceGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool found = false;
    for (DWORD index = 0; !found; ++index) {
        SP_DEVICE_INTERFACE_DATA interface_data{};
        interface_data.cbSize = sizeof(interface_data);
        if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &kHidDeviceInterfaceGuid, index, &interface_data)) {
            break;
        }
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &interface_data, nullptr, 0, &required, nullptr);
        if (required < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
            continue;
        }
        std::vector<std::byte> detail_buffer(required);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detail_buffer.data());
        detail->cbSize = sizeof(*detail);
        SP_DEVINFO_DATA device_info{};
        device_info.cbSize = sizeof(device_info);
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &interface_data, detail, required, nullptr, &device_info)) {
            continue;
        }
        if (CompareStringOrdinal(detail->DevicePath, -1, wanted.c_str(), -1, TRUE) != CSTR_EQUAL) {
            continue;
        }
        DEVPROPTYPE type = 0;
        GUID container{};
        if (SetupDiGetDevicePropertyW(devices, &device_info, &kDeviceContainerId, &type,
                                      reinterpret_cast<PBYTE>(&container), sizeof(container), nullptr, 0) &&
            type == DEVPROP_TYPE_GUID) {
            *out = container;
            found = true;
        }
    }
    SetupDiDestroyDeviceInfoList(devices);
    return found;
}

struct EndpointCandidate {
    std::string name;
    int channels = 0;
};

bool QueryEndpointChannels(IMMDevice* endpoint, int expected_channels) {
    ComPtr<IAudioClient> client;
    if (FAILED(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf())))) {
        return false;
    }
    WAVEFORMATEX* format = nullptr;
    if (FAILED(client->GetMixFormat(&format)) || !format) {
        return false;
    }
    bool matches = format->nChannels == expected_channels && format->nSamplesPerSec == kPcmRate;
    if (matches && expected_channels == 4) {
        if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
            const auto* extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
            constexpr DWORD kQuadMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
            matches = extended->dwChannelMask == kQuadMask;
        } else {
            matches = false;
        }
    }
    CoTaskMemFree(format);
    return matches;
}

bool ReadEndpointContainer(IMMDevice* endpoint, GUID* out) {
    ComPtr<IPropertyStore> properties;
    if (FAILED(endpoint->OpenPropertyStore(STGM_READ, properties.GetAddressOf()))) {
        return false;
    }
    PROPVARIANT value;
    PropVariantInit(&value);
    const HRESULT result = properties->GetValue(PKEY_Device_ContainerId, &value);
    const bool ok = SUCCEEDED(result) && value.vt == VT_CLSID && value.puuid;
    if (ok) {
        *out = *value.puuid;
    }
    PropVariantClear(&value);
    return ok;
}

std::vector<EndpointCandidate> AudioEndpointsForContainer(const GUID& container, int expected_channels) {
    std::vector<EndpointCandidate> out;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(enumerator.GetAddressOf())))) {
        return out;
    }
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.GetAddressOf()))) {
        return out;
    }
    UINT count = 0;
    if (FAILED(devices->GetCount(&count))) {
        return out;
    }
    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> endpoint;
        if (FAILED(devices->Item(index, endpoint.GetAddressOf()))) {
            continue;
        }
        GUID endpoint_container{};
        if (!ReadEndpointContainer(endpoint.Get(), &endpoint_container) || !IsEqualGUID(endpoint_container, container) ||
            !QueryEndpointChannels(endpoint.Get(), expected_channels)) {
            continue;
        }
        ComPtr<IPropertyStore> properties;
        if (FAILED(endpoint->OpenPropertyStore(STGM_READ, properties.GetAddressOf()))) {
            continue;
        }
        PROPVARIANT name;
        PropVariantInit(&name);
        if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR && name.pwszVal) {
            std::string utf8 = WideToUtf8(name.pwszVal);
            if (!utf8.empty()) {
                out.push_back({std::move(utf8), expected_channels});
            }
        }
        PropVariantClear(&name);
    }
    return out;
}

std::optional<uint32_t> ResolveSdlDevice(const GUID& container, int expected_channels) {
    const std::vector<EndpointCandidate> native_endpoints = AudioEndpointsForContainer(container, expected_channels);
    std::vector<ControllerSpeakerEndpointName> endpoint_names;
    endpoint_names.reserve(native_endpoints.size());
    for (const EndpointCandidate& endpoint : native_endpoints) {
        endpoint_names.push_back({endpoint.name, endpoint.channels});
    }

    int count = 0;
    SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&count);
    std::vector<std::string> names;
    std::vector<ControllerAudioEndpointName> sdl_devices;
    names.reserve(count > 0 ? static_cast<size_t>(count) : 0);
    sdl_devices.reserve(count > 0 ? static_cast<size_t>(count) : 0);
    for (int index = 0; ids && index < count; ++index) {
        const char* name = SDL_GetAudioDeviceName(ids[index]);
        SDL_AudioSpec spec{};
        if (!name || !SDL_GetAudioDeviceFormat(ids[index], &spec, nullptr)) {
            continue;
        }
        names.emplace_back(name);
        sdl_devices.push_back({ids[index], names.back(), spec.channels});
    }
    SDL_free(ids);
    return MatchUniqueControllerAudioEndpoint(endpoint_names, sdl_devices, expected_channels);
}

#endif

}

ControllerSpeakerOutput::~ControllerSpeakerOutput() {
    Close();
}

bool ControllerSpeakerOutput::IsOpen() const {
    if (!stream_) return false;
    const SDL_AudioDeviceID device = SDL_GetAudioStreamDevice(stream_);
    return device != 0 && SDL_GetAudioDeviceName(device) != nullptr;
}

bool ControllerSpeakerOutput::OpenForGamepad(SDL_Gamepad* gamepad, std::string* reason) {
    Close();
    if (!gamepad) {
        SetReason(reason, "no gamepad selected");
        return false;
    }
    if (SDL_GetGamepadConnectionState(gamepad) != SDL_JOYSTICK_CONNECTION_WIRED) {
        SetReason(reason, "controller speaker routing requires a wired gamepad");
        return false;
    }
    const uint16_t vendor = SDL_GetGamepadVendor(gamepad);
    const uint16_t product = SDL_GetGamepadProduct(gamepad);
    const int expected_channels = vendor == kSonyVendor && (product == kDualShock4 || product == kDualShock4Slim)
                                     ? 1
                                     : vendor == kSonyVendor && (product == kDualSense || product == kDualSenseEdge) ? 4 : 0;
    if (expected_channels == 0) {
        SetReason(reason, "controller model has no verified speaker audio layout");
        return false;
    }
#if !defined(_WIN32)
    (void)expected_channels;
    SetReason(reason, "exact HID-to-audio container matching is currently implemented on Windows only");
    return false;
#else
    const char* path = SDL_GetGamepadPath(gamepad);
    GUID container{};
    if (!path || !HidContainerId(path, &container)) {
        SetReason(reason, "could not match the SDL HID path to a Windows PnP container ID");
        return false;
    }

    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            SetReason(reason, std::format("SDL audio initialization failed: {}", SDL_GetError()));
            return false;
        }
        owns_audio_subsystem_ = true;
    }

    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize_com = SUCCEEDED(com_result);
    if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) {
        Close();
        SetReason(reason, std::format("COM initialization failed ({:08X})", static_cast<unsigned>(com_result)));
        return false;
    }
    const std::optional<uint32_t> id = ResolveSdlDevice(container, expected_channels);
    if (uninitialize_com) {
        CoUninitialize();
    }
    if (!id) {
        Close();
        SetReason(reason, "no unique active SDL playback endpoint matched the controller container and channel layout");
        return false;
    }

    SDL_AudioSpec spec{SDL_AUDIO_F32, expected_channels, kPcmRate};
    stream_ = SDL_OpenAudioDeviceStream(*id, &spec, nullptr, nullptr);
    if (!stream_) {
        Close();
        SetReason(reason, std::format("SDL could not open the matched controller audio endpoint: {}", SDL_GetError()));
        return false;
    }
    SDL_AudioSpec source{};
    SDL_AudioSpec output{};
    if (!SDL_GetAudioStreamFormat(stream_, &source, &output) || source.channels != expected_channels || output.channels != expected_channels ||
        output.freq != kPcmRate) {
        Close();
        SetReason(reason, "opened controller endpoint changed format; expected the verified PCM channel layout at 48 kHz");
        return false;
    }
    if (expected_channels == 4) {
        int map_count = 0;
        int* map = SDL_GetAudioStreamOutputChannelMap(stream_, &map_count);
        const bool identity = !map || (map_count == 4 && map[0] == 0 && map[1] == 1 && map[2] == 2 && map[3] == 3);
        SDL_free(map);
        if (!identity) {
            Close();
            SetReason(reason, "opened DualSense endpoint has a nonstandard output channel map");
            return false;
        }
    }
    const char* name = SDL_GetAudioDeviceName(*id);
    device_name_ = name ? name : "controller audio endpoint";
    route_ = expected_channels == 4 ? ControllerPcmRoute::DualSenseQuad : ControllerPcmRoute::DualShock4Mono;
    SDL_ResumeAudioStreamDevice(stream_);
    SetReason(reason, {});
    LogInfo("input: controller PCM endpoint opened for '{}' ({} channels)", SDL_GetGamepadName(gamepad), expected_channels);
    return true;
#endif
}

void ControllerSpeakerOutput::Close() {
    if (stream_) {
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
    route_ = ControllerPcmRoute::None;
    device_name_.clear();
    haptic_filter_.Reset();
    if (owns_audio_subsystem_) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        owns_audio_subsystem_ = false;
    }
}

void ControllerSpeakerOutput::ClearPending() {
    if (stream_) SDL_ClearAudioStream(stream_);
    haptic_filter_.Reset();
}

bool ControllerSpeakerOutput::WriteMono(const float* speaker_mono, uint32_t frames) {
    if (!stream_ || route_ != ControllerPcmRoute::DualShock4Mono || !speaker_mono || frames == 0 || frames > INT_MAX / sizeof(float)) {
        return false;
    }
    if (SDL_GetAudioStreamQueued(stream_) > 48000 * static_cast<int>(sizeof(float)) / 10) SDL_ClearAudioStream(stream_);
    return SDL_PutAudioStreamData(stream_, speaker_mono, static_cast<int>(frames * sizeof(float)));
}

bool ControllerSpeakerOutput::WriteDualSense(const float* speaker_stereo, const float* actuator_stereo, uint32_t frames) {
    if (!stream_ || route_ != ControllerPcmRoute::DualSenseQuad || !speaker_stereo || frames == 0 ||
        frames > audio::ControllerPcmBlock::kMaxFrames) {
        return false;
    }
    std::array<float, audio::ControllerPcmBlock::kMaxFrames * 4> quad{};
    const std::span<const float> speaker(speaker_stereo, static_cast<size_t>(frames) * 2);
    const std::span<const float> actuator = actuator_stereo
                                                ? std::span<const float>(actuator_stereo, static_cast<size_t>(frames) * 2)
                                                : std::span<const float>();
    const std::span<float> output(quad.data(), static_cast<size_t>(frames) * 4);
    if (!InterleaveDualSensePcm(speaker, actuator, output, frames)) {
        return false;
    }
    if (SDL_GetAudioStreamQueued(stream_) > 48000 * 4 * static_cast<int>(sizeof(float)) / 10) SDL_ClearAudioStream(stream_);
    return SDL_PutAudioStreamData(stream_, quad.data(), static_cast<int>(output.size() * sizeof(float)));
}

bool ControllerSpeakerOutput::WriteCapturedBlock(const audio::ControllerPcmBlock& block, bool speaker_enabled,
                                                  bool dualsense_haptics_enabled, float speaker_gain, float haptics_gain) {
    if (block.frames == 0 || block.frames > audio::ControllerPcmBlock::kMaxFrames) {
        return false;
    }
    const size_t stereo_samples = static_cast<size_t>(block.frames) * 2;
    const std::span<const float> authored(block.stereo.data(), stereo_samples);
    const std::span<float> speaker_buffer(speaker_scratch_.data(), stereo_samples);
    const std::span<float> haptics_source(haptics_source_scratch_.data(), stereo_samples);
    if (!ScaleControllerPcm(authored, speaker_buffer, haptics_source, block.frames, speaker_enabled, dualsense_haptics_enabled,
                            speaker_gain, haptics_gain)) {
        return false;
    }
    const std::span<const float> speaker(speaker_scratch_.data(), stereo_samples);
    if (route_ == ControllerPcmRoute::DualShock4Mono) {
        const std::span<float> mono(mono_scratch_.data(), block.frames);
        return DownmixControllerSpeakerMono(speaker, mono, block.frames) && WriteMono(mono.data(), block.frames);
    }
    if (route_ == ControllerPcmRoute::DualSenseQuad) {
        const float* actuator = nullptr;
        if (dualsense_haptics_enabled) {
            const std::span<float> haptics(actuator_scratch_.data(), stereo_samples);
            const std::span<const float> source(haptics_source_scratch_.data(), stereo_samples);
            if (!haptic_filter_.Process(source, haptics, block.frames)) {
                return false;
            }
            actuator = actuator_scratch_.data();
        } else {
            haptic_filter_.Reset();
        }
        return WriteDualSense(speaker_scratch_.data(), actuator, block.frames);
    }
    return false;
}

}
