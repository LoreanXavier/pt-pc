# Voice recognition

## The game's data

| file | size | content |
| --- | --- | --- |
| `silent/Recognition/GnD/EnglishUS.gnd` | 685 | grammar, plaintext, Sony 2013 header |
| `silent/Recognition/vrc/en-orbis.vrc` | 3,780,007 | acoustic model for Sony's psvr engine; header bytes are obfuscated, later sections carry plain strings (`ENGLISH//EN`, `PS//PS`, a phone set) |

Only English US data ships in chunk1.psarc. The grammar declares language `ENGLISH//EN`, one word and one sentence
rule: `!$WORDS` is `"Jack"` with pronunciation `jh ae k`, and `!$SENTENCE!$` is `==> $WORDS`. So the game listens for a
single keyword, and its detection compares each recognized word with `"Jack"`. "Jarith", which some players say,
appears nowhere in the game's data, so the port does not accept it either.

## The port's recognizer

The PS4 engine cannot be reused, so the port listens with whisper.cpp 1.9.4 (MIT) and ggml, loaded at run time from
`voice/`. ggml's CPU code is built once per instruction set (`ggml-cpu-x64` for plain SSE2 up to `-alderlake`), and the
recognizer loads the best one for the player's CPU, logged as `voice: whisper.cpp CPU code ggml-cpu-<name>`;
`PT_VOICE_CPU=<name>` forces one. The models are downloaded by CMake with SHA-256 checks, their notices in
`voice/licenses`: Whisper base.en quantized q5_1 (60 MB) and the Silero VAD 6.2.0.

The model loads on the recognizer's own thread when the game starts listening (from the f160 setup to the ending) or
the microphone test page opens, in about 0.1 to 0.2 s, and stays loaded until the game stops listening. The pipeline:

1. The audio (16 kHz mono from the SDL3 microphone) runs through the Silero VAD in 32 ms windows. Before the VAD only,
   a gain lifts the noise floor (the 10th percentile of the last 3 s) toward -62 dBFS, by at most 30 dB, so a quiet
   microphone is not missed. A segment opens after 64 ms at a speech probability of 0.5 or more and keeps 0.3 s of
   audio before that; it closes after 0.5 s under 0.35, is cut at 5 s and decoded anyway, and is dropped with less than
   0.1 s of speech.
2. The segment is scaled so its 99.9th percentile reaches 0.7 of full scale, and padded to 1.5 s.
3. Whisper transcribes it greedily (English, no timestamps, at most 16 tokens), with the encoder run over 384 positions
   instead of 1500, which is 4 to 5 times faster at the same accuracy on the test set.
4. The utterance counts when the transcript contains jack, jacks, jacked, jak, jac, jaq, jacques or a close spelling
   (in a transcript of at most 12 different words), or, in a transcript of at most 3 words, jacket and other jack...
   words, jock, or a word that sounds like "Jack" with an accent: after folding accented letters, an onset of j, dj,
   dz, dzh, dzj or zh, the vowel a, e, ae, ah, eh or aa, and the coda k, c, ck, kk, q, cq or kh (Jek, Jeck, Djack,
   Dzhek, Jäk). A transcript of at most 3 words also counts when the decoder's first token gives " Jack" a probability
   of 0.04 or more although the model wrote another word ("Check", "Chuck", "Yeah" for a devoiced or clipped "Jack").

No prompt is given to Whisper: a prompt lifts detection but also turns rhymes (deck, Zack, check) into the word.

Every utterance is logged with its transcript, p(jack), length and decode time. The microphone test page in the PC
settings shows the last transcript and a level meter. `PT_VOICE_DUMP=<folder>` saves every segment as the model gets
it. Latency from the end of the word is about 0.8 s: 0.5 s of silence to close the segment plus the decode (median
0.28 s on a desktop CPU with 4 threads; about 2 s on the SSE2 path). If you have no microphone, `[voice] key = J` in
`pt.ini` lets a key stand in for the word.

## Tools

- `tools/voice_check.py --exe build/release/pt.exe --out <dir>` builds a test set from Windows SAPI voices (the word in
  several voices, rates and accents, and other words), optionally the game's own audio, and synthetic noise, varies
  the level from -20 to -60 dBFS and the noise under it, runs every clip through `pt.exe --voice-test` and prints the
  detection and false accept rates per group, the decode time and the worst cases. `--cpu x64` runs the SSE2 path,
  `--limit N` keeps every Nth clip, `--fan` builds a noisy laptop set (fan noise, mains hum, a cheap capture chain).
  On the main set the shipped recognizer detects about 74 to 83 % of the "Jack" clips depending on how many hard
  accent groups the set includes, with under 1 % false accepts (rhymes such as Zack, deck, jag), and none on the
  game's own audio or noise.
- `tools/voice_mic_session.ps1` is a 3 minute session with a real microphone: a console lists what to say (Jack in
  several ways, then words that must not count, and talk) and prints what was heard. It keeps the log and every
  segment.
- `tests/voice_match_test.cpp` (`pt_voice_match_test`) checks the word rules on 51 transcripts.
