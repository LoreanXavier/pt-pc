#pragma once

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "engine/anim/anim_codec.h"
#include "engine/anim/motion_data.h"

namespace pt::anim {

struct StreamChunk {
    uint32_t tag = 0;
    size_t offset = 0;
    uint32_t size = 0;
    double time = 0.0;
};

struct MotionBlock {
    double time = 0.0;
    uint32_t flags = 0;
    uint32_t start = 0;
    uint32_t count = 0;
    uint32_t track_count = 0;
    uint32_t size = 0;
    uint32_t prefetch = 0;
    std::vector<uint32_t> offsets;
    bool has_offset_vector = false;
    glm::vec3 offset_vector{0.0f};
    bool has_applied_offset = false;
    glm::vec3 applied_offset{0.0f};
    std::vector<uint16_t> track_flags;
    size_t payload = 0;
    size_t payload_end = 0;

    uint16_t TrackFlags(size_t track) const { return track < track_flags.size() ? track_flags[track] : 0; }
};

struct EventPacket {
    double time = 0.0;
    uint32_t start_frame = 0;
    uint32_t table_hash = 0;
    std::vector<StreamEvent> events;
};

enum class ActorKind : uint8_t { Camera, CameraParam, SiFrame, Locator, ModelRoot, Skeleton, MotionPoints, Timeline, Other };

struct ActorUnit {
    uint32_t hash = 0;
    uint8_t flags = 0;
    std::vector<uint16_t> tracks;
    int rotation = -1;
    int translation = -1;
};

struct StreamActor {
    ActorKind kind = ActorKind::Other;
    std::string target;
    int node = -1;
    std::vector<ActorUnit> units;
};

struct StreamTrack {
    uint16_t index = 0;
    uint8_t kind = 0;
    uint8_t bits = 0;
    uint32_t unit_hash = 0;
    uint8_t unit_flags = 0;
    int actor = -1;
    bool valid = false;
};

struct TrackBlock {
    uint32_t start = 0;
    uint32_t count = 0;
    uint16_t flags = 0;
    uint32_t first_key = 0;
    uint32_t key_count = 0;
};

struct DecodedTrack {
    uint8_t kind = 0;
    std::vector<TrackBlock> blocks;
    std::vector<Key> keys;

    bool Sample(double frame, glm::vec4& out) const;
};

class DemoStreamFile {
public:
    static constexpr uint32_t kTagDemo = 0x4F4D4544;
    static constexpr uint32_t kTagSound = 0x20444E53;
    static constexpr uint32_t kTagSys = 0x20535953;
    static constexpr uint32_t kTagEnd = 0x20444E45;

    bool Parse(std::vector<uint8_t> bytes, std::string* error);

    std::span<const uint8_t> Bytes() const { return bytes_; }
    const std::vector<StreamChunk>& Chunks() const { return chunks_; }
    const std::vector<MotionNode>& Nodes() const { return nodes_; }
    const std::vector<MotionBlock>& Blocks() const { return blocks_; }
    const std::vector<EventPacket>& Packets() const { return packets_; }
    const std::vector<StreamTrack>& Tracks() const { return tracks_; }
    const std::vector<StreamActor>& Actors() const { return actors_; }

    uint32_t Length() const { return length_; }
    uint32_t StreamFrames() const { return stream_frames_; }
    uint32_t Loops() const { return loops_; }
    double EndTime() const { return end_time_; }
    bool HasSound() const { return sound_chunks_ > 0; }
    uint32_t SoundChunks() const { return sound_chunks_; }

    bool DecodeTrack(size_t track, uint32_t last_frame, DecodedTrack& out) const;
    bool ActorActive(size_t actor, uint32_t frame) const;
    std::vector<const StreamEvent*> FirstLoopEvents() const;
    int FindActor(ActorKind kind, std::string_view target = {}) const;

private:
    void BuildActors();

    std::vector<uint8_t> bytes_;
    std::vector<StreamChunk> chunks_;
    std::vector<MotionNode> nodes_;
    std::vector<MotionBlock> blocks_;
    std::vector<EventPacket> packets_;
    std::vector<StreamTrack> tracks_;
    std::vector<StreamActor> actors_;
    uint32_t length_ = 0;
    uint32_t stream_frames_ = 0;
    uint32_t loops_ = 1;
    double end_time_ = 0.0;
    uint32_t sound_chunks_ = 0;
};

}
