#include "engine/anim/demo_file.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace pt::anim {
namespace {

template <typename T>
T At(std::span<const uint8_t> data, size_t offset) {
    T value{};
    if (offset + sizeof(T) <= data.size()) {
        std::memcpy(&value, data.data() + offset, sizeof(T));
    }
    return value;
}

size_t CoveringBlock(const std::vector<TrackBlock>& blocks, double frame) {
    auto it = std::lower_bound(blocks.begin(), blocks.end(), frame,
                               [](const TrackBlock& b, double f) { return static_cast<double>(b.start) + static_cast<double>(b.count) < f; });
    size_t index = it == blocks.end() ? blocks.size() - 1 : static_cast<size_t>(it - blocks.begin());
    if (index > 0 && frame < static_cast<double>(blocks[index].start)) {
        --index;
    }
    return index;
}

}

bool DecodedTrack::Sample(double frame, glm::vec4& out) const {
    if (blocks.empty()) {
        return false;
    }
    const size_t index = CoveringBlock(blocks, frame);
    const TrackBlock* block = &blocks[index];
    if (block->key_count == 0) {
        size_t previous = index;
        while (previous > 0 && blocks[previous].key_count == 0) {
            --previous;
        }
        if (blocks[previous].key_count != 0) {
            out = keys[blocks[previous].first_key + blocks[previous].key_count - 1].value;
            return true;
        }
        if (index + 1 < blocks.size() && blocks[index + 1].key_count && static_cast<double>(blocks[index + 1].start) <= frame) {
            block = &blocks[index + 1];
        } else {
            return false;
        }
    }
    out = SampleKeys(std::span<const Key>(keys.data() + block->first_key, block->key_count), kind, frame);
    return true;
}

bool DemoStreamFile::Parse(std::vector<uint8_t> bytes, std::string* error) {
    bytes_ = std::move(bytes);
    chunks_.clear();
    nodes_.clear();
    blocks_.clear();
    packets_.clear();
    tracks_.clear();
    actors_.clear();
    sound_chunks_ = 0;
    const std::span<const uint8_t> data(bytes_);
    size_t pos = 0;
    size_t header = 0;
    bool has_header = false;
    while (pos + 8 <= data.size()) {
        StreamChunk chunk;
        chunk.tag = At<uint32_t>(data, pos);
        chunk.size = At<uint32_t>(data, pos + 4);
        chunk.offset = pos;
        if (chunk.size == 0) {
            break;
        }
        if (chunk.size < 8 || pos + chunk.size > data.size()) {
            if (error) {
                *error = std::format("chunk at {:#x} has bad size {:#x}", pos, chunk.size);
            }
            return false;
        }
        if (chunk.tag != kTagSys && chunk.size >= 0x10) {
            chunk.time = At<double>(data, pos + 8);
        }
        chunks_.push_back(chunk);
        if (chunk.tag == kTagSound) {
            ++sound_chunks_;
        } else if (chunk.tag == kTagEnd) {
            end_time_ = chunk.time;
        } else if (chunk.tag == kTagDemo && chunk.size >= 0x20) {
            const size_t o = pos + 16;
            const uint32_t kind = At<uint32_t>(data, o);
            if (kind == 1 && !has_header) {
                header = pos;
                has_header = true;
            } else if (kind == 0) {
                MotionBlock block;
                block.time = chunk.time;
                block.flags = At<uint32_t>(data, o + 4);
                block.start = At<uint32_t>(data, o + 8);
                block.count = At<uint32_t>(data, o + 12);
                block.track_count = At<uint32_t>(data, o + 16);
                block.size = At<uint32_t>(data, o + 20);
                block.prefetch = At<uint32_t>(data, o + 24);
                size_t p = o + 0x1C;
                if (p + 4 * static_cast<size_t>(block.track_count) > pos + chunk.size) {
                    continue;
                }
                block.offsets.resize(block.track_count);
                for (uint32_t t = 0; t < block.track_count; ++t) {
                    block.offsets[t] = At<uint32_t>(data, p + 4 * static_cast<size_t>(t));
                }
                p += 4 * static_cast<size_t>(block.track_count);
                if (block.flags & 4) {
                    block.has_offset_vector = true;
                    block.offset_vector = glm::vec3(At<float>(data, p), At<float>(data, p + 4), At<float>(data, p + 8));
                    p += 12;
                }
                if (block.has_offset_vector) {
                    block.has_applied_offset = true;
                    block.applied_offset = block.offset_vector;
                } else if (!blocks_.empty() && blocks_.back().has_applied_offset) {
                    block.has_applied_offset = true;
                    block.applied_offset = blocks_.back().applied_offset;
                }
                if (block.flags & 2) {
                    block.track_flags.resize(block.track_count);
                    for (uint32_t t = 0; t < block.track_count; ++t) {
                        block.track_flags[t] = At<uint16_t>(data, p + 2 * static_cast<size_t>(t));
                    }
                }
                block.payload = o;
                block.payload_end = pos + chunk.size;
                blocks_.push_back(std::move(block));
            } else if (kind == 2) {
                EventPacket packet;
                packet.time = chunk.time;
                packet.start_frame = static_cast<uint32_t>(std::llround(chunk.time * kDemoFramesPerSecond));
                ParseEventTable(data.subspan(0, pos + chunk.size), o + 0x0C, packet.table_hash, packet.events);
                packets_.push_back(std::move(packet));
            }
        }
        pos += chunk.size;
    }
    if (!has_header) {
        if (error) {
            *error = "no DEMO header packet";
        }
        return false;
    }
    if (!ParseMotionTree(data, header + 0x20, nodes_)) {
        if (error) {
            *error = "bad node tree";
        }
        return false;
    }
    stream_frames_ = 0;
    for (const MotionBlock& b : blocks_) {
        stream_frames_ = std::max(stream_frames_, b.start + b.count);
    }
    std::vector<uint32_t> ends;
    for (const EventPacket& packet : packets_) {
        for (const StreamEvent& e : packet.events) {
            if (e.type == kEventDemoEnd) {
                for (const EventSection& s : e.sections) {
                    ends.push_back(static_cast<uint32_t>(std::max(s.start, 0)));
                }
            }
        }
    }
    length_ = ends.empty() ? stream_frames_ : ends.front();
    loops_ = std::max<uint32_t>(1, static_cast<uint32_t>(ends.size()));
    BuildActors();
    return true;
}

void DemoStreamFile::BuildActors() {
    for (size_t i = 0; i < nodes_.size(); ++i) {
        const MotionNode& node = nodes_[i];
        if (!node.units) {
            continue;
        }
        std::string group;
        for (int p = node.parent; p >= 0; p = nodes_[static_cast<size_t>(p)].parent) {
            const std::string& n = nodes_[static_cast<size_t>(p)].name;
            if (n == "CAMERA" || n == "LOCATOR" || n == "MOTION") {
                group = n;
                break;
            }
        }
        StreamActor actor;
        actor.node = static_cast<int>(i);
        actor.target = node.TargetName();
        const bool has_target = node.Param(kHashTargetName) != nullptr;
        if (node.name == "CameraParam") {
            actor.kind = ActorKind::CameraParam;
        } else if (node.name == "SI Frame") {
            actor.kind = ActorKind::SiFrame;
        } else if (node.name == "MOVE" && group == "CAMERA") {
            actor.kind = ActorKind::Camera;
        } else if (node.name == "MOVE" && group == "LOCATOR") {
            actor.kind = ActorKind::Locator;
        } else if (node.name == "MOVE" && group == "MOTION") {
            actor.kind = ActorKind::ModelRoot;
        } else if (node.name == "SKEL") {
            actor.kind = ActorKind::Skeleton;
        } else if (node.name == "MTP") {
            actor.kind = ActorKind::MotionPoints;
        } else if (node.name == "MOTION" && !has_target) {
            actor.kind = ActorKind::Timeline;
        }
        const int actor_index = static_cast<int>(actors_.size());
        for (const UnitDesc& unit : node.units->units) {
            ActorUnit au;
            au.hash = unit.hash;
            au.flags = unit.flags;
            for (const TrackDesc& t : unit.tracks) {
                au.tracks.push_back(t.index);
                if (IsRotationKind(t.kind)) {
                    if (au.rotation < 0) {
                        au.rotation = t.index;
                    }
                } else if (au.translation < 0) {
                    au.translation = t.index;
                }
                if (tracks_.size() <= t.index) {
                    tracks_.resize(static_cast<size_t>(t.index) + 1);
                }
                StreamTrack& st = tracks_[t.index];
                st.index = t.index;
                st.kind = t.kind;
                st.bits = t.bits;
                st.unit_hash = unit.hash;
                st.unit_flags = unit.flags;
                st.actor = actor_index;
                st.valid = true;
            }
            actor.units.push_back(std::move(au));
        }
        actors_.push_back(std::move(actor));
    }
}

bool DemoStreamFile::DecodeTrack(size_t track, uint32_t last_frame, DecodedTrack& out) const {
    out = DecodedTrack{};
    if (track >= tracks_.size() || !tracks_[track].valid) {
        return false;
    }
    const StreamTrack& info = tracks_[track];
    out.kind = info.kind;
    const std::span<const uint8_t> data(bytes_);
    for (const MotionBlock& b : blocks_) {
        if (b.start > last_frame) {
            break;
        }
        if (track >= b.offsets.size()) {
            continue;
        }
        TrackBlock tb;
        tb.start = b.start;
        tb.count = b.count;
        tb.flags = b.TrackFlags(track);
        tb.first_key = static_cast<uint32_t>(out.keys.size());
        const uint32_t offset = b.offsets[track];
        if (!(tb.flags & 8) && offset) {
            const std::span<const uint8_t> payload = data.subspan(b.payload, b.payload_end - b.payload);
            ReadKeys(payload, offset, info.kind, info.bits, (tb.flags & 1) != 0, b.count, b.start, out.keys);
            if ((tb.flags & 4) && b.has_applied_offset) {
                const int components = std::min(KindComponents(info.kind), 3);
                for (size_t k = tb.first_key; k < out.keys.size(); ++k) {
                    for (int c = 0; c < components; ++c) {
                        out.keys[k].value[c] += b.applied_offset[c];
                    }
                }
            }
        }
        tb.key_count = static_cast<uint32_t>(out.keys.size()) - tb.first_key;
        out.blocks.push_back(tb);
    }
    return true;
}

bool DemoStreamFile::ActorActive(size_t actor, uint32_t frame) const {
    if (actor >= actors_.size() || actors_[actor].units.empty() || actors_[actor].units[0].tracks.empty() || blocks_.empty()) {
        return true;
    }
    const uint16_t first = actors_[actor].units[0].tracks[0];
    for (const MotionBlock& b : blocks_) {
        if (frame <= b.start + b.count) {
            return !(b.TrackFlags(first) & 2);
        }
    }
    return !(blocks_.back().TrackFlags(first) & 2);
}

std::vector<const StreamEvent*> DemoStreamFile::FirstLoopEvents() const {
    std::vector<const StreamEvent*> out;
    for (const EventPacket& packet : packets_) {
        const uint32_t loop = length_ ? std::min(packet.start_frame / length_, loops_ - 1) : 0;
        if (loop > 0) {
            continue;
        }
        for (const StreamEvent& e : packet.events) {
            if (!e.Continuation()) {
                out.push_back(&e);
            }
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const StreamEvent* a, const StreamEvent* b) {
        const int32_t sa = a->sections.empty() ? -1 : a->sections[0].start;
        const int32_t sb = b->sections.empty() ? -1 : b->sections[0].start;
        return sa < sb;
    });
    return out;
}

int DemoStreamFile::FindActor(ActorKind kind, std::string_view target) const {
    for (size_t i = 0; i < actors_.size(); ++i) {
        if (actors_[i].kind == kind && (target.empty() || actors_[i].target == target)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

}
