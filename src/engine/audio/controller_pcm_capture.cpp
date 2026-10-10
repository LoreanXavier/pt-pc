#include "engine/audio/controller_pcm_capture.h"

#include <algorithm>
#include <cstring>

namespace pt::audio {

bool ControllerPcmQueue::TryPush(const float* stereo, uint32_t frames, const float* haptic) {
    if (!stereo || frames == 0 || frames > ControllerPcmBlock::kMaxFrames) {
        return false;
    }
    const uint64_t write = write_index_.load(std::memory_order_relaxed);
    const uint64_t read = read_index_.load(std::memory_order_acquire);
    if (write - read >= kCapacity) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    ControllerPcmBlock& block = blocks_[write % kCapacity];
    block.frames = frames;
    std::copy_n(stereo, static_cast<size_t>(frames) * 2, block.stereo.data());
    std::copy_n(haptic ? haptic : stereo, static_cast<size_t>(frames) * 2, block.haptic.data());
    write_index_.store(write + 1, std::memory_order_release);
    return true;
}

bool ControllerPcmQueue::TryPop(ControllerPcmBlock& block) {
    const uint64_t read = read_index_.load(std::memory_order_relaxed);
    const uint64_t write = write_index_.load(std::memory_order_acquire);
    if (read == write) {
        return false;
    }
    block = blocks_[read % kCapacity];
    read_index_.store(read + 1, std::memory_order_release);
    return true;
}

void ControllerPcmCapture::BeginBlock(uint32_t frames) {
    block_frames_ = frames <= ControllerPcmBlock::kMaxFrames ? frames : 0;
    for (size_t i = 0; i < block_events_.size(); ++i) {
        block_events_[i] = block_frames_ ? selected_events_[i].load(std::memory_order_acquire) : 0;
        block_haptic_events_[i] = block_frames_ ? haptic_events_[i].load(std::memory_order_acquire) : 0;
    }
    active_ = false;
    if (HasSelectedEvents()) {
        std::fill_n(scratch_.data(), static_cast<size_t>(block_frames_) * 2, 0.0f);
        std::fill_n(haptic_scratch_.data(), static_cast<size_t>(block_frames_) * 2, 0.0f);
    }
}

void ControllerPcmCapture::SetEvent(uint32_t event_id) {
    selected_events_[0].store(event_id, std::memory_order_release);
    for (size_t i = 1; i < selected_events_.size(); ++i) {
        selected_events_[i].store(0, std::memory_order_release);
    }
}

void ControllerPcmCapture::SetEvents(std::span<const uint32_t> event_ids) {
    for (size_t i = 0; i < selected_events_.size(); ++i) {
        selected_events_[i].store(i < event_ids.size() ? event_ids[i] : 0, std::memory_order_release);
    }
}

void ControllerPcmCapture::SetHapticEvents(std::span<const uint32_t> event_ids) {
    for (size_t i = 0; i < haptic_events_.size(); ++i) {
        haptic_events_[i].store(i < event_ids.size() ? event_ids[i] : 0, std::memory_order_release);
    }
}

bool ControllerPcmCapture::HasSelectedEvents() const {
    const auto any = [](uint32_t event_id) { return event_id != 0; };
    return std::any_of(block_events_.begin(), block_events_.end(), any) ||
           std::any_of(block_haptic_events_.begin(), block_haptic_events_.end(), any);
}

bool ControllerPcmCapture::Accumulate(uint32_t event_id, uint32_t frame, float left, float right) {
    if (event_id == 0 || frame >= block_frames_) {
        return false;
    }
    const bool speaker = std::find(block_events_.begin(), block_events_.end(), event_id) != block_events_.end();
    const bool haptic_only = !speaker && std::find(block_haptic_events_.begin(), block_haptic_events_.end(), event_id) != block_haptic_events_.end();
    if (!speaker && !haptic_only) {
        return false;
    }
    if (speaker) {
        scratch_[static_cast<size_t>(frame) * 2] += left;
        scratch_[static_cast<size_t>(frame) * 2 + 1] += right;
    }
    // the haptic-only events (the player's own footsteps) are a light touch under Lisa's
    constexpr float kHapticOnlyGain = 0.35f;
    const float weight = speaker ? 1.0f : kHapticOnlyGain;
    haptic_scratch_[static_cast<size_t>(frame) * 2] += left * weight;
    haptic_scratch_[static_cast<size_t>(frame) * 2 + 1] += right * weight;
    active_ = true;
    return true;
}

bool ControllerPcmCapture::SubmitBlock() {
    return active_ && queue_.TryPush(scratch_.data(), block_frames_, haptic_scratch_.data());
}

}
