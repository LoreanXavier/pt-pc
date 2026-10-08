#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine/audio/dsp.h"
#include "engine/audio/sound_package.h"
#include "engine/audio/sound_system.h"
#include "engine/audio/subtitles.h"
#include "engine/audio/wem.h"
#include "engine/core/log.h"
#include "engine/fs/vfs.h"
#include "game/game_sound.h"

namespace fs = std::filesystem;
using namespace pt::audio;

namespace {

struct Options {
    fs::path game = "game/CUSA01127";
    fs::path dump = "dump/audio";
    fs::path out_dir = "dump/audio_test";
    std::string mode;
    std::string argument;
    std::string event;
    fs::path out;
    fs::path motion;
    float seconds = 0.0f;
    bool has_position = false;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float forward[3] = {0.0f, 0.0f, 1.0f};
    float listener[9] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f};
    fs::path listener_path;
    std::vector<AuxSendLevel> aux;
    std::vector<std::pair<std::string, std::string>> states;
    std::vector<std::pair<std::string, std::string>> switches;
    std::vector<std::pair<std::string, float>> rtpcs;
    std::vector<std::pair<float, std::string>> timeline;
    uint32_t seed = 1;
    std::string language = "Eng";
    int repeat = 1;
    std::vector<std::string> arguments;
};

struct WavData {
    uint32_t channels = 0;
    uint32_t sample_rate = 0;
    std::vector<int16_t> samples;
};

bool ReadWav16(const fs::path& path, WavData& wav) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (data.size() < 12 || std::memcmp(data.data(), "RIFF", 4) != 0) {
        return false;
    }
    size_t pos = 12;
    bool have_fmt = false;
    while (pos + 8 <= data.size()) {
        uint32_t size;
        std::memcpy(&size, data.data() + pos + 4, 4);
        const uint8_t* body = data.data() + pos + 8;
        if (std::memcmp(data.data() + pos, "fmt ", 4) == 0) {
            uint16_t format;
            uint16_t channels;
            uint16_t bits;
            std::memcpy(&format, body, 2);
            std::memcpy(&channels, body + 2, 2);
            std::memcpy(&wav.sample_rate, body + 4, 4);
            std::memcpy(&bits, body + 14, 2);
            if (bits != 16) {
                return false;
            }
            wav.channels = channels;
            have_fmt = true;
        } else if (std::memcmp(data.data() + pos, "data", 4) == 0 && have_fmt) {
            const size_t bytes = std::min<size_t>(size, data.size() - pos - 8);
            wav.samples.resize(bytes / 2);
            std::memcpy(wav.samples.data(), body, wav.samples.size() * 2);
            return true;
        }
        pos += 8 + size + (size & 1);
    }
    return false;
}

bool WriteWavFloat(const fs::path& path, const float* samples, size_t frames, uint32_t channels, uint32_t rate) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    const uint32_t data_bytes = static_cast<uint32_t>(frames * channels * 2);
    auto put32 = [&](uint32_t v) { file.write(reinterpret_cast<const char*>(&v), 4); };
    auto put16 = [&](uint16_t v) { file.write(reinterpret_cast<const char*>(&v), 2); };
    file.write("RIFF", 4);
    put32(36 + data_bytes);
    file.write("WAVEfmt ", 8);
    put32(16);
    put16(1);
    put16(static_cast<uint16_t>(channels));
    put32(rate);
    put32(rate * channels * 2);
    put16(static_cast<uint16_t>(channels * 2));
    put16(16);
    file.write("data", 4);
    put32(data_bytes);
    std::vector<int16_t> pcm(frames * channels);
    for (size_t i = 0; i < pcm.size(); ++i) {
        const float v = std::clamp(samples[i], -1.0f, 1.0f);
        pcm[i] = static_cast<int16_t>(std::lround(v * 32767.0f));
    }
    file.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(pcm.size() * 2));
    return static_cast<bool>(file);
}

struct CompareResult {
    std::string key;
    std::string codec;
    uint32_t channels = 0;
    uint64_t frames = 0;
    uint64_t reference_frames = 0;
    int max_error = -1;
    double rms_error = 0.0;
    uint64_t mismatches = 0;
    uint64_t samples = 0;
    std::string error;
};

int QuantizeError(float value, int16_t reference, bool float_codec) {
    const long q = float_codec ? static_cast<long>(value * 32767.0f) : std::lround(static_cast<double>(value) * 32768.0);
    return static_cast<int>(std::abs(std::clamp<long>(q, -32768, 32767) - reference));
}

CompareResult CompareMedia(const std::string& key, const Media& media, const fs::path& wav_path) {
    CompareResult result;
    result.key = key;
    result.codec = media.CodecName();
    result.channels = media.Channels();
    result.frames = media.Frames();
    WavData wav;
    if (!ReadWav16(wav_path, wav)) {
        result.error = "reference WAV missing";
        return result;
    }
    if (wav.channels != media.Channels()) {
        result.error = "channel count differs";
        return result;
    }
    result.reference_frames = wav.samples.size() / wav.channels;
    const auto decoded = media.DecodeAll();
    const size_t frames = std::min<size_t>(decoded.size() / media.Channels(), result.reference_frames);
    const bool float_codec = media.Info().codec_tag == wem_codec::WwiseVorbis;
    int max_error = 0;
    double sum = 0.0;
    for (size_t i = 0; i < frames * media.Channels(); ++i) {
        const int e = QuantizeError(decoded[i], wav.samples[i], float_codec);
        max_error = std::max(max_error, e);
        sum += static_cast<double>(e) * e;
        result.mismatches += e != 0 ? 1 : 0;
    }
    const uint64_t total = decoded.size() / media.Channels();
    if (total > 8192) {
        auto reader = media.OpenReader();
        std::vector<float> chunk(4096ull * media.Channels());
        for (uint64_t target : {total / 3 + 17, total / 2 + 1, total - 3000, uint64_t{1}}) {
            reader->Seek(target);
            const uint32_t got = reader->Read(chunk.data(), 4096);
            const uint64_t expected = std::min<uint64_t>(4096, total - target);
            if (got != expected || std::memcmp(chunk.data(), decoded.data() + target * media.Channels(),
                                               sizeof(float) * got * media.Channels()) != 0) {
                result.error = std::format("seek to {} does not reproduce the decode", target);
                break;
            }
        }
    }
    result.max_error = max_error;
    result.samples = frames * media.Channels();
    result.rms_error = frames ? std::sqrt(sum / static_cast<double>(frames * media.Channels())) : 0.0;
    if (decoded.size() / media.Channels() != result.reference_frames) {
        result.error = std::format("frame count {} vs reference {}", decoded.size() / media.Channels(), result.reference_frames);
    }
    return result;
}

struct DemoStreamRef {
    const char* asset;
    const char* reference;
};

const DemoStreamRef kDemoStreams[] = {
    {"/Assets/sh/demo/demo_stream/gc_p00_020.fsm", "demo_stream/gc_p00_020.wav"},
    {"/Assets/sh/demo/demo_stream/gc_p00_030.fsm", "demo_stream/gc_p00_030.wav"},
    {"/Assets/sh/demo/demo_stream/gc_p02_100.fsm", "demo_stream/gc_p02_100.wav"},
    {"/Assets/sh/demo/demo_stream/gc_p07_030.fsm", "demo_stream/gc_p07_030.wav"},
    {"/Assets/sh/demo/demo_stream/#Eng/gc_p06_010_final.fsm", "demo_stream/gc_p06_010_final_Eng.wav"},
};

constexpr const char* kResidentPackage = "/Assets/sh/level/common/resident.fpk";

int RunDecodeAll(const Options& options) {
    pt::Vfs vfs;
    if (!vfs.Mount(options.game)) {
        return 1;
    }
    const auto load_start = std::chrono::steady_clock::now();
    SoundBankSet banks;
    std::string error;
    if (!banks.Load(vfs, SoundBankSet::DefaultPackages(), &error)) {
        std::printf("bank load failed: %s\n", error.c_str());
        return 1;
    }
    const double load_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - load_start).count();
    std::printf("loaded %zu banks, %zu media in %.2f s, %zu media errors\n", banks.Banks().size(), banks.AllMedia().size(), load_seconds,
                banks.MediaErrors().size());

    struct Job {
        std::string key;
        std::shared_ptr<const Media> media;
        fs::path wav;
    };
    std::vector<Job> jobs;
    for (const auto& [id, media] : banks.AllMedia()) {
        const std::string& bank = banks.MediaBank().at(id);
        jobs.push_back({bank + "/" + std::to_string(id), media, options.dump / bank / (std::to_string(id) + ".wav")});
    }
    vfs.LoadPackage(kResidentPackage);
    std::vector<std::shared_ptr<const std::vector<uint8_t>>> stream_storage;
    for (const auto& stream : kDemoStreams) {
        auto fsm = vfs.ReadFile(stream.asset);
        if (!fsm) {
            std::printf("missing %s\n", stream.asset);
            continue;
        }
        auto audio = ExtractDemoStreamAudio(*fsm);
        if (!audio) {
            std::printf("no audio in %s\n", stream.asset);
            continue;
        }
        auto storage = std::make_shared<const std::vector<uint8_t>>(std::move(audio->wem));
        auto media = Media::Create(0, storage, 0, storage->size(), &error);
        if (!media) {
            std::printf("%s: %s\n", stream.asset, error.c_str());
            continue;
        }
        stream_storage.push_back(storage);
        jobs.push_back({std::string("demo_stream/") + fs::path(stream.reference).stem().string(), media, options.dump / stream.reference});
    }
    std::sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return a.key < b.key; });

    std::vector<CompareResult> results(jobs.size());
    std::atomic<size_t> next{0};
    const auto decode_start = std::chrono::steady_clock::now();
    std::vector<std::thread> workers;
    const unsigned thread_count = std::max(1u, std::thread::hardware_concurrency());
    for (unsigned t = 0; t < thread_count; ++t) {
        workers.emplace_back([&] {
            for (size_t i = next++; i < jobs.size(); i = next++) {
                results[i] = CompareMedia(jobs[i].key, *jobs[i].media, jobs[i].wav);
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }
    const double decode_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - decode_start).count();

    int worst = 0;
    int failures = 0;
    std::map<std::string, int> worst_by_codec;
    std::map<std::string, int> count_by_codec;
    std::map<std::string, uint64_t> mismatches_by_codec;
    std::map<std::string, uint64_t> samples_by_codec;
    for (const auto& r : results) {
        mismatches_by_codec[r.codec] += r.mismatches;
        samples_by_codec[r.codec] += r.samples;
        const bool failed = r.max_error < 0 || !r.error.empty();
        if (failed || r.max_error > 2) {
            std::printf("%-28s %-9s ch %u frames %8llu ref %8llu max %6d rms %.3f %s\n", r.key.c_str(), r.codec.c_str(), r.channels,
                        static_cast<unsigned long long>(r.frames), static_cast<unsigned long long>(r.reference_frames), r.max_error,
                        r.rms_error, r.error.c_str());
        }
        failures += failed ? 1 : 0;
        worst = std::max(worst, r.max_error);
        worst_by_codec[r.codec] = std::max(worst_by_codec[r.codec], r.max_error);
        ++count_by_codec[r.codec];
    }
    for (const auto& [codec, value] : worst_by_codec) {
        std::printf("%-10s %4d media, max sample error %d (16-bit LSB), %llu of %llu samples differ\n", codec.c_str(), count_by_codec[codec],
                    value, static_cast<unsigned long long>(mismatches_by_codec[codec]),
                    static_cast<unsigned long long>(samples_by_codec[codec]));
    }
    std::printf("decoded %zu media in %.1f s: %d failures, max sample error %d (16-bit LSB)\n", results.size(), decode_seconds, failures,
                worst);
    return failures == 0 ? 0 : 1;
}

uint32_t NameOrId(std::string_view text) {
    if (!text.empty() && text[0] == '#') {
        return static_cast<uint32_t>(std::stoul(std::string(text.substr(1)), nullptr, 0));
    }
    return Fnv1Hash32(text);
}

double Db(double value) {
    return value > 1e-10 ? 20.0 * std::log10(value) : -200.0;
}

struct Analysis {
    double peak = 0.0;
    double rms = 0.0;
    double first_sound = -1.0;
    double last_sound = -1.0;
    std::vector<double> rms_per_second;
};

Analysis Analyze(const std::vector<float>& samples) {
    Analysis a;
    const size_t frames = samples.size() / 2;
    double sum = 0.0;
    double second_sum = 0.0;
    size_t second_count = 0;
    for (size_t i = 0; i < frames; ++i) {
        const double l = samples[i * 2];
        const double r = samples[i * 2 + 1];
        const double peak = std::max(std::fabs(l), std::fabs(r));
        a.peak = std::max(a.peak, peak);
        if (peak > 1e-4) {
            if (a.first_sound < 0.0) {
                a.first_sound = static_cast<double>(i) / kOutputRate;
            }
            a.last_sound = static_cast<double>(i) / kOutputRate;
        }
        sum += l * l + r * r;
        second_sum += l * l + r * r;
        if (++second_count == kOutputRate) {
            a.rms_per_second.push_back(std::sqrt(second_sum / (2.0 * second_count)));
            second_sum = 0.0;
            second_count = 0;
        }
    }
    if (second_count > kOutputRate / 10) {
        a.rms_per_second.push_back(std::sqrt(second_sum / (2.0 * second_count)));
    }
    a.rms = frames ? std::sqrt(sum / (2.0 * frames)) : 0.0;
    return a;
}

void PrintAnalysis(const Analysis& a, double seconds) {
    std::printf("rendered %.2f s: peak %.2f dBFS, rms %.2f dBFS, first sound %.3f s, last sound %.3f s\n", seconds, Db(a.peak), Db(a.rms),
                a.first_sound, a.last_sound);
    std::printf("rms per second (dBFS):");
    for (size_t i = 0; i < a.rms_per_second.size(); ++i) {
        std::printf("%s%.1f", i % 10 == 0 ? (i ? "\n  " : " ") : " ", Db(a.rms_per_second[i]));
    }
    std::printf("\n");
}

struct Marker {
    double time = 0.0;
    PlayingId id = 0;
    std::string label;
};

int RunEvent(const Options& options) {
    pt::Vfs vfs;
    if (!vfs.Mount(options.game)) {
        return 1;
    }
    SoundSystem sound;
    if (!sound.Init(vfs, false)) {
        return 1;
    }
    sound.SetRandomSeed(options.seed);
    const bool have_subtitles = sound.LoadSubtitles(options.language);
    constexpr GameObjectId kObject = 1;
    sound.RegisterObject(kObject, "test");
    const float* l = options.listener;
    sound.SetListener(glm::vec3(l[0], l[1], l[2]), glm::vec3(l[3], l[4], l[5]), glm::vec3(l[6], l[7], l[8]));
    if (options.has_position) {
        sound.SetObjectTransform(kObject, glm::vec3(options.position[0], options.position[1], options.position[2]),
                                 glm::vec3(options.forward[0], options.forward[1], options.forward[2]));
    }
    if (!options.aux.empty()) {
        sound.SetObjectAuxSends(0, options.aux);
        sound.SetObjectAuxSends(kObject, options.aux);
    }
    for (const auto& [group, state] : options.states) {
        sound.SetStateId(NameOrId(group), state == "None" ? 0 : NameOrId(state));
    }
    for (const auto& [group, value] : options.switches) {
        sound.SetSwitchId(NameOrId(group), NameOrId(value), kObject);
    }
    for (const auto& [name, value] : options.rtpcs) {
        sound.SetRtpcId(NameOrId(name), value, 0);
    }
    std::vector<std::array<float, 10>> poses;
    if (!options.listener_path.empty()) {
        std::ifstream file(options.listener_path);
        std::array<float, 10> pose{};
        while (file >> pose[0] >> pose[1] >> pose[2] >> pose[3] >> pose[4] >> pose[5] >> pose[6] >> pose[7] >> pose[8] >> pose[9]) {
            poses.push_back(pose);
        }
        std::sort(poses.begin(), poses.end(), [](const auto& a, const auto& b) { return a[0] < b[0]; });
        std::printf("listener path: %zu poses from %.3f s to %.3f s\n", poses.size(), poses.empty() ? 0.0 : poses.front()[0],
                    poses.empty() ? 0.0 : poses.back()[0]);
    }
    size_t next_pose = 0;
    double now = 0.0;
    std::vector<Marker> markers;
    std::vector<std::pair<double, PlayingId>> ends;
    sound.SetMarkerCallback([&](PlayingId id, std::string_view label) { markers.push_back({now, id, std::string(label)}); });
    sound.SetEndCallback([&](PlayingId id) { ends.push_back({now, id}); });
    PlayingId id = 0;
    if (!options.arguments.empty()) {
        std::vector<std::string_view> arguments(options.arguments.begin(), options.arguments.end());
        id = sound.PostDialogueEventId(NameOrId(options.event), arguments, kObject);
    } else {
        for (int i = 0; i < options.repeat; ++i) {
            const PlayingId posted = sound.PostEventId(NameOrId(options.event), kObject);
            id = id ? id : posted;
        }
    }
    if (!id) {
        std::printf("event %s not found\n", options.event.c_str());
        return 1;
    }
    std::printf("posted %s (%08X) as playing id %u (%d times)\n", options.event.c_str(), NameOrId(options.event), id, options.repeat);
    uint32_t max_voices = 0;
    auto timeline = options.timeline;
    std::sort(timeline.begin(), timeline.end());
    size_t next_entry = 0;
    constexpr uint32_t kChunk = 480;
    const uint64_t max_frames = static_cast<uint64_t>((options.seconds > 0.0f ? options.seconds : 600.0f) * kOutputRate);
    std::vector<float> samples;
    std::vector<float> chunk(kChunk * 2);
    std::vector<std::pair<double, MotionLevels>> motion;
    uint64_t frames = 0;
    uint64_t silent_after_end = 0;
    while (frames < max_frames) {
        now = static_cast<double>(frames) / kOutputRate;
        if (next_pose < poses.size() && poses[next_pose][0] <= now + 1e-9) {
            while (next_pose + 1 < poses.size() && poses[next_pose + 1][0] <= now + 1e-9) {
                ++next_pose;
            }
            const auto& p = poses[next_pose++];
            sound.SetListener(glm::vec3(p[1], p[2], p[3]), glm::vec3(p[4], p[5], p[6]), glm::vec3(p[7], p[8], p[9]));
        }
        while (next_entry < timeline.size() && timeline[next_entry].first <= now + 1e-9) {
            const std::string& what = timeline[next_entry].second;
            if (what == "stopall") {
                sound.StopAll(0.0f);
                std::printf("%8.3f s  StopAll\n", now);
            } else if (what.rfind("state:", 0) == 0) {
                const size_t eq = what.find('=');
                sound.SetStateId(NameOrId(what.substr(6, eq - 6)), NameOrId(what.substr(eq + 1)));
                std::printf("%8.3f s  %s\n", now, what.c_str());
            } else {
                const PlayingId extra = sound.PostEventId(NameOrId(what), kObject);
                std::printf("%8.3f s  posted %s as %u\n", now, what.c_str(), extra);
            }
            ++next_entry;
        }
        sound.RenderOffline(chunk.data(), kChunk);
        sound.Update(static_cast<float>(kChunk) / kOutputRate);
        const MotionLevels levels = sound.Motion();
        motion.push_back({now, levels});
        max_voices = std::max(max_voices, sound.Stats().voices);
        samples.insert(samples.end(), chunk.begin(), chunk.end());
        frames += kChunk;
        if (options.seconds <= 0.0f && next_entry >= timeline.size() && !sound.IsPlaying(id)) {
            silent_after_end += kChunk;
            if (silent_after_end >= kOutputRate / 2) {
                break;
            }
        }
    }
    for (const auto& marker : markers) {
        std::printf("%8.3f s  marker '%s' (playing id %u)", marker.time, marker.label.c_str(), marker.id);
        if (have_subtitles) {
            const std::string subtitle = sound.Subtitles().SubtitleIdForMarker(marker.label);
            if (!subtitle.empty()) {
                const SubtitleEntry* entry = sound.Subtitles().FindById(subtitle);
                std::printf(" -> subtitle %s", subtitle.c_str());
                if (entry && !entry->lines.empty()) {
                    std::printf(", %zu lines, first at %.2f s: \"%s\"", entry->lines.size(), entry->lines.front().start_seconds,
                                entry->lines.front().text.c_str());
                }
            }
        }
        std::printf("\n");
    }
    for (const auto& [time, ended] : ends) {
        std::printf("%8.3f s  playing id %u ended\n", time, ended);
    }
    const auto stats = sound.Stats();
    std::printf("voices at end: %u (virtual %u), playing ids %u, most voices at once %u\n", stats.voices, stats.virtual_voices,
                stats.playing_ids, max_voices);
    PrintAnalysis(Analyze(samples), static_cast<double>(frames) / kOutputRate);
    uint8_t peak_large = 0;
    uint8_t peak_small = 0;
    double first_motion = -1.0;
    double last_motion = -1.0;
    for (const auto& [time, levels] : motion) {
        peak_large = std::max(peak_large, levels.large_motor);
        peak_small = std::max(peak_small, levels.small_motor);
        if (levels.large_motor || levels.small_motor) {
            first_motion = first_motion < 0.0 ? time : first_motion;
            last_motion = time;
        }
    }
    if (first_motion >= 0.0) {
        std::printf("vibration: large motor peak %u, small motor peak %u, from %.2f s to %.2f s\n", peak_large, peak_small, first_motion,
                    last_motion);
    } else {
        std::printf("vibration: none\n");
    }
    if (!options.motion.empty()) {
        std::ofstream csv(options.motion);
        csv << "seconds,large,small\n";
        for (const auto& [time, levels] : motion) {
            csv << std::format("{:.3f},{},{}\n", time, levels.large_motor, levels.small_motor);
        }
        std::printf("wrote %s\n", options.motion.string().c_str());
    }
    fs::path out = options.out.empty() ? options.out_dir / (options.event + ".wav") : options.out;
    if (!WriteWavFloat(out, samples.data(), samples.size() / 2, 2, kOutputRate)) {
        std::printf("cannot write %s\n", out.string().c_str());
        return 1;
    }
    std::printf("wrote %s\n", out.string().c_str());
    return 0;
}

int RunDemoStream(const Options& options) {
    pt::Vfs vfs;
    if (!vfs.Mount(options.game)) {
        return 1;
    }
    vfs.LoadPackage(kResidentPackage);
    auto fsm = vfs.ReadFile(options.argument);
    if (!fsm) {
        std::printf("%s not found\n", options.argument.c_str());
        return 1;
    }
    auto audio = ExtractDemoStreamAudio(*fsm);
    if (!audio) {
        std::printf("%s carries no sound stream\n", options.argument.c_str());
        return 1;
    }
    std::printf("%s: %zu byte wem, stream starts at %.3f s, demo ends at %.3f s\n", options.argument.c_str(), audio->wem.size(),
                audio->start_time, audio->end_time);
    auto storage = std::make_shared<const std::vector<uint8_t>>(audio->wem);
    std::string error;
    auto media = Media::Create(0, storage, 0, storage->size(), &error);
    if (!media) {
        std::printf("stream: %s\n", error.c_str());
        return 1;
    }
    SoundSystem sound;
    if (!sound.Init(vfs, false)) {
        return 1;
    }
    const PlayingId id = sound.PlayStream(audio->wem, 0);
    if (!id) {
        return 1;
    }
    std::vector<float> samples;
    std::vector<float> chunk(480 * 2);
    uint64_t frames = 0;
    uint64_t tail = 0;
    const uint64_t max_frames = static_cast<uint64_t>((options.seconds > 0.0f ? options.seconds : 600.0f) * kOutputRate);
    while (frames < max_frames) {
        sound.RenderOffline(chunk.data(), 480);
        sound.Update(0.01f);
        samples.insert(samples.end(), chunk.begin(), chunk.end());
        frames += 480;
        if (!sound.IsPlaying(id) && (tail += 480) >= kOutputRate / 2) {
            break;
        }
    }
    PrintAnalysis(Analyze(samples), static_cast<double>(frames) / kOutputRate);
    const auto decoded = media->DecodeAll();
    const uint32_t channels = media->Channels();
    const float c = 0.70710678f;
    const float gain =
        FastPow10(0.0f) * FastPow10(0.0f) * FastPow10(0.0f) * FastPow10(-6.0f * 0.05f) * FastPow10(3.0f * 0.05f) * FastPow10(0.0f);
    const size_t lookahead = 480;
    double max_diff = 0.0;
    double sum_diff = 0.0;
    double sum_ref = 0.0;
    double ref_peak = 0.0;
    size_t compared = 0;
    for (size_t i = 0; i < decoded.size() / channels && i + lookahead < samples.size() / 2; ++i) {
        const float* f = decoded.data() + i * channels;
        float l = 0.0f;
        float r = 0.0f;
        if (channels == 8) {
            l = f[0] + c * f[2] + 0.5f * f[3] + c * f[4] + c * f[6];
            r = f[1] + c * f[2] + 0.5f * f[3] + c * f[5] + c * f[7];
        } else if (channels == 2) {
            l = f[0];
            r = f[1];
        } else {
            l = r = f[0] * c;
        }
        l = std::clamp(l * gain, -1.0f, 1.0f);
        r = std::clamp(r * gain, -1.0f, 1.0f);
        ref_peak = std::max({ref_peak, static_cast<double>(std::fabs(l)), static_cast<double>(std::fabs(r))});
        const double dl = samples[(i + lookahead) * 2] - l;
        const double dr = samples[(i + lookahead) * 2 + 1] - r;
        max_diff = std::max({max_diff, std::fabs(dl), std::fabs(dr)});
        sum_diff += dl * dl + dr * dr;
        sum_ref += static_cast<double>(l) * l + static_cast<double>(r) * r;
        ++compared;
    }
    std::printf("compared %zu frames with the direct downmix times %.6f (%.3f dB: voice, make-up, output bus, bus -6 dB, master +3 dB "
                "and the limiter through the eboot's fast 10^x; lookahead 10 ms): max difference %.5f, error %.1f dB below signal\n",
                compared, gain, Db(gain), max_diff, sum_ref > 0.0 ? -10.0 * std::log10(std::max(sum_diff, 1e-30) / sum_ref) : 0.0);
    std::printf("reference peak %.2f dBFS (the master limiter starts at -1.5 dBFS)\n", Db(ref_peak));
    fs::path out = options.out.empty() ? options.out_dir / (fs::path(options.argument).stem().string() + "_stream.wav") : options.out;
    if (!WriteWavFloat(out, samples.data(), samples.size() / 2, 2, kOutputRate)) {
        return 1;
    }
    std::printf("wrote %s\n", out.string().c_str());
    return 0;
}

int RunSubtitles(const Options& options) {
    pt::Vfs vfs;
    if (!vfs.Mount(options.game)) {
        return 1;
    }
    SoundBankSet banks;
    std::string error;
    if (!banks.Load(vfs, SoundBankSet::DefaultPackages(), &error)) {
        std::printf("bank load failed: %s\n", error.c_str());
        return 1;
    }
    SubtitleTable table;
    if (!table.Load(vfs, options.argument, banks.SabTables(), &error)) {
        std::printf("%s\n", error.c_str());
        return 1;
    }
    std::printf("language %s: %zu entries, %zu marker links\n", table.Language().c_str(), table.Entries().size(), table.MarkerLinks().size());
    for (const auto& entry : table.Entries()) {
        std::printf("%08X %s\n", entry.key, entry.id.empty() ? "(no sab link)" : entry.id.c_str());
        for (const auto& line : entry.lines) {
            std::string text = line.text;
            std::replace(text.begin(), text.end(), '\n', '|');
            std::printf("    %7.2f %7.2f  %s\n", line.start_seconds, line.end_seconds, text.c_str());
        }
    }
    int linked = 0;
    int labels = 0;
    for (const auto& [id, media] : banks.AllMedia()) {
        for (const auto& marker : media->Info().markers) {
            if (marker.label.empty()) {
                continue;
            }
            ++labels;
            const std::string subtitle = table.SubtitleIdForMarker(marker.label);
            if (!subtitle.empty()) {
                ++linked;
                const SubtitleEntry* entry = table.FindById(subtitle);
                std::printf("marker %-28s media %10u -> %-16s %s\n", marker.label.c_str(), id, subtitle.c_str(),
                            entry ? "found" : "missing in subp");
            }
        }
    }
    std::printf("%d of %d labelled markers link to a subtitle\n", linked, labels);
    return 0;
}

int RunFxImpulse(const Options& options) {
    pt::Vfs vfs;
    if (!vfs.Mount(options.game)) {
        return 1;
    }
    SoundBankSet banks;
    std::string error;
    if (!banks.Load(vfs, SoundBankSet::DefaultPackages(), &error)) {
        std::printf("bank load failed: %s\n", error.c_str());
        return 1;
    }
    for (const auto& bank : banks.Banks()) {
        for (const auto& object : bank->Objects()) {
            if (object->type != HircType::FxCustom && object->type != HircType::FxShareSet) {
                continue;
            }
            const auto& fx = *static_cast<const FxObject*>(object.get());
            auto effect = CreateEffect(fx);
            if (!effect) {
                continue;
            }
            const uint32_t frames = kOutputRate * 6;
            std::vector<float> left(frames, 0.0f);
            std::vector<float> right(frames, 0.0f);
            left[0] = 1.0f;
            right[0] = 1.0f;
            for (uint32_t offset = 0; offset < frames; offset += 256) {
                effect->Process(left.data() + offset, right.data() + offset, std::min<uint32_t>(256, frames - offset));
            }
            double total = 0.0;
            std::printf("fx %u plugin %08X tail %.2f s, energy per 250 ms (dB):", fx.id, fx.plugin_id, effect->TailSeconds());
            for (uint32_t window = 0; window < frames; window += kOutputRate / 4) {
                double sum = 0.0;
                for (uint32_t i = window; i < std::min(frames, window + kOutputRate / 4); ++i) {
                    sum += left[i] * left[i] + right[i] * right[i];
                }
                total += sum;
                std::printf(" %.1f", 10.0 * std::log10(std::max(sum / 2.0, 1e-30)));
            }
            std::printf("\n    total energy %.2f dB relative to the impulse, peak %.3f\n", 10.0 * std::log10(std::max(total / 2.0, 1e-30)),
                        std::max(*std::max_element(left.begin(), left.end()), *std::max_element(right.begin(), right.end())));
        }
    }
    return 0;
}

int RunThreadStress(const Options& options) {
    pt::Vfs vfs;
    if (!vfs.Mount(options.game)) {
        return 1;
    }
    SoundSystem sound;
    if (!sound.Init(vfs, false)) {
        return 1;
    }
    sound.SetRandomSeed(options.seed);
    std::atomic<bool> running{true};
    std::atomic<uint64_t> rendered{0};
    std::thread renderer([&] {
        std::vector<float> chunk(512 * 2);
        while (running.load()) {
            sound.RenderOffline(chunk.data(), 512);
            rendered += 512;
        }
    });
    const char* events[] = {"Play_plr_footstep_wk_l", "Play_sfx_clock_bell", "Play_radio_f010", "Stop_radio_f010", "Play_sfx_heartbeat",
                            "Play_bgm_corridor_f110", "Stop_bgm_corridor_f110", "Set_state_in_game", "Play_sfx_door_open_01",
                            "Play_sfx_rainfall_01", "Set_rtpc_rainfall_l_fade", "Pause_All", "Resume_All", "Play_tel_10_call"};
    uint32_t state = options.seed * 2654435761u + 1;
    auto next = [&] {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    int posted = 0;
    int markers = 0;
    sound.SetMarkerCallback([&](PlayingId, std::string_view) { ++markers; });
    const auto start = std::chrono::steady_clock::now();
    for (int step = 0; step < 20000; ++step) {
        const GameObjectId object = 1 + next() % 16;
        switch (next() % 8) {
            case 0:
            case 1:
            case 2:
                posted += sound.PostEvent(events[next() % std::size(events)], object) ? 1 : 0;
                break;
            case 3:
                sound.SetObjectTransform(object, glm::vec3(static_cast<float>(next() % 60) - 30.0f, 0.0f, static_cast<float>(next() % 60) - 30.0f),
                                         glm::vec3(0.0f, 0.0f, 1.0f));
                break;
            case 4:
                sound.SetSwitch("Material", (next() & 1) ? "wood" : "tile", object);
                sound.SetRtpc("rainfall", static_cast<float>(next() % 100) / 100.0f);
                break;
            case 5:
                if (next() % 50 == 0) {
                    sound.UnregisterObject(object);
                } else {
                    sound.IsEventPlaying("Play_radio_f010");
                }
                break;
            case 6:
                sound.Update(0.001f);
                break;
            default:
                if (next() % 200 == 0) {
                    sound.StopAll(0.1f);
                }
                break;
        }
        if (step % 64 == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }
    running = false;
    renderer.join();
    sound.Update(0.0f);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const auto stats = sound.Stats();
    std::printf("thread stress: %d posts, %d markers, %.1f s of audio rendered in %.2f s wall, %u voices at end\n", posted, markers,
                static_cast<double>(rendered.load()) / kOutputRate, seconds, stats.voices);
    return 0;
}

bool ReachesAudio(const SoundBankSet& banks, uint32_t id, int depth) {
    const HircObject* object = banks.Find(id);
    if (!object || depth > 32) {
        return false;
    }
    switch (object->type) {
        case HircType::Sound: {
            const auto& sound = *static_cast<const SoundObject*>(object);
            const uint32_t plugin = sound.source.plugin_id;
            if (plugin == codec::ToneGenerator) {
                return true;
            }
            auto media = banks.FindMedia(sound.source.source_id);
            return media != nullptr;
        }
        case HircType::MusicTrack:
            return true;
        case HircType::SwitchCntr: {
            const auto& sw = *static_cast<const SwitchObject*>(object);
            for (const auto& package : sw.switches) {
                for (uint32_t node : package.nodes) {
                    if (ReachesAudio(banks, node, depth + 1)) {
                        return true;
                    }
                }
            }
            return false;
        }
        default:
            if (const auto* children = GetChildren(object)) {
                for (uint32_t child : *children) {
                    if (ReachesAudio(banks, child, depth + 1)) {
                        return true;
                    }
                }
            }
            return false;
    }
}

int RunScanEvents(const Options& options) {
    pt::Vfs vfs;
    if (!vfs.Mount(options.game)) {
        return 1;
    }
    SoundBankSet banks;
    std::string error;
    if (!banks.Load(vfs, SoundBankSet::DefaultPackages(), &error)) {
        std::printf("bank load failed: %s\n", error.c_str());
        return 1;
    }
    struct Job {
        uint32_t id = 0;
        bool expects_audio = false;
        double rms = 0.0;
        double peak = 0.0;
    };
    std::vector<Job> jobs;
    for (const auto& bank : banks.Banks()) {
        for (const auto& object : bank->Objects()) {
            if (object->type != HircType::Event) {
                continue;
            }
            Job job;
            job.id = object->id;
            for (uint32_t action_id : static_cast<const EventObject*>(object.get())->action_ids) {
                const HircObject* found = banks.Find(action_id);
                if (!found || found->type != HircType::Action) {
                    continue;
                }
                const auto& action = *static_cast<const ActionObject*>(found);
                if ((action.Category() == 0x04 || action.Category() == 0x05) && ReachesAudio(banks, action.target_id, 0)) {
                    job.expects_audio = true;
                }
            }
            jobs.push_back(job);
        }
    }
    const float seconds = options.seconds > 0.0f ? options.seconds : 10.0f;
    std::atomic<size_t> next{0};
    std::vector<std::thread> workers;
    const unsigned thread_count = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    for (unsigned t = 0; t < thread_count; ++t) {
        workers.emplace_back([&] {
            pt::Vfs local_vfs;
            if (!local_vfs.Mount(options.game)) {
                return;
            }
            for (size_t i = next++; i < jobs.size(); i = next++) {
                SoundSystem sound;
                if (!sound.Init(local_vfs, false)) {
                    continue;
                }
                sound.SetRandomSeed(options.seed);
                sound.SetState("state_common", "in_game");
                sound.PostEventId(jobs[i].id, 1);
                std::vector<float> samples(static_cast<size_t>(seconds * kOutputRate) * 2);
                for (size_t offset = 0; offset < samples.size(); offset += 960) {
                    sound.RenderOffline(samples.data() + offset, static_cast<uint32_t>(std::min<size_t>(480, (samples.size() - offset) / 2)));
                    sound.Update(0.01f);
                }
                const Analysis a = Analyze(samples);
                jobs[i].rms = a.rms;
                jobs[i].peak = a.peak;
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }
    int flagged = 0;
    int audible = 0;
    int expecting = 0;
    for (const auto& job : jobs) {
        expecting += job.expects_audio ? 1 : 0;
        audible += job.rms > 1e-5 ? 1 : 0;
        const bool silent = Db(job.rms) < -100.0;
        if (job.expects_audio && silent) {
            ++flagged;
        }
        std::printf("%08X %s rms %7.1f dBFS peak %7.1f dBFS%s\n", job.id, job.expects_audio ? "audio" : "     ", Db(job.rms), Db(job.peak),
                    job.expects_audio && silent ? "  SILENT" : "");
    }
    std::printf("%zu events, %d reach media, %d audible in %.0f s, %d silent although they reach media\n", jobs.size(), expecting, audible,
                seconds, flagged);
    return 0;
}

bool TestOneShotObjectPool() {
    constexpr pt::audio::GameObjectId base = 0x10000;
    pt::game::OneShotObjectPool pool(base, 2);
    std::vector<pt::audio::PlayingId> live{11, 22};
    const auto is_playing = [&](pt::audio::PlayingId id) {
        return std::find(live.begin(), live.end(), id) != live.end();
    };
    const auto first = pool.Acquire(is_playing);
    const auto second = pool.Acquire(is_playing);
    pool.Track(first, 11);
    pool.Track(second, 22);
    const auto third = pool.Acquire(is_playing);
    if (first != base || second != base + 1 || third != base + 2 || pool.Size() != 3) {
        std::printf("one-shot pool reused a live emitter instead of allocating overflow\n");
        return false;
    }
    live.clear();
    const auto reused = pool.Acquire(is_playing);
    if (reused < base || reused >= base + 3) {
        std::printf("one-shot pool failed to reuse an emitter after playback ended\n");
        return false;
    }
    pt::game::OneShotObjectPool failed_post_pool(base + 100, 1);
    const auto failed_post_object = failed_post_pool.Acquire(is_playing);
    failed_post_pool.Track(failed_post_object, 0);
    if (failed_post_pool.Acquire(is_playing) != failed_post_object) {
        std::printf("one-shot pool treated a failed post as a live sound\n");
        return false;
    }
    pool.Reset();
    const auto after_reset = pool.Acquire(is_playing);
    if (pool.Size() != 2 || after_reset < base || after_reset >= base + 2) {
        std::printf("one-shot pool failed to reset overflow objects for shutdown\n");
        return false;
    }
    return true;
}

void Usage() {
    std::printf(
        "pt_audio_test [--game DIR] [--dump DIR]\n"
        "  --one-shot-pool                 verify positional emitters stay owned until playback ends\n"
        "  --decode-all                      decode every media and compare with the vgmstream WAVs in --dump\n"
        "  --event NAME [--seconds N] [--position X Y Z] [--forward X Y Z] [--listener PX PY PZ FX FY FZ UX UY UZ] [--aux BUS LEVEL]\n"
        "               [--state G S] [--switch G V] [--rtpc NAME V] [--at T EVENT] [--seed N]\n"
        "               [--out FILE.wav]     render offline (names may be #ids; --at also takes stopall and state:G=S)\n"
        "               [--motion FILE.csv]  also write the pad vibration levels (large and small motor, 0..255) every 10 ms\n"
        "               [--listener-path FILE] listener poses over time, lines of T PX PY PZ FX FY FZ UX UY UZ (seconds)\n"
        "  --demo-stream ASSET_PATH [--out FILE.wav]\n"
        "  --subtitles LANG\n"
        "  --fx-impulse                      impulse responses of the bus effects\n"
        "  --thread-stress                   API calls from this thread while another thread renders\n"
        "  --scan-events [--seconds N]       render every event (state_common in_game) and flag silent ones that reach media\n");
}

}

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::printf("missing value for %s\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--one-shot-pool") {
            return TestOneShotObjectPool() ? 0 : 1;
        } else if (arg == "--game") {
            options.game = next("--game");
        } else if (arg == "--dump") {
            options.dump = next("--dump");
        } else if (arg == "--decode-all") {
            options.mode = "decode-all";
        } else if (arg == "--event") {
            options.mode = "event";
            options.event = next("--event");
        } else if (arg == "--demo-stream") {
            options.mode = "demo-stream";
            options.argument = next("--demo-stream");
        } else if (arg == "--fx-impulse") {
            options.mode = "fx-impulse";
        } else if (arg == "--subtitles") {
            options.mode = "subtitles";
            options.argument = next("--subtitles");
        } else if (arg == "--seconds") {
            options.seconds = std::stof(next("--seconds"));
        } else if (arg == "--out") {
            options.out = next("--out");
        } else if (arg == "--motion") {
            options.motion = next("--motion");
        } else if (arg == "--position") {
            options.has_position = true;
            for (float& v : options.position) {
                v = std::stof(next("--position"));
            }
        } else if (arg == "--forward") {
            for (float& v : options.forward) {
                v = std::stof(next("--forward"));
            }
        } else if (arg == "--listener") {
            for (float& v : options.listener) {
                v = std::stof(next("--listener"));
            }
        } else if (arg == "--listener-path") {
            options.listener_path = next("--listener-path");
        } else if (arg == "--aux") {
            const std::string bus = next("--aux");
            options.aux.push_back({bus, std::stof(next("--aux"))});
        } else if (arg == "--state") {
            const std::string group = next("--state");
            options.states.push_back({group, next("--state")});
        } else if (arg == "--switch") {
            const std::string group = next("--switch");
            options.switches.push_back({group, next("--switch")});
        } else if (arg == "--rtpc") {
            const std::string name = next("--rtpc");
            options.rtpcs.push_back({name, std::stof(next("--rtpc"))});
        } else if (arg == "--at") {
            const float time = std::stof(next("--at"));
            options.timeline.push_back({time, next("--at")});
        } else if (arg == "--seed") {
            options.seed = static_cast<uint32_t>(std::stoul(next("--seed")));
        } else if (arg == "--language") {
            options.language = next("--language");
        } else if (arg == "--repeat") {
            options.repeat = std::max(1, std::stoi(next("--repeat")));
        } else if (arg == "--arg") {
            options.arguments.push_back(next("--arg"));
        } else if (arg == "--thread-stress") {
            options.mode = "thread-stress";
        } else if (arg == "--scan-events") {
            options.mode = "scan-events";
        } else {
            Usage();
            return 2;
        }
    }
    if (options.mode == "decode-all") {
        return RunDecodeAll(options);
    }
    if (options.mode == "event") {
        return RunEvent(options);
    }
    if (options.mode == "demo-stream") {
        return RunDemoStream(options);
    }
    if (options.mode == "subtitles") {
        return RunSubtitles(options);
    }
    if (options.mode == "fx-impulse") {
        return RunFxImpulse(options);
    }
    if (options.mode == "thread-stress") {
        return RunThreadStress(options);
    }
    if (options.mode == "scan-events") {
        return RunScanEvents(options);
    }
    Usage();
    return 2;
}
