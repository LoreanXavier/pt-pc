#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "engine/audio/audio_objects.h"

namespace pt::audio {

class SpatialOutput {
public:
    using RenderFunction = std::function<void(float* interleaved, uint32_t frames, uint32_t channels)>;
    using ObjectRenderFunction = std::function<void(float* bed, uint32_t frames, AudioObjectMix& mix)>;

    SpatialOutput();
    ~SpatialOutput();
    SpatialOutput(const SpatialOutput&) = delete;
    SpatialOutput& operator=(const SpatialOutput&) = delete;

    bool Open(uint32_t sample_rate, RenderFunction render, const std::string& endpoint_id = {});
    bool Open(uint32_t sample_rate, RenderFunction render, ObjectRenderFunction object_render, const std::string& endpoint_id);
    uint32_t DynamicObjects() const;
    void Close();
    const std::string& DeviceName() const;
    const std::string& DeviceId() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
