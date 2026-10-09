# Surround output

Surround output is optional. The existing stereo `SoundEngine::Render(out, frames)` path remains the default and keeps its original two-channel master mix. `SoundSystem::Init(vfs, open_device, surround_output)` enables host negotiation when the user setting is on.

The renderer uses Wwise's internal speaker order `FL, FR, FC, BL, BR, SL, SR, LFE`. SDL's 7.1 order is `FL, FR, FC, LFE, BL, BR, SL, SR`; its 5.1 order is `FL, FR, FC, LFE, BL, BR`. The current game speaker renderer has no LFE signal, so the LFE position is silent. Six-channel output folds each back/side pair into the corresponding SDL surround channel at -3 dB per source, preserving both rear fields.

The playback device's preferred channel count selects 7.1 at eight channels, 5.1 at six or seven channels, and stereo below six. If opening the surround stream fails, playback retries in stereo. SDL documents these channel layouts and cross-platform swizzling in [CategoryAudio](https://wiki.libsdl.org/SDL3/CategoryAudio), and `SDL_GetAudioDeviceFormat` reports the opened device format or its preferred format before opening ([API](https://wiki.libsdl.org/SDL3/SDL_GetAudioDeviceFormat)).

The per-speaker render uses the mix already routed by Wwise speaker gains. The master peak limiter shares its existing linked-channel detector and lookahead gain across the speaker channels. Existing effect buses still render as stereo and enter the master speaker field on front left and front right, as they do in the current limiter side-chain model.
