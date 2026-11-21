#pragma once

namespace pt::devices {

struct StartOptions {
    bool gamepad = false;
    bool audio = false;
    bool background = false;
    bool virtual_only = false;
};

void Start(const StartOptions& options);

bool TakeGamepadsReady();
bool GamepadsReady();
bool AudioStarting();
bool WaitAudio(double seconds);
bool EnsureAudio();
bool Stop();

}
