#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace pt::audio {

struct ControllerPcmBlock {
    static constexpr uint32_t kMaxFrames = 256;
    uint32_t frames = 0;
    // the selected events for the speaker; haptic adds the haptic-only events (the player's footsteps) to them for the actuators
    std::array<float, kMaxFrames * 2> stereo{};
    std::array<float, kMaxFrames * 2> haptic{};
};

// Single audio-render producer and single game-thread consumer. Overflow drops the newest block.
class ControllerPcmQueue {
public:
    static constexpr size_t kCapacity = 8;

    // without a haptic mix the speaker mix stands for it
    bool TryPush(const float* stereo, uint32_t frames, const float* haptic = nullptr);
    bool TryPop(ControllerPcmBlock& block);
    uint32_t DroppedBlocks() const { return dropped_.load(std::memory_order_relaxed); }

private:
    std::array<ControllerPcmBlock, kCapacity> blocks_{};
    std::atomic<uint64_t> write_index_{0};
    std::atomic<uint64_t> read_index_{0};
    std::atomic<uint32_t> dropped_{0};
};

class ControllerPcmCapture {
public:
    static constexpr size_t kMaxEvents = 16;
    void SetEvent(uint32_t event_id);
    void SetEvents(std::span<const uint32_t> event_ids);
    // events that reach the actuators only, never the speaker
    void SetHapticEvents(std::span<const uint32_t> event_ids);
    void BeginBlock(uint32_t frames);
    bool HasSelectedEvents() const;
    bool Accumulate(uint32_t event_id, uint32_t frame, float left, float right);
    bool SubmitBlock();
    bool TryPop(ControllerPcmBlock& block) { return queue_.TryPop(block); }
    uint32_t DroppedBlocks() const { return queue_.DroppedBlocks(); }

private:
    std::array<std::atomic<uint32_t>, kMaxEvents> selected_events_{};
    std::array<uint32_t, kMaxEvents> block_events_{};
    std::array<std::atomic<uint32_t>, kMaxEvents> haptic_events_{};
    std::array<uint32_t, kMaxEvents> block_haptic_events_{};
    uint32_t block_frames_ = 0;
    bool active_ = false;
    std::array<float, ControllerPcmBlock::kMaxFrames * 2> scratch_{};
    std::array<float, ControllerPcmBlock::kMaxFrames * 2> haptic_scratch_{};
    ControllerPcmQueue queue_;
};

}
