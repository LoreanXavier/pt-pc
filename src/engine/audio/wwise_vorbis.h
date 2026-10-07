#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace pt::audio {

struct WemInfo;

class WwiseVorbisStream {
public:
    struct Packet {
        uint32_t offset = 0;
        uint16_t size = 0;
        bool long_block = false;
        uint64_t sample_start = 0;
        uint32_t sample_count = 0;
    };

    WwiseVorbisStream();
    ~WwiseVorbisStream();
    WwiseVorbisStream(const WwiseVorbisStream&) = delete;
    WwiseVorbisStream& operator=(const WwiseVorbisStream&) = delete;

    bool Init(std::span<const uint8_t> blob, const WemInfo& info, std::string* error);

    uint32_t Channels() const { return channels_; }
    uint64_t SampleCount() const { return sample_count_; }
    const std::vector<Packet>& Packets() const { return packets_; }
    std::span<const uint8_t> Blob() const { return blob_; }
    void* VorbisInfo() const { return vorbis_info_; }
    bool ModifiedPackets() const { return modified_packets_; }
    int ModeBits() const { return mode_bits_; }
    bool ModeBlockFlag(uint32_t mode) const { return mode < mode_blockflags_.size() && mode_blockflags_[mode]; }
    uint32_t PacketMode(const Packet& packet) const;
    size_t RebuildPacket(size_t index, bool prev_long, std::vector<uint8_t>& out) const;

private:
    bool BuildSetup(std::span<const uint8_t> setup, std::vector<uint8_t>& out, std::string* error);

    std::span<const uint8_t> blob_;
    uint32_t channels_ = 0;
    uint32_t sample_rate_ = 0;
    uint64_t sample_count_ = 0;
    uint8_t blocksize_small_ = 0;
    uint8_t blocksize_large_ = 0;
    bool modified_packets_ = true;
    int mode_bits_ = 0;
    std::vector<bool> mode_blockflags_;
    std::vector<Packet> packets_;
    void* vorbis_info_ = nullptr;
    void* vorbis_comment_ = nullptr;
};

class WwiseVorbisDecoder {
public:
    explicit WwiseVorbisDecoder(const WwiseVorbisStream& stream);
    ~WwiseVorbisDecoder();
    WwiseVorbisDecoder(const WwiseVorbisDecoder&) = delete;
    WwiseVorbisDecoder& operator=(const WwiseVorbisDecoder&) = delete;

    uint32_t Read(float* out, uint32_t frames);
    bool Seek(uint64_t frame);
    uint64_t Position() const { return position_; }

private:
    bool DecodeNextPacket();
    void Reset();

    const WwiseVorbisStream& stream_;
    struct State;
    std::unique_ptr<State> state_;
    size_t next_packet_ = 0;
    bool prev_long_ = false;
    uint64_t position_ = 0;
    uint64_t skip_ = 0;
    std::vector<uint8_t> packet_buffer_;
};

}
