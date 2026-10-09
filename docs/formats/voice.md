# Voice recognition data

## Files

| file | size | content |
| --- | --- | --- |
| `silent/Recognition/GnD/EnglishUS.gnd` | 685 | grammar, plaintext, Sony 2013 header |
| `silent/Recognition/vrc/en-orbis.vrc` | 3,780,007 | acoustic model for Sony's psvr engine; header bytes are obfuscated, later sections carry plain strings (`ENGLISH//EN`, `PS//PS`, a phone set) |

Only English US data ships in chunk1.psarc.

## Grammar

The grammar declares language `ENGLISH//EN`, one word and one sentence rule:

| section | content |
| --- | --- |
| `!$WORDS` | `"Jack"` with pronunciation `jh ae k` |
| `!$SENTENCE!$` | `==> $WORDS` |

So the game listens for a single keyword. The phone set in the model includes filler units (`sil`, `mus`, `noi`, `nvc`, `voi`) next to the English phones, which the original engine presumably uses to reject other sounds (guess).

## Engine side

eboot links Sony's psvr engine (g2p, mfcc, hmm, gnet, saliency sources) and calls `sceAudioInOpen`, `sceAudioInInput`, `sceAudioInClose` from the P.T. code range. `ShGameCoreRecog` is the likely owner (guess from the name).

## Port implementation

`pt::VoiceRecognizer` (`src/engine/voice/voice_recognizer.*`) is fed from the SDL3 microphone at 16 kHz mono while the game listens. It listens from the f160 setup to the `ending` setup (`gameplay.md` section 7). A detection calls the same entry as the psvr result (`SetCondition(EndJack)`).

When the configured keyboard fallback is enabled, pressing both gamepad triggers (L2 + R2, or LT + RT) also submits one `Jack` keyword while the game is listening. Either trigger may be pressed first; releasing either one rearms the chord. The triggers must belong to the same controller. Gameplay and menu state decide whether the submitted command is accepted.

### Why the PocketSphinx recognizer failed (up to 2026-10-05)

The released builds (for example 0013ebb) had no Vosk runtime: `tools/prepare_voice_runtime.py` was never run for a package. So they used PocketSphinx with:

- a VAD gate at an absolute -52 dBFS of 10 ms frame energy;
- a closed grammar of `jack` and 55 distractors.

The consequences:

- A microphone whose speech reaches -48 to -63 dBFS, as testers reported on the microphone test page, rarely or never opens a segment. Nothing is decoded, and the page stays at "Recognized words: None yet".
- Every utterance that does open a segment is forced onto the closest grammar word, so an accented "Jack" becomes jacket, back or yak.
- A segment that stays above the gate for more than 6 s (a fan, the radio from the speakers) is dropped. No new segment can start until 0.26 s below the gate, so steady noise locks recognition out.
- The engine was destroyed and rebuilt on the game thread each time the game paused or the microphone test opened. Vosk's model load would have stalled the frame.

On the test set below, that recognizer (the 0013ebb exe) detects 174 of 844 positive clips (21%).

### The recognizer now

whisper.cpp 1.9.4 (MIT) and ggml are DLLs in `voice/`, loaded at run time, so `pt.exe` itself still needs only x86-64 SSE2. ggml's CPU code is built once per instruction set: `ggml-cpu-x64` (SSE2), `-sse42`, `-sandybridge`, `-haswell`, `-skylakex`, `-cannonlake`, `-cascadelake`, `-icelake` and `-alderlake`. The recognizer loads the one whose `ggml_backend_score` is highest on the player's CPU and logs `voice: whisper.cpp CPU code ggml-cpu-<name>`. `PT_VOICE_CPU=<name>` forces one, for example `x64` for the path of a CPU without AVX.

The models are downloaded by CMake with SHA-256 checks, and their MIT notices are in `voice/licenses`:

- Whisper base.en, quantized q5_1 (60 MB): `ggml-base.en-q5_1.bin`.
- The Silero VAD v6.2.0: `ggml-silero-v6.2.0.bin`.

The model loads once on the recognizer's own thread (normal priority since 2026-10-08, see "Slow under load" below) when the game starts listening or the microphone test opens, in about 0.1 to 0.2 s. It stays loaded until the game stops listening. Pausing only closes the microphone.

The pipeline:

1. **VAD.** The audio runs through the Silero VAD in 32 ms windows. Before the VAD only, the audio gets a gain that lifts the noise floor (the 10th percentile of the last 3 s) toward -62 dBFS, by at most 30 dB, so a quiet microphone is not missed. A segment opens after two 32 ms frames at a speech probability of 0.5 or more; a single intervening frame at 0.25 or more can bridge them; a frame below 0.25 or a second moderate frame before another strong frame resets the candidate. It keeps 0.3 s of audio before that and closes after 0.35 s under 0.35 (0.5 s until 2026-10-08, see "Latency" below). Once open, only frames at 0.35 or more count toward voiced duration. The 0.1 s minimum is quantized to three VAD frames (96 ms); shorter segments are dropped. It is cut at 5 s and decoded anyway, so steady noise cannot lock recognition out. Setting `PT_VOICE_DIAGNOSTICS=1` logs per-frame VAD probability and gain, short-segment drops, and microphone backlog drops.
2. **Normalization.** The segment is scaled so its 99.9th percentile reaches 0.7 of full scale, and padded to 1.5 s.
3. **Transcription.** Whisper transcribes it greedily (English, no timestamps, at most 16 tokens). The encoder runs over 384 positions (7.7 s) instead of 1500. On the test set that is 4 to 5 times faster at the same accuracy. 256 positions or fewer make the decoder repeat words.
4. **The word.** The utterance holds the word when its transcript contains, counting each word once:
   - jack, jacks, jacked, jak, jac, jaq, jacques (and close spellings), in a transcript of at most 12 different words;
   - any of the following in a transcript of at most 3 words:
     - jacket and other jack... words;
     - jock (the vowel of a Canadian or Scottish "Jack");
     - a word that sounds like "Jack" with an accent.

   **Only "Jack" (2026-10-06 evening).** "Jarith", which some players say, is no longer accepted. The original accepts only "Jack":
   - Its grammar `silent/Recognition/GnD/EnglishUS.gnd` declares the one word `"Jack"  jh ae k`, and its sentence rule is that word.
   - The eboot's detection (0x927090) compares each word of the result with `strcmp("Jack", word)`.
   - "Jarith" appears nowhere in chunk1, the eboot's strings or the acoustic model `en-orbis.vrc`. No game script or caption names it.
   - Whether Sony's engine would score a spoken "Jarith" above its 0.5 threshold against the one-word grammar cannot be checked without running that engine. Nothing in the game's data makes it an intended input.

   Up to then the port had accepted Jarith, Jareth, Jerith, Gareth, Jared, Jaren, Jarrus and Jerus (9c70f9f). Now they are negatives in `tools/voice_check.py` and `tests/voice_match_test.cpp`.

   **Accented "Jack".** The rule is `SoundsLikeJack`. Accented Latin letters are first folded to their base letters (ä, æ, é, ç, ž, č, ĵ, and the Turkish ı, ğ, ş, ö, ü). A word then sounds like "Jack" when it has:
   - an onset of j, dj, dz, dzh, dzj or zh;
   - the vowel a, e, ae, ah, eh or aa;
   - the coda k, c, ck, kk, q, cq or kh.

   That takes the Turkish "Jek", and also Jeck, Djack, Dzhek, Jäk, Džek and Jaek. No English word has that shape except jack itself, so check, deck, Zack, jet, jerk and jag stay out. ("Jake" is taken separately, as a transcript of at most 3 words: a tester's "Jack" came back as "Jake"; `tests/voice_match_test.cpp` expects it and the SAPI "Jake" negatives are accepted 7 of 8 times. A decision for the project owner, see the 2026-10-08 section.) A y onset is not taken, because the model also writes "Yek" for "yak". `tests/voice_match_test.cpp` checks 51 transcripts.

   A transcript of at most 3 words also counts when the decoder's first token gives " Jack" a probability of 0.04 or more, although the model wrote another word ("Check", "Chuck", "Yeah" for a devoiced or clipped "Jack"). Common words and all of the game's audio stay at or below 0.002.

No initial prompt is given to Whisper. On the test set (seed 31, 1036 positives, 736 negatives), biasing it toward the word lifts detection, but also turns rhymes into the word:

| prompt | detection | false accepts | of those: deck / Zack / check |
| --- | --- | --- | --- |
| none (shipped) | 73.6% | 0.68% | 0 / 1 / 0 of 8 each |
| "Jack." | 84.7% | 5.98% | 4 / 3 / 0 |
| "Jack. Jarith." | 87.6% | 5.30% | 4 / 3 / 1 |
| "Jack, Jarith, check, deck, Zack, yeah." | 85.9% | 4.48% | 3 / 1 / 0 |
| none first, then "Jack. Jarith." for short utterances with p(jack) at least 0.01 | 77.6% | 1.90% | 1 / 2 / 0 |

Every utterance is logged with its transcript, p(jack), length and decode time. The microphone test page shows the last transcript and a peak-hold level meter (falling 60 dB a second). `PT_VOICE_DUMP=<folder>` saves every segment as the model gets it.

### Tests

- **`tools/voice_check.py`** builds a test set and runs it through `pt.exe --voice-test <list>`.
  - Positives: Windows SAPI voices (David, Zira, Mark, desktop and OneCore) saying Jack, Hey Jack, Jack it's me, Jack where are you, Jack twice, Jack with IPA accents (dʒak, dʒɛk, dʒæːk, jæk), slow, low and high pitch, soft, and Jarith or Jareth, each at three rates.
  - Negatives: 47 other words and phrases from the same voices.
  - Variations: 3 random versions of each clip, at -20 to -60 dBFS, under pink or white noise, a fan, the start room ambience or the hallway radio mix 6 to 30 dB below, flat, headset or telephone band, and pitched 0.85 to 1.15.
  - Also: all 318 of the game's audio files, long recordings of the radio and the hallway, noise and silence.
- **Run on 2026-10-06, base.en q5_1, cascadelake CPU code, 4 threads.** Two sets with different random seeds. In the first set, the 8 "jock" clips are counted as positives.

| clips | count | PocketSphinx (0013ebb) | Whisper |
| --- | --- | --- | --- |
| Jack, said at -20 to -40 dBFS | 300 | 41% | 93% |
| Jack at -50 to -60 dBFS | 192 | 16% | 94% |
| Jarith, Jareth (accepted until 2026-10-06 evening, now negatives) | 144 | 1% | 85% |
| Jack as [dʒak] | 48 | 0% | 81% |
| hard renderings: "yack" (jæk), "jeck" (dʒɛk), extra slow | 144 | 8% | 42% |
| Jack over the radio from the speakers, 6 to 15 dB under it | 12 | 42% | 100% |
| all positives, set 1 / set 2 | 844 / 892 | 21% / - | 82.8% / 81.6% |
| the game's audio (318 files, long radio and hallway) | 314 | 0 | 0 |
| noise and silence | 13 | 0 | 0 |
| other words, set 1 / set 2 | 352 / 680 | 3 | 3 / 4 (Zack, deck, tack, yak) |

- **Accents, run of 2026-10-06 evening.** This is set 3 (seed 31, the shipped rules). It adds Turkish-accent clips and the near words jerk, geek, jug, yet, yep, jag and jail.
  - Overall: detection 73.6% (763/1036), false accepts 0.68% (5/736: jag 3, tack 1, Zack 1).
  - Game audio and noise: 0 of 327. Check, deck, Jake and jerk: 0 of 8 each.
  - "Jeck" spoken as text: 30/48. "Hey, Jek" (dʒɛk): 34/48. dʒɛk alone: 29/48; the misses are transcribed "Check".
  - The IPA dʒek (`jek_tr`): 0/48. The SAPI voices pronounce it as "Jake", which stays rejected.
  - Plain Jack: 22/24, Hey Jack 45/48. Jarith was still a positive in this run: 38/48.
  - The total is lower than in sets 1 and 2 because this set adds the hard accent groups.
  - The user's own case, a Turkish "Jek" that Whisper writes as "jek", is taken by `SoundsLikeJack` (`tests/voice_match_test.cpp`).
- **A bad laptop microphone with fan noise (`tools/voice_check.py --fan`, seed 41).**
  - The set: every SAPI clip in laptop fan noise at 20, 10, 5 and 0 dB speech-to-noise ratio. The noise is:
    - broadband air, low-passed at 2.5 kHz and slowly breathing;
    - a blade tone of 180 to 420 Hz with harmonics and wobble;
    - mains hum at 50 or 60 Hz with harmonics to the 9th;
    - a faint coil whine at 5 to 7.5 kHz.
  - The mix then goes through a cheap capture chain:
    - band-limited to 300 Hz to 4 kHz;
    - a Windows-like automatic gain that pulls every pause up toward -20 dBFS (fast attack, 30 dB/s release, at most +30 dB);
    - soft clipping;
    - 10-bit samples.
  - The set has 876 positives and 852 negatives (other words, Jarith included), plus 30 s of fan noise alone at -60, -45, -35 and -25 dBFS.
  - "Core" below is the Jack clips without the hard renderings (yack, jeck, IPA dʒek, extra slow): 171 per ratio. FA counts the false accepts among 212 negatives per ratio.

| | 20 dB | 10 dB | 5 dB | 0 dB | decode median |
| --- | --- | --- | --- | --- | --- |
| shipped (base.en, Silero at 0.5) | 91% / FA 7 | 71% / FA 7 | 58% / FA 8 | 24% / FA 3 | 0.17 s |
| spectral subtraction before the VAD | 91% / 7 | 73% / 9 | 54% / 6 | 26% / 3 | 0.16 s |
| spectral subtraction before the VAD and Whisper | 89% / 7 | 74% / 9 | 49% / 4 | 17% / 3 | 0.17 s |
| Whisper small.en q5_1 (190 MB) | 94% / 3 | 76% / 4 | 55% / 6 | 23% / 2 | 0.65 s |
| VAD opens at 0.3 and closes under 0.2 | 92% / 7 | 71% / 6 | 56% / 7 | 30% / 4 | 0.15 s |

  - Fan noise alone opened no segment in any variant, so it never reaches Whisper and cannot make the word.
  - The false accepts are rhymes the model writes as "Jack" (jag, deck, Zack) or puts above 0.04 in p(jack) (Jeff, jerk).
  - At 10 dB and below, the misses are Whisper's: "John" (31 of the 5 and 10 dB misses), "Check", "Yeah", "Job". The 300 Hz to 4 kHz band and the gain pumping take away what tells "Jack" from "John". At 0 dB, half the misses never open a segment.
  - Spectral subtraction (minimum-statistics noise per bin, over-subtraction 1.5, floor -18 dB) gains 2 points at 10 dB and loses at 0 to 5 dB.
  - small.en gains 3 to 5 points and halves the false accepts, at 3 to 4 times the decode time and 130 MB more in the package.
  - A lower VAD threshold gains only at 0 dB.
  - None is shipped. The recognizer is the same as before; this set is a baseline for a later change (an RNNoise-class denoiser, BSD-3, would be the next to try).
- **Decode time.** Median 0.28 s and 90th percentile 0.52 s per utterance on this machine (cascadelake variant, 4 threads). The forced SSE2 variant (`--cpu x64`) takes about 2.1 s per utterance, with 45 of 57 positives and 0 of 46 negatives on a sample.
- **CPU cost.** The VAD costs about 0.002 s of CPU per second of audio (20 s of noise: 0.19 s of CPU including the model load). An utterance costs its decode time on up to 4 threads: 90 s of the radio playing aloud is 9 utterances, 8.3 s of CPU. The game thread only queues samples.
- **Latency.** From the end of the word: the silence that closes the segment (0.352 s since 2026-10-08, 11 VAD frames; 0.512 s before) plus the decode (median 0.14 to 0.16 s), so about 0.5 s at the median (0.68 s before).
- **`tools/walkthrough.py voice`** plays the true end with `PT_VOICE_INPUT=<wav>` standing in for the microphone (a SAPI "Jack" in quiet room noise, looped every 6 s) and with no scripted voice command. The first f160 pass walks through. In the second pass the recognizer hears the word, `TrueEnd heard Jack` follows, and the true end completes.
- **`tools/voice_mic_session.ps1`** is a 3 minute session with a real microphone, run by the player. A console lists what to say (Jack eleven ways, then Jarith and other words that must not count, and talk) and prints what was heard. It keeps the log and every segment.

The original accepts anything its single-word grammar scores above 0.5. This recognizer also accepts a few rhymes (Zack, deck, tack) at a rate of about 1%, while the game's own speech and noise produced no false accept.

Unicode filesystem paths remain native paths when opened and are converted explicitly to UTF-8 at SDL and log boundaries. The selected ggml CPU backend is loaded through the native platform loader and registered only after checking the pinned runtime API version; its module stays loaded for the registry lifetime. Path regression tests cover Turkish letters, accented Latin, Japanese, Cyrillic, Arabic, Greek, Hebrew, Devanagari, Korean, Thai, and emoji. This validates resource loading, not recognition accuracy across accents or microphones.

## 2026-10-08 night: first utterance, load, failures, test page

Measured on this machine (8 cores, 16 threads, cascadelake variant, 4 threads), base.en q5_1, SAPI voices.

### One "Jack", right after the microphone opens (`tools/voice_onset_check.py`)

Each clip is one utterance (Jack, Jack!, Hey Jack, Jack it's me, Jack where are you; David, Zira, Mark; 15 per row) behind 0.1 s of digital silence (the stream opening) and room noise, or exact silence for a noise-gated headset. The recognizer is Reset before each clip, as the game does when it opens the microphone.

| scenario | detected |
| --- | --- |
| word at -20, -30, -40, -50 dBFS, 0.3 s after the stream opens (pink noise at -55) | 15/15 each |
| word at -60 dBFS (5 dB under the noise) | 9/15 |
| -30 dBFS after 0.3, 1, 3, 10 s of noise | 15/15 each |
| noise at -70 or -45 dBFS, fan noise, the start room's ambience, an opening click | 15/15 each |
| noise gate (exact silence), word at -20, -35, -50 dBFS, after 1 s or 10 s | 15/15 each |
| all | 294/300 = 98.0%, the same before and after the changes below |

So the first utterance is not lost to the start-up state in this model of the microphone: the adaptive gain (noise floor from whatever history exists, at most 30 dB), the empty pre-roll, the first decode (130 to 180 ms, no warm-up penalty), digital silence at the start and a noise gate all pass. What is left is what the corpus shows: a clear plain "Jack" is found 96% (23/24, Hey Jack 47/48, Jack it's me 47/48) and accents are not (Jek 39/48, "ca" 35/48, "e" 29/48, slow 26/48); the community's "three times, three seconds apart" is three independent tries at that rate. The misses are the model's: "Check", "Yeah", "John", or a hallucinated sentence on a noisy short segment (p(jack) near 0 there, so no rescue by the first-token rule: a rule "p(jack) of 0.05 or more in a long transcript of a short segment" adds 0 to 3 clips in 888).

### Slow under load (the cause found tonight)

ggml runs a decode on the worker thread plus 3 helper threads it creates itself. The helpers run at normal priority and wait for each other at a barrier. With the worker at below normal priority (the old setting), any load on the CPU made the worker the straggler and stalled all four. `load_test` (the same 12 to 40 Jack clips while N busy loops of normal priority run, 16 logical CPUs here; decode time per utterance):

| busy loops | worker below normal (old), 4 threads | worker normal (now), 4 threads | 2 threads | 1 thread |
| --- | --- | --- | --- | --- |
| 0 | median 155 ms | 160 ms | 259 ms | 515 ms |
| 8 | 223 ms | 213 ms | 388 ms | 670 ms |
| 12 | 5385 ms (p90 11549) | 384 ms (p90 499) | 406 ms | 738 ms |
| 14 | 10624 ms (p90 17920, runs timed out) | 740 ms (p90 934) | - | - |
| 16 | 20 to 40 s per utterance | - | - | - |

A game that keeps most of a smaller CPU busy is that situation, and a word that arrives 10 s late is a word that was "missed" when the player tries again. The worker is now at normal priority (`Settings::priority`, `PT_VOICE_TUNE=prio=-1` restores the old one). The decode is short bursts (0.15 s on 4 threads per utterance) and the VAD costs 0.002 s per second of audio.

### Latency: the silence that closes a segment

`Settings::end_seconds` 0.5 to 0.35 s. On the corpus below it moves 1 clip (+1 detection) and 0 false accepts; 0.25 s loses 0.6 points (78.3%). A sentence with a pause ("Hey, Jack", "Jack, it's me") now becomes two segments more often (the onset check shows 1 to 5 extra utterances per 15 clips); the word is found in its own piece.

### Corpus before and after (`tools/voice_check.py --seed 31`, 888 positives, 751 negatives)

| | before (172093d) | after |
| --- | --- | --- |
| detection | 701/888 = 78.9% | 702/888 = 79.1% |
| false accepts | 12/751 = 1.60% | 12/751 = 1.60% |
| of those | Jake 7/8 (see above), jag 3/8, tack 1/8, Zack 1/8 | the same clips |
| game audio (318 files, long radio and hallway), noise, silence | 0/327 | 0/327 |
| other rhymes: check, deck, chuck, jerk, jet, Jarith, Jareth | 0 | 0 |
| decode median / p90 | 164 / 267 ms | 144 / 175 ms |

(This clip set is the older 888 + 751 cache; the 73.6% and 0.68% above are from a differently built set with the same seed.) A lower first-token threshold, computed from the logged p(jack) of the baseline run: 0.03 adds 15 detections and no false accept; 0.02 adds 34 and 4 (jag, yak, Zack); 0.01 adds 64 and 16. Not changed: 0.04 stays, because a rhyme counted as the word is worse than a missed one, and these gains come from the same synthetic set.

### Player reports

- `pt (10).log` (HyperX Cloud III, haswell variant): the recognizer ran, decodes took 160 to 210 ms, and every utterance is a different, long transcript ('Yeah, this is a way to say Jorif diga', 'Possessed, okay, shk back', 'but uh, JARUS!', 'Jourissa', 'J', 'J-r-est'). p(jack) is 0.000 to 0.002 throughout, so the model heard no "Jack" at all in any of them; this is a speaker saying "Jarith" or another word, which is rejected on purpose. Nothing in the decoder settings is a fix without accepting that word. `pt (9).log`: 'Mmm' and 'Hey you', the same.
- `issue1.log` (user folder with Ş and a space): the old build asked ggml to load the CPU code from a UTF-8 narrow path (`load_backend: failed to load ...ggml-cpu-sandybridge.dll`). Commit 230914d loads it with `LoadLibraryExW` and registers the backend itself. Checked with this build in `...\Şefan Vinaji\AppData\Local\Programs\P.T. PC Port\` (the same name and layout, a copy of the runtime): `voice: whisper.cpp CPU code ggml-cpu-cascadelake`, models loaded, `jack-three-second-gaps.wav` gives 3 detections. Also `pt_voice_match_test` (54 of 54 transcripts, models load from a path with Turkish, Japanese, Cyrillic, Arabic, Greek, Hebrew, Hindi, Korean, Thai and emoji).
- The message blamed the CPU for any failure. Now (`LoadRuntime`): no `ggml-cpu-*` file in the folder; none of them could be loaded (with the Windows error text: `Windows error 193: %1 is not a valid Win32 application`) "a path or blocked or missing DLL problem, not the CPU"; they loaded but this CPU runs none ("this CPU runs none of the N ggml-cpu variants"); one loaded but did not register (backend API version). Tested with corrupt DLLs (second message) and a missing model (`ggml-base.en-q5_1.bin is missing`); the CPU message needs a CPU that scores 0 on every variant and was not run.
- Test page "always none": the "Recognized words" row shows why instead, and its hint (and the level row's) gives the sentence: loading, no microphone (`pc_mic_unavailable`), speech files missing, speech engine failed to load, CPU not supported, speech model failed, no sound for 3 s while nothing has been heard (muted, wrong device; the meter peak is at or under -75 dBFS), else "None yet" with "Say Jack ...". `VoiceRecognizer::GetFailure()` carries the reason. 12 keys (`pc_mic_st_*`, `pc_mic_err_*`) in English and the 11 translation tables. `PT_VOICE_INPUT=<wav>` makes the page testable headless (`tools/menu_quit_check.py`'s route plus a few waits): a silent wav shows "No sound", `PT_VOICE_CPU=bogus` shows "Speech engine failed", a Jack clip shows "Jack"/"Yack", and the level meter followed the words (-80, -70, -18, -23, -29 dBFS in successive frames).

### Tried tonight and not changed (same corpus, detection / false accepts)

| change | result |
| --- | --- |
| VAD onset bridging and the 3-frame minimum (dcc30c2) switched off (`bridge=0,minspeech=0.128`) | corpus 79.1% / 12, the same; but `tests/voice_vad_check.py` fails: `jack-three-second-gaps.wav` gives 1 of 3 detections and `jack-tail-cut-50ms.wav` 0 of 1. Kept. |
| `startp=0.4` (a segment opens on a weaker VAD frame) | 79.4% / 13: +3 detections for +1 false accept. Not taken. |
| `beam=5` (beam search instead of greedy) | 78.9% / 12, decode median 192 ms. No gain. |
| silence closing a segment 0.25 s | 78.3% / 12 |
| a lower first-token threshold | see above (0.03: +15 and +0 on this set) |

Of the 186 missed positives only 14 never opened a segment (all at -50 dBFS or lower under 6 to 10 dB noise): the misses are the decoder's, not the VAD's, so onset tuning cannot lift the single-"Jack" rate further.

### Measuring

`PT_VOICE_TUNE=name=value,...` overrides `Settings` for a run (start, end, startp, endp, preroll, minspeech, maxsegment, floor, maxgain, maxwords, jackp, threads, prio, bridge, beam), for `tools/voice_check.py` and the onset check. Not a player setting.

## 2026-10-08 second night: a second opinion from a larger model

Goal: more single spoken "Jack" across accents and microphones without more false accepts. Two corpora, the same clips for every row:

- **main**: `tools/voice_check.py --seed 31` (888 positives, 751 negatives, the game's audio and noise: 327 clips).
- **accent**: `tools/voice_check.py --accent --seed 11` (648 positives, 200 negatives). New: IPA renderings of "Jack" (zh onset, open a, a vowel after the k, long vowel, a clipped word without its k), very slow, very fast, whispered, shouted, "Jaaack", each at 3 rates and 5 SAPI voices, with room reverb, a clipped (overdriven) microphone, very low gain (-55 to -62 dBFS), headset or telephone band, noise, speed changes; and as negatives the words the player of `pt (10).log` was heard saying (Jorif diga, Jorith, shk back, Possessed okay, JARUS, Jourissa, J r est, James, Hey you, Mmm, Lee Soutnich possessed, That's Jeff, last time, John, Josh, Job) plus every sixth ordinary negative under the same effects. Only en-US SAPI voices are installed here, so "accents" are phoneme renderings, not foreign speakers. "Chack" ([tS ae k]) is not in the set: the model writes it "Check", which is another word.

| config | main detection | main FA | game audio and noise FA | accent detection | accent FA | decode median / p90 (4 threads) | model files |
| --- | --- | --- | --- | --- | --- | --- | --- |
| base.en q5_1 (before) | 702/888 = 79.1% | 12/751 | 0/327 | 408/648 = 63.0% | 3/200 | 144 / 175 ms | 57 MB |
| base.en q8_0 | 705 = 79.4% | 13 | 0 | 406 = 62.7% | 2 | 150 / 169 ms | 78 MB |
| base.en f16 | 707 = 79.6% | 13 | 0 | 407 = 62.8% | 4 | 178 / 194 ms | 141 MB |
| small.en q5_1 alone | 749 = 84.3% | 13 | 0 | 437 = 67.4% | 2 | 545 / 612 ms (up to 795 ms in the accent set) | 181 MB |
| small.en q8_0 alone | 754 = 84.9% | 14 | 0 | 439 = 67.7% | 1 | 459 / 552 ms | 252 MB |
| base (multilingual) q5_1 alone | 729 = 82.1% | 36 | 0 | 435 = 67.1% | 10 | 222 / 254 ms | 57 MB |
| small (multilingual) q5_1 alone | 791 = 89.1% | 24 | 0 | 504 = 77.8% | 7 | 600 / 688 ms | 181 MB |
| **base.en, then small.en q5_1 on a short utterance that is not the word (shipped)** | **769 = 86.6%** | **13** | **0** | **484 = 74.7%** | **4** | 168 / 822 ms | 57 + 181 MB |
| the same, first-token threshold 0.03 in both | 775 = 87.3% | 14 | 0 | 496 = 76.5% | 5 | | |
| base.en, then small (multilingual) q5_1, threshold 0.04 | 800 = 90.1% | 23 | 0 | 521 = 80.4% | 7 | | 57 + 181 MB |
| the same, text only (no first-token rule) | 761 = 85.7% | 14 | 0 | 481 = 74.2% | 4 | | |
| base.en, then "Jack." as initial prompt | 814 = 91.7% | 45 | 0 | 510 = 78.7% | 11 | | 57 MB |
| base.en, then sampling at 0.4, best of 5 | 704 = 79.3% | 12 | 0 | 410 = 63.3% | 3 | | 57 MB |
| base.en, then beam search 5 | 703 = 79.2% | 12 | 0 | 411 = 63.4% | 3 | | 57 MB |

Notes on the rows:

- A bigger quantization of the same model (q8_0, f16) moves nothing: the misses are the model's errors, not rounding.
- The larger model alone is slow (0.5 to 0.8 s every utterance, and more under load). The two models make different mistakes (the union of their detections is 88.1% on main against 84.3% for small.en alone), so the **cascade** runs small.en only on a short utterance (at most 3 words and 3 s) that base.en did not take for the word. The word then counts when small.en writes it (or its first token gives " Jack" 0.04 or more with at most 3 words). A word base.en takes is as fast as before (0.14 s).
- New false accepts of the cascade: `jag` -> "check | Jack" (1 clip of main) and `Jacob` -> "Jake Hand" (the Jake rule, accent set). Jake stays accepted as before (7/8 of the main set, unchanged).
- The multilingual models find more but spell rhymes as the word more often (deck, jag, tack, yak, zack): 23 to 36 false accepts on main. Rejected. An initial prompt "Jack." does the same at 45 (6%): rejected, as in the first table above.
- The threshold 0.03 gains +6 (main) and +12 (accent) detections and costs one false accept each: yak -> "Yuck" and jerk -> "Check" (p 0.032). Rhymes counted as the word: not taken.
- Per group, before -> after (detections of 48 unless noted): main: Jack with an e vowel (jack_e) 29 -> 43, slow 26 -> 37, "Hey, Jek" 34 -> 41, "Jeck" 35 -> 43, "ca" 35 -> 41, "ae" 40 -> 47, Jek (TR) 39 -> 42, Jack over the radio 8/12 -> 11/12, plain Jack 23/24 -> 23/24 and Hey Jack 47/48 -> 47/48 (the base model already takes those). Accent: zh-onset [z a k] 17 -> 29 and [z ae k] 32 -> 41, long vowel [dz e: k] 27 -> 39, slow Jek 20 -> 29, slow and low 30 -> 38, "Jaaack" 24 -> 31, whispered 44 -> 48, fast 17 -> 18, the clipped word without its k 23 -> 24. The player's words: 0 false accepts of 128 clips (Jorith included).

### Cost of the second opinion

- **Package and memory:** `ggml-small.en-q5_1.bin` is 181 MB (SHA-256 `bfdff489...ad30`, the official ggerganov/whisper.cpp file, in `cmake/Dependencies.cmake` and `tools/package.py`): the voice models go from 58 MB to 239 MB. Peak working set of `--voice-test` rises from 190 MB to 659 MB, while the game listens (f160 to the ending) or the microphone test page is open. A missing second model only logs a warning; the base model still runs alone.
- **Time, on an utterance that is rescued** (the base decode plus small.en; 13 such clips; this machine, cascadelake, 4 threads; the same clips at normal worker priority with busy loops on the other cores): 651 ms median (p90 673) alone; 948 ms (p90 974) beside 8 busy loops; 1204 ms (p90 1482) beside 12 of 16 logical CPUs; with 2 threads 1180 ms alone and 1935 ms beside 12. An utterance base.en takes is unchanged (about 0.14 s). A rescued word is therefore detected about 0.35 s of silence plus 0.65 s after it ends; before, it was not detected.
- `PT_VOICE_TUNE` gained `rescue` (0 off, 1 prompt, 2 sampling, 3 beam, 4 second model), `rescuep`, `rescuewords`, `rescuesec`, `rescuejackp`, `nst`; `PT_VOICE_MODEL`, `PT_VOICE_RESCUE_MODEL` and `PT_VOICE_PROMPT` pick other files or a prompt for measurements. `tools/voice_check.py --tag` runs several configurations on one set side by side.

Not verified: real human speech and real foreign accents (only SAPI), a real mid-range CPU (the load rows are busy loops on a 16-thread CPU), and the effect on frame time in the game while a rescue runs.
