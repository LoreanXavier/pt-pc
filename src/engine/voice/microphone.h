#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct SDL_AudioStream;

namespace pt {

class Microphone {
public:
    ~Microphone() { Close(); }
    Microphone() = default;
    Microphone(const Microphone&) = delete;
    Microphone& operator=(const Microphone&) = delete;
    bool Open(int sample_rate, const std::string& device_name = {});
    void Close();
    bool IsOpen() const { return stream_ != nullptr; }
    size_t Read(std::vector<int16_t>& out);
    bool SetMonitor(bool enabled);

private:
    SDL_AudioStream* stream_ = nullptr;
    SDL_AudioStream* monitor_ = nullptr;
    int sample_rate_ = 16000;
};

}
