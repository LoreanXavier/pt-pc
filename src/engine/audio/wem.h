#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace pt::audio {

namespace wem_codec {
constexpr uint16_t Pcm = 0x0001;
constexpr uint16_t WwiseImaAdpcm = 0x0002;
constexpr uint16_t PcmExtensible = 0xFFFE;
constexpr uint16_t WwiseVorbis = 0xFFFF;
}

struct WemMarker {
    uint32_t id = 0;
    uint32_t sample = 0;
    std::string label;
};

struct WemInfo {
    uint16_t codec_tag = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint32_t avg_bytes_per_second = 0;
    uint16_t block_align = 0;
    uint16_t bits_per_sample = 0;
    uint32_t channel_mask = 0;
    size_t fmt_offset = 0;
    size_t fmt_size = 0;
    size_t data_offset = 0;
    size_t data_size = 0;
    bool looping = false;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;
    std::vector<WemMarker> markers;
};

bool ParseWem(std::span<const uint8_t> blob, WemInfo& info, std::string* error);
uint32_t DefaultChannelMask(uint32_t channels);

class WwiseVorbisStream;

class MediaReader {
public:
    virtual ~MediaReader() = default;
    virtual uint32_t Read(float* out, uint32_t frames) = 0;
    virtual bool Seek(uint64_t frame) = 0;
    virtual uint64_t Position() const = 0;
};

class Media {
public:
    static std::shared_ptr<Media> Create(uint32_t id, std::shared_ptr<const std::vector<uint8_t>> storage, size_t offset, size_t size,
                                         std::string* error);
    ~Media();

    uint32_t Id() const { return id_; }
    const WemInfo& Info() const { return info_; }
    uint32_t Channels() const { return info_.channels; }
    uint32_t SampleRate() const { return info_.sample_rate; }
    uint64_t Frames() const { return frames_; }
    std::span<const uint8_t> Blob() const;
    std::unique_ptr<MediaReader> OpenReader() const;
    std::vector<float> DecodeAll() const;
    const char* CodecName() const;

private:
    Media() = default;

    uint32_t id_ = 0;
    std::shared_ptr<const std::vector<uint8_t>> storage_;
    size_t offset_ = 0;
    size_t size_ = 0;
    WemInfo info_;
    uint64_t frames_ = 0;
    std::unique_ptr<WwiseVorbisStream> vorbis_;
};

}
