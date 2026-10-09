"""Detection and false accept rates of the true end's voice recognizer (formats/voice.md).

Builds a test set from Windows SAPI voices (the word and its spellings in several voices, rates and accents, and other
words), the game's own audio (radio, voices, music, ambience) and synthetic noise, varies the level from -20 to -60 dBFS
and the noise under it, runs every clip through `pt.exe --voice-test` and prints the rates per group, the decode time
and the worst cases. Nothing is written outside --out.

  python tools/voice_check.py --exe build/release/pt.exe --out <scratch>/voice_check [--game-audio <dump>/audio]

--cpu x64 runs ggml's plain x86-64 (SSE2) CPU code instead of the best for this CPU and checks the log that it did, the
path of a CPU without AVX; --limit N keeps every Nth clip for a quick run.
"""
import argparse
import hashlib
import itertools
import json
import re
import subprocess
import sys
from pathlib import Path

import numpy as np
import soundfile as sf
from scipy import signal as sps

RATE = 16000
VOICES = {'david': 'Microsoft David Desktop', 'zira': 'Microsoft Zira Desktop', 'mark': 'Microsoft Mark',
          'davidm': 'Microsoft David', 'ziram': 'Microsoft Zira'}


def ph(ipa, word='Jack'):
    return f"<phoneme alphabet='ipa' ph='{ipa}'>{word}</phoneme>"


POSITIVE = {
    'jack': 'Jack', 'jack_excl': 'Jack!', 'jack_q': 'Jack?', 'hey_jack': 'Hey, Jack.', 'jack_its_me': "Jack, it's me.",
    'jack_twice': 'Jack. Jack!', 'jack_where': 'Jack, where are you?',
    'jack_ca': ph('dʒak'), 'jack_e': ph('dʒɛk'), 'jack_long': ph('dʒæːk'), 'jack_y': ph('jæk'), 'jack_ae': ph('dʒæk'),
    'jack_slow': "<prosody rate='x-slow'>Jack</prosody>", 'jack_lowp': "<prosody pitch='x-low'>Jack</prosody>",
    'jack_highp': "<prosody pitch='x-high'>Jack</prosody>", 'jack_soft': "<prosody volume='x-soft'>Jack</prosody>",
    # a Turkish (or Slavic, German) accent: the vowel fronted to e, as players write it "Jek"
    'jek_tr': ph('dʒek', 'Jek'), 'jek_tr_long': ph('dʒɛːk', 'Jek'), 'jek_text': 'Jek', 'jeck_text': 'Jeck',
    'hey_jek': "Hey, " + ph('dʒɛk', 'Jek'),
    'jock': 'jock',
}
NEGATIVE = {
    'back': 'back', 'black': 'black', 'track': 'track', 'check': 'check', 'chuck': 'Chuck', 'jake': 'Jake', 'yak': 'yak',
    'shack': 'shack', 'hello': 'hello', 'yes': 'yes', 'no': 'no', 'what': 'what?', 'lisa': 'Lisa',
    'help': 'help me', 'door': 'open the door', 'where': 'where am I', 'scared': "I'm so scared", 'crazy': 'this is crazy',
    'jessica': 'Jessica', 'jacob': 'Jacob', 'jam': 'jam', 'jazz': 'jazz', 'zack': 'Zack', 'jeff': 'Jeff', 'hey': 'hey',
    'numbers': 'two zero four eight six three', 'hell': 'what the hell', 'okay': 'okay', 'mom': 'mom', 'radio': 'turn off the radio',
    'snack': 'snack', 'tack': 'tack', 'sack': 'sack', 'hack': 'hack', 'pack': 'pack', 'jet': 'jet', 'chat': 'chat',
    'deck': 'deck', 'neck': 'neck', 'garage': 'the garage', 'baby': 'the baby is crying', 'fridge': 'look in the fridge',
    # "Jarith" is not the original's word (formats/voice.md): a negative
    'jarith': ph('dʒæɹɪθ', 'Jarith'), 'jareth': 'Jareth',
    'jerk': 'jerk', 'geek': 'geek', 'jug': 'jug', 'yet': 'yet', 'yep': 'yep', 'jag': 'jag', 'jail': 'jail',
    'gun': 'gun control', 'jacket_story': 'my jacket is wet and the hallway is dark',
}
RATES = (-3, 0, 3)


def tts(cache, out_dir, positive=None, negative=None):
    positive = POSITIVE if positive is None else positive
    negative = NEGATIVE if negative is None else negative
    out_dir.mkdir(parents=True, exist_ok=True)
    lines = []
    for (key, text), (vname, voice), rate in itertools.product(positive.items(), VOICES.items(), RATES):
        lines.append(f'pos_{key}_{vname}_r{rate}\t{voice}\t{rate}\t{text}')
    for (key, text), (vname, voice) in itertools.product(negative.items(), VOICES.items()):
        lines.append(f'neg_{key}_{vname}_r0\t{voice}\t0\t{text}')
    todo = [l for l in lines if not (out_dir / (l.split('\t')[0] + '.wav')).exists()]
    if todo:
        listing = cache / 'tts_list.txt'
        listing.write_text('\n'.join(todo) + '\n', encoding='utf-8')
        script = cache / 'tts.ps1'
        script.write_text(r'''param([string]$list, [string]$out)
Add-Type -AssemblyName System.Speech
$s = New-Object System.Speech.Synthesis.SpeechSynthesizer
$fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(16000, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
foreach ($line in Get-Content -Encoding UTF8 $list) {
  $p = $line -split "`t"
  $s.SelectVoice($p[1]); $s.Rate = [int]$p[2]
  $s.SetOutputToWaveFile((Join-Path $out ($p[0] + ".wav")), $fmt)
  $s.SpeakSsml("<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xml:lang='en-US'>" + $p[3] + "</speak>")
  $s.SetOutputToNull()
}
''', encoding='utf-8')
        # pwsh: Windows PowerShell 5.1 cannot select the OneCore voices
        subprocess.run(['pwsh', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(script), '-list', str(listing),
                        '-out', str(out_dir)], check=True)
    # the OneCore and desktop voices of one speaker can render identical audio: keep one copy
    seen, clips = set(), []
    for wav in sorted(out_dir / (line.split('\t')[0] + '.wav') for line in lines):
        digest = hashlib.sha1(wav.read_bytes()[44:]).hexdigest()
        if digest not in seen:
            seen.add(digest)
            clips.append(wav)
    return clips


def load(path, seconds=None):
    audio, rate = sf.read(path, always_2d=True, dtype='float32')
    audio = audio.mean(axis=1)
    if rate != RATE:
        audio = sps.resample_poly(audio, RATE, rate).astype(np.float32)
    if seconds:
        audio = audio[:int(seconds * RATE)]
    return audio


def active_rms(x):
    """RMS over the 50 ms frames within 20 dB of the loudest one: the level of the speech, not of the pauses."""
    n = 800
    frames = x[:len(x) // n * n].reshape(-1, n) if len(x) >= n else x.reshape(1, -1)
    power = (frames ** 2).mean(axis=1)
    loud = power[power >= power.max() * 0.01]
    return float(np.sqrt(loud.mean() + 1e-20))


def noise(kind, length, rng):
    t = np.arange(length) / RATE
    if kind == 'white':
        return rng.normal(size=length)
    if kind == 'pink':
        white = rng.normal(size=length)
        b, a = [0.049922035, -0.095993537, 0.050612699, -0.004408786], [1, -2.494956002, 2.017265875, -0.522189400]
        return sps.lfilter(b, a, white)
    if kind == 'fan':
        hum = np.sin(2 * np.pi * 100 * t) + 0.5 * np.sin(2 * np.pi * 200 * t + 1) + 0.25 * np.sin(2 * np.pi * 300 * t + 2)
        b, a = sps.butter(2, 800 / (RATE / 2))
        return hum + 2.0 * sps.lfilter(b, a, rng.normal(size=length))
    raise ValueError(kind)


def to_db(x, db):
    return x * (10 ** (db / 20) / (active_rms(x) + 1e-20))


def microphone(x, kind):
    """A cheap headset or laptop microphone: band limited, and sometimes the 300 Hz to 3.4 kHz telephone band."""
    if kind == 'flat':
        return x
    low, high = (300, 3400) if kind == 'phone' else (120, 6000)
    b, a = sps.butter(2, [low / (RATE / 2), high / (RATE / 2)], btype='band')
    return sps.lfilter(b, a, x)


def speed(x, factor):
    """Resample to change pitch and tempo together (a smaller or larger voice)."""
    if factor == 1.0:
        return x
    up, down = {0.9: (10, 9), 1.1: (10, 11), 0.85: (20, 17), 1.15: (20, 23)}[factor]
    return sps.resample_poly(x, up, down)


def scene(word, level_db, background, snr_db, rng, game_backgrounds, mic='flat', lead=1.2, tail=1.2):
    word = microphone(word, mic)
    word = to_db(word, level_db)
    length = len(word) + int((lead + tail) * RATE)
    if background in game_backgrounds:
        source = game_backgrounds[background]
        start = rng.integers(0, max(1, len(source) - length))
        bed = np.resize(source[start:start + length], length)
    else:
        bed = noise(background, length, rng)
    bed = to_db(bed, level_db - snr_db)
    out = bed.copy()
    at = int(lead * RATE)
    out[at:at + len(word)] += word
    return out


def write(path, x):
    sf.write(path, np.clip(x, -1, 1), RATE, subtype='PCM_16')


def laptop_fan(length, rng, mains=50):
    """A laptop's own noise at its microphone: the fan's broadband air noise (low-passed, slowly breathing), its blade
    tone and harmonics with a slow wobble, mains hum at 50 or 60 Hz and its harmonics, and a faint high coil whine."""
    t = np.arange(length) / RATE
    b, a = sps.butter(2, 2500 / (RATE / 2))
    air = sps.lfilter(b, a, rng.normal(size=length))
    air *= 1 + 0.3 * np.sin(2 * np.pi * rng.uniform(0.1, 0.4) * t + rng.uniform(0, 6))
    blade = rng.uniform(180, 420)
    wobble = 1 + 0.01 * np.sin(2 * np.pi * 0.3 * t)
    phase = 2 * np.pi * blade * np.cumsum(wobble) / RATE
    tone = sum(0.6 / k * np.sin(k * phase + rng.uniform(0, 6)) for k in (1, 2, 3, 4))
    hum = sum(0.5 / k * np.sin(2 * np.pi * mains * k * t + rng.uniform(0, 6)) for k in (1, 2, 3, 5, 7, 9))
    whine = 0.05 * np.sin(2 * np.pi * rng.uniform(5000, 7500) * t)
    out = air / (np.std(air) + 1e-12) + 0.5 * tone + 0.4 * hum + whine
    return out.astype(np.float32)


def cheap_microphone(x, rng):
    """A bad laptop microphone and Windows' capture path: 300 Hz to 4 kHz, an automatic gain that pulls every pause up
    to the speech level (fast attack 20 ms, slow 1 s release, up to +30 dB toward -20 dBFS), soft clipping and 10-bit
    samples."""
    b, a = sps.butter(2, [300 / (RATE / 2), 4000 / (RATE / 2)], btype='band')
    x = sps.lfilter(b, a, x)
    block = 160
    out = np.empty_like(x)
    gain_db = 0.0
    for i in range(0, len(x), block):
        seg = x[i:i + block]
        level = 10 * np.log10(np.mean(seg ** 2) + 1e-12)
        want = float(np.clip(-20 - level, -20, 30))
        # down fast (half the way each 10 ms), up at 30 dB a second
        gain_db = gain_db + 0.5 * (want - gain_db) if want < gain_db else min(want, gain_db + 0.3)
        out[i:i + block] = seg * 10 ** (gain_db / 20)
    out = np.tanh(out * 1.4) / np.tanh(1.4)
    return (np.round(out * 511) / 511).astype(np.float32)


def build_fan(args, rng):
    """The noisy laptop set: every SAPI clip (positives and other words) in laptop fan noise at 20, 10, 5 and 0 dB SNR
    (speech against noise, before the microphone), through cheap_microphone, plus fan noise alone (30 s each) at four
    levels for the VAD."""
    clips = tts(args.out / 'cache', args.out / 'cache' / 'tts')
    folder = args.out / 'clips'
    folder.mkdir(parents=True, exist_ok=True)
    cases = []
    for wav in clips:
        x = load(wav)
        positive = wav.stem.startswith('pos_')
        for snr in (20, 10, 5, 0):
            word = to_db(speed(x, float(rng.choice([0.9, 1.0, 1.1]))), float(rng.uniform(-45, -25)))
            length = len(word) + int(2.4 * RATE)
            bed = to_db(laptop_fan(length, rng, int(rng.choice([50, 60]))), 20 * np.log10(active_rms(word)) - snr)
            mix = bed.copy()
            at = int(1.2 * RATE)
            mix[at:at + len(word)] += word
            name = f'{wav.stem}_snr{snr}.wav'
            write(folder / name, cheap_microphone(mix, rng))
            cases.append(dict(name=name, group=('pos' if positive else 'neg') + f'_snr{snr:02d}', positive=positive))
    for i, level in enumerate((-60, -45, -35, -25)):
        name = f'fanonly_{-level}db.wav'
        write(folder / name, cheap_microphone(to_db(laptop_fan(30 * RATE, rng, (50, 60)[i % 2]), level), rng))
        cases.append(dict(name=name, group='neg_fan_only', positive=False))
    return cases


def build(args, rng):
    clips = tts(args.out / 'cache', args.out / 'cache' / 'tts')
    game = {}
    if args.game_audio:
        ga = Path(args.game_audio)
        for name, rel in (('radio', '../audio_game/radio.wav'), ('ambience', '../audio_game/bg_startroom_ingame.wav'),
                          ('hallway', '../audio_game/boot_to_hallway.wav')):
            if (ga / rel).exists():
                game[name] = load(ga / rel)
    cases = []
    folder = args.out / 'clips'
    folder.mkdir(parents=True, exist_ok=True)

    def add(name, group, positive, audio):
        path = folder / f'{name}.wav'
        write(path, audio)
        cases.append(dict(name=path.name, group=group, positive=positive))

    backgrounds = ['pink', 'white', 'fan'] + [k for k in ('ambience', 'hallway') if k in game]
    for wav in clips:
        x = load(wav)
        stem = wav.stem
        positive = stem.startswith('pos_')
        key = re.match(r'(pos|neg)_(.+)_(david|zira|mark|davidm|ziram)_r', stem)[2]
        # the plain clip, then variations: level down to -60 dBFS, noise 6 to 30 dB under the word, band limited
        # microphones and voices pitched up or down
        variants = [dict(level=-25, bg='pink', snr=40, mic='flat', speed=1.0)]
        for _ in range(args.variants if positive else max(1, args.variants // 2)):
            variants.append(dict(level=float(rng.choice([-20, -30, -40, -50, -55, -60])), bg=str(rng.choice(backgrounds)),
                                 snr=float(rng.choice([6, 10, 15, 20, 30])), mic=str(rng.choice(['flat', 'headset', 'phone'])),
                                 speed=float(rng.choice([0.85, 0.9, 1.0, 1.1, 1.15]))))
        for i, v in enumerate(variants):
            audio = scene(speed(x, v['speed']), v['level'], v['bg'], v['snr'], rng, game, v['mic'])
            group = ('pos_' if positive else 'neg_') + key
            add(f'{stem}_v{i}_{int(v["level"])}db_{v["bg"]}{int(v["snr"])}_{v["mic"]}_s{v["speed"]}', group, positive, audio)
    # the word over the game's radio, the way a microphone near the speakers hears it
    if 'radio' in game:
        jacks = [w for w in clips if re.match(r'pos_(jack|hey_jack)_', w.stem)]
        for i, wav in enumerate(jacks[:12]):
            add(f'radio_jack_{i}', 'pos_over_radio', True, scene(load(wav), -30, 'radio', [6, 10, 15][i % 3], rng, game))
    # negatives without the word: the game's own sounds, long recordings, noise at several levels
    if args.game_audio:
        ga = Path(args.game_audio)
        for wav in sorted(ga.glob('*/*.wav')):
            x = load(wav, seconds=60)
            if len(x) < RATE // 4 or active_rms(x) < 1e-5:
                continue
            level = float(rng.choice([-20, -30, -45]))
            pad = np.zeros(RATE // 2, dtype=np.float32)
            add(f'game_{wav.parent.name}_{wav.stem}', 'neg_game_' + wav.parent.name, False,
                np.concatenate([pad, to_db(x, level) + to_db(noise('pink', len(x), rng), level - 40), pad]))
        for name, x in game.items():
            for level in (-20, -40):
                add(f'game_long_{name}_{-level}', 'neg_game_long', False, to_db(x, level))
    for kind, level in itertools.product(['pink', 'white', 'fan'], [-70, -55, -40, -25]):
        add(f'noise_{kind}_{-level}', 'neg_noise', False, to_db(noise(kind, 20 * RATE, rng), level))
    add('silence', 'neg_noise', False, np.zeros(10 * RATE, dtype=np.float32))
    for wav in sorted(Path(args.fixtures).glob('*.wav')) if args.fixtures else []:
        positive = 'negative' not in wav.stem
        add(f'fixture_{wav.stem}', 'fixture_' + ('pos' if positive else 'neg'), positive, load(wav))
    return cases


def ipa(code, word='Jack'):
    return ph(code, word)


# renderings of "Jack" the SAPI voices can say that stand for accents and other speakers (2026-10-08): the zh onset of
# French or Turkish speakers, the open a of Spanish or Asian ones, the epenthetic vowel after the k (Japanese), the long
# vowel, a devoiced onset, a clipped word without its k, very slow, very fast, whispered or shouted
ACCENT_POSITIVE = {
    'ac_zhak': ipa('\u0292\u00e6k'), 'ac_zhak_a': ipa('\u0292ak'), 'ac_dzak_open': ipa('d\u0292\u0251k'),
    'ac_jakku': ipa('d\u0292\u00e6k\u028a'), 'ac_dzaak': ipa('d\u0292\u0251\u02d0k'), 
    'ac_dzek_long': ipa('d\u0292\u025b\u02d0k'), 'ac_clip': ipa('d\u0292\u00e6'), 'ac_jaaack': 'Jaaack',
    'ac_jek_slow': "<prosody rate='x-slow'>Jek</prosody>", 'ac_slow_low': "<prosody rate='x-slow' pitch='x-low'>Jack</prosody>",
    'ac_fast': "<prosody rate='x-fast'>Jack</prosody>", 'ac_whisper': "<prosody volume='x-soft' rate='slow'>Jack</prosody>",
    'ac_shout': "<prosody volume='x-loud'>Jack!</prosody>", 'ac_plain': 'Jack', 'ac_hey': 'Hey Jack',
}
# what the player of C:/Users/r1otp/Downloads/pt (10).log was heard saying, and other near sounds: all must stay rejected
ACCENT_NEGATIVE = {
    'pl_jorif_diga': 'Jorif diga', 'pl_jorith': 'Jorith', 'pl_shk_back': 'shk back', 'pl_possessed': 'Possessed, okay',
    'pl_jarus': 'JARUS!', 'pl_jourissa': 'Jourissa', 'pl_jrest': 'J r est', 'pl_james': 'James', 'pl_hey_you': 'Hey you',
    'pl_mmm': 'Mmm', 'pl_soutnich': 'Lee Soutnich possessed', 'pl_thats_jeff': "That's Jeff", 'pl_last_time': 'last time',
    'pl_jon': 'John', 'pl_josh': 'Josh', 'pl_job': 'Job',
}


def reverb(x, rng):
    """A small room: the dry word plus its tail through an exponentially decaying noise impulse (RT60 0.2 to 0.7 s)."""
    rt60 = rng.uniform(0.2, 0.7)
    t = np.arange(int(rt60 * RATE)) / RATE
    rir = rng.normal(size=len(t)) * np.exp(-6.9 * t / rt60)
    rir /= np.sqrt((rir ** 2).sum()) + 1e-9
    wet = sps.fftconvolve(x, rir)[:len(x)]
    mix = rng.uniform(0.3, 0.7)
    return ((1 - mix) * x + mix * wet * (np.std(x) / (np.std(wet) + 1e-9))).astype(np.float32)


def hard_clip(x, rng):
    """A microphone overdriven by 12 to 24 dB."""
    g = 10 ** (rng.uniform(12, 24) / 20)
    return np.clip(x * g / (np.abs(x).max() + 1e-9), -1, 1).astype(np.float32)


def build_accent(args, rng):
    """The accent and real-microphone set: ACCENT_POSITIVE (3 voices' rates, 4 variations each), the player's words
    (ACCENT_NEGATIVE) and the ordinary negatives under the same effects. Effects: reverb, a clipped microphone, low
    gain, headset or telephone band, noise under the word, a pitch/speed change."""
    cache = args.out / 'cache'
    pos = tts(cache, cache / 'tts', ACCENT_POSITIVE, ACCENT_NEGATIVE)
    ordinary = [w for w in tts(cache, cache / 'tts') if w.stem.startswith('neg_')]
    folder = args.out / 'clips'
    folder.mkdir(parents=True, exist_ok=True)
    game = {}
    if args.game_audio:
        ga = Path(args.game_audio)
        for name, rel in (('ambience', '../audio_game/bg_startroom_ingame.wav'), ('hallway', '../audio_game/boot_to_hallway.wav')):
            if (ga / rel).exists():
                game[name] = load(ga / rel)
    backgrounds = ['pink', 'white', 'fan'] + list(game)
    cases = []
    accent_keys = [k for k in list(ACCENT_POSITIVE) + list(ACCENT_NEGATIVE)]
    clips = [w for w in pos if any(f'_{k}_' in w.stem for k in accent_keys)]
    ordinary = ordinary[::6]
    for wav in clips + ordinary:
        positive = wav.stem.startswith('pos_')
        key = re.match(r'(pos|neg)_(.+)_(david|zira|mark|davidm|ziram)_r', wav.stem)[2]
        x = load(wav)
        for i in range(4 if positive else 2):
            fx = ['none', 'reverb', 'clip', 'lowgain'][i % 4]
            word = speed(x, float(rng.choice([0.85, 0.9, 1.0, 1.1, 1.15])))
            if fx == 'reverb':
                word = reverb(word, rng)
            elif fx == 'clip':
                word = hard_clip(word, rng)
            level = float(rng.choice([-55, -58, -62])) if fx == 'lowgain' else float(rng.choice([-20, -30, -40, -50]))
            bg = 'pink' if fx == 'lowgain' else str(rng.choice(backgrounds))
            mic = str(rng.choice(['flat', 'headset', 'phone']))
            audio = scene(word, level, bg, float(rng.choice([10, 15, 20, 30])), rng, game, mic)
            name = f'{wav.stem}_{fx}_{int(level)}db_{bg}_{mic}.wav'
            write(folder / name, audio)
            cases.append(dict(name=name, group=('pos_' if positive else 'neg_') + key, positive=positive))
    return cases


def run(args, cases):
    listing = args.out / f'list{args.tag}.txt'
    listing.write_text('\n'.join(str((args.out / 'clips' / c['name']).resolve()) for c in cases) + '\n', encoding='utf-8')
    work = args.out / ('run' + args.tag)
    work.mkdir(exist_ok=True)
    env = {**__import__('os').environ, 'SDL_AUDIO_DRIVER': 'dummy'}
    if args.cpu:
        env['PT_VOICE_CPU'] = args.cpu
    subprocess.run([str(Path(args.exe).resolve()), '--headless', '--voice-test', str(listing.resolve())], cwd=work, env=env,
                   timeout=7200, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    log = (work / 'pt.log').read_text(errors='replace')
    cpu = re.search(r'voice: whisper.cpp CPU code ggml-cpu-(\w+)', log)
    print(f'CPU code: ggml-cpu-{cpu[1] if cpu else "none"}')
    if args.cpu and (not cpu or cpu[1] != args.cpu):
        sys.exit(f'the recognizer did not run ggml-cpu-{args.cpu}')
    found = {}
    for m in re.finditer(r'voice test: file (\S+) \| (\d+) detections \| (\d+) utterances \| ([\d.]+) s \| (.*)', log):
        found[m[1]] = dict(detections=int(m[2]), utterances=int(m[3]), seconds=float(m[4]), heard=m[5].strip())
    return found, log


def report(cases, found, out, tag=''):
    groups = {}
    decode = []
    errors = []
    opened = []
    for c in cases:
        r = found.get(c['name'])
        if r is None:
            errors.append(f'missing result for {c["name"]}')
            continue
        if c['group'] == 'neg_fan_only':
            opened.append(f'{c["name"]}: {r["utterances"]} utterances {r["heard"]}')
        hit = r['detections'] > 0
        g = groups.setdefault(c['group'], [0, 0, c['positive']])
        g[0] += hit
        g[1] += 1
        decode += [float(v) for v in re.findall(r'\|(\d+)ms\]', r['heard'])]
        if hit != c['positive']:
            errors.append(f'{"MISS" if c["positive"] else "FALSE"} {c["name"]}: {r["heard"]}')
    pos = [v for v in groups.values() if v[2]]
    neg = [v for v in groups.values() if not v[2]]
    lines = ['group                          rate', '-' * 44]
    for name, (hit, total, positive) in sorted(groups.items()):
        lines.append(f'{name:28} {hit:4}/{total:<4} {"detected" if positive else "false accepts"}')
    p_hit, p_total = sum(v[0] for v in pos), sum(v[1] for v in pos)
    n_hit, n_total = sum(v[0] for v in neg), sum(v[1] for v in neg)
    lines.append('-' * 44)
    lines.append(f'detection   {p_hit}/{p_total} = {100 * p_hit / max(1, p_total):.1f}%')
    lines.append(f'false accept {n_hit}/{n_total} = {100 * n_hit / max(1, n_total):.2f}%')
    if decode:
        d = np.array(decode)
        lines.append(f'decode ms: median {np.median(d):.0f}, p90 {np.percentile(d, 90):.0f}, max {d.max():.0f} over {len(d)} utterances')
    if opened:
        lines += ['fan noise alone (segments the VAD opened):'] + ['  ' + o for o in opened]
    text = '\n'.join(lines + [''] + errors)
    (out / f'report{tag}.txt').write_text(text, encoding='utf-8')
    print(text)
    return p_hit, p_total, n_hit, n_total


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--exe', required=True)
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--game-audio', help='the audio dump folder with radio and voice wavs (dump/audio)')
    p.add_argument('--fixtures', help='a folder of recorded wavs; names without "negative" hold the word')
    p.add_argument('--variants', type=int, default=3)
    p.add_argument('--seed', type=int, default=7)
    p.add_argument('--reuse', action='store_true', help='run on the clips of a previous build of the set')
    p.add_argument('--cpu', help='force a ggml CPU variant (x64 is SSE2 only)')
    p.add_argument('--limit', type=int, default=1, help='keep every Nth clip')
    p.add_argument('--tag', default='', help='suffix of the run folder and report, to run several configurations on one set')
    p.add_argument('--accent', action='store_true', help='the accent and real-microphone set (build_accent)')
    p.add_argument('--fan', action='store_true', help='the noisy laptop set (build_fan) instead of the main one')
    args = p.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(args.seed)
    cases_file = args.out / 'cases.json'
    if args.reuse and cases_file.exists():
        cases = json.loads(cases_file.read_text())
    else:
        cases = build_fan(args, rng) if args.fan else build_accent(args, rng) if args.accent else build(args, rng)
        cases_file.write_text(json.dumps(cases, indent=1))
    cases = cases[::max(1, args.limit)]
    found, _ = run(args, cases)
    report(cases, found, args.out, args.tag)


if __name__ == '__main__':
    sys.exit(main())
