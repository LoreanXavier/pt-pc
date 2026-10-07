# Radio Tone

A sound replacement. The radio's first broadcast (`Play_radio_f010`) plays the Wwise media with id 615826540 from
`sfx_common`. This mod ships `Assets/sh/sound/wem/615826540.wav`, a twelve second 440 Hz tone with a slow tremolo, in
plain 16-bit PCM; the game picks it up instead of the bank's own file and plays it from the radio with the radio's own
volume and position.

Copy the folder into `mods/` next to `pt.exe`. `pt.log` says `mods: sound media 615826540 from
/Assets/sh/sound/wem/615826540.wav` when the banks load, and the radio hums the tone once you walk down the first
corridor.

To find another sound's id: `python tools/wwise_bank.py extract <game>/chunk1/as/sh/sound/asset/sfx_common.sbp --out wem`
writes every embedded file as `wem/sfx_common/<id>.wem`; listen to them with vgmstream or foobar2000 and replace the
one you want with `<id>.wav`.
