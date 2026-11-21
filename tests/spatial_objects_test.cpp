#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "engine/audio/sound_package.h"
#include "engine/audio/sound_system.h"
#include "engine/audio/wwise_bank.h"
#include "engine/core/log.h"
#include "engine/fs/vfs.h"

namespace {

constexpr uint32_t kBlock = 512;

struct Result {
    double bed = 0.0;
    double objects = 0.0;
    std::vector<float> samples;
    uint32_t object_blocks = 0;
    glm::dvec3 place{0.0};
};

void Prepare(pt::audio::SoundSystem& sound) {
    sound.StopAll(0.0f);
    sound.ResetSession();
    sound.SetRandomSeed(1);
    sound.SetListener({0.0f, 1.6f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f});
    sound.SetObjectTransform(1, {0.0f, 1.6f, 2.0f}, {0.0f, 0.0f, -1.0f});
}

Result Run(pt::audio::SoundSystem& sound, uint32_t event, int mode) {
    Prepare(sound);
    sound.PostEventId(event, 1);
    Result r;
    std::vector<float> bed(kBlock * 8);
    pt::audio::AudioObjectMix mix;
    for (int b = 0; b < 160; ++b) {
        sound.Update(static_cast<float>(kBlock) / 48000.0f);
        if (mode == 0) {
            sound.RenderOfflineChannels(bed.data(), kBlock, 8);
        } else {
            mix.capacity = mode == 1 ? 0 : 32;
            sound.RenderOfflineObjects(bed.data(), kBlock, mix);
        }
        for (float v : bed) r.bed += static_cast<double>(v) * v;
        r.samples.insert(r.samples.end(), bed.begin(), bed.end());
        for (uint32_t i = 0; i < mix.count; ++i) {
            const auto& object = mix.objects[i];
            double energy = 0.0;
            for (float v : object.samples) energy += static_cast<double>(v) * v;
            r.objects += energy;
            r.place += glm::dvec3(object.position) * energy;
            if (energy > 0.0) ++r.object_blocks;
        }
    }
    return r;
}

}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::printf("usage: pt_spatial_objects_test <game folder>\n");
        return 2;
    }
    pt::Vfs vfs;
    if (!vfs.Mount(argv[1])) {
        std::printf("FAIL: cannot mount %s\n", argv[1]);
        return 2;
    }
    pt::audio::SoundBankSet banks;
    std::string error;
    if (!banks.Load(vfs, pt::audio::SoundBankSet::DefaultPackages(), &error)) {
        std::printf("FAIL: banks: %s\n", error.c_str());
        return 2;
    }
    std::vector<uint32_t> events;
    for (const auto& bank : banks.Banks()) {
        for (const auto& object : bank->Objects()) {
            if (object->type == pt::audio::HircType::Event) events.push_back(object->id);
        }
    }
    std::sort(events.begin(), events.end());
    events.erase(std::unique(events.begin(), events.end()), events.end());

    pt::audio::SoundSystem sound;
    if (!sound.Init(vfs, false, false)) {
        std::printf("FAIL: sound init\n");
        return 2;
    }
    sound.RegisterObject(1, "Probe");
    int failures = 0, heard = 0, repeatable = 0, as_objects = 0, at_probe = 0;
    double worst_energy = 0.0;
    for (const uint32_t id : events) {
        const Result mix71 = Run(sound, id, 0);
        if (mix71.bed <= 1e-9) continue;
        ++heard;
        const bool same = Run(sound, id, 0).samples == mix71.samples;
        repeatable += same;
        const Result none = Run(sound, id, 1);
        if (same && (none.samples != mix71.samples || none.objects != 0.0)) {
            std::printf("FAIL: event %08X: with no object room the bed is not the 7.1 mix\n", id);
            ++failures;
        }
        const Result objects = Run(sound, id, 2);
        if (objects.object_blocks == 0) continue;
        ++as_objects;
        const double ratio = (objects.bed + objects.objects) / mix71.bed;
        if (same) {
            worst_energy = std::max(worst_energy, std::abs(ratio - 1.0));
            if (std::abs(ratio - 1.0) > 0.05) {
                std::printf("FAIL: event %08X: bed and objects carry %.4f of the 7.1 mix's energy\n", id, ratio);
                ++failures;
            }
        }
        const glm::dvec3 place = objects.place / std::max(objects.objects, 1e-30);
        at_probe += glm::length(place - glm::dvec3(0.0, 0.0, 2.0)) < 0.05;
    }
    std::printf("%d events heard (%d repeatable), %d with dynamic objects, %d of them 2 m ahead on the probe; energy within %.4f\n", heard,
                repeatable, as_objects, at_probe, worst_energy);
    if (as_objects == 0 || at_probe == 0) {
        std::printf("FAIL: no sound became a dynamic object at its place\n");
        ++failures;
    }
    std::printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
