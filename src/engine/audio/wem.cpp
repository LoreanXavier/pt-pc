#include "engine/audio/wem.h"

#include <algorithm>
#include <cstring>
#include <format>

#include "engine/audio/wwise_vorbis.h"

namespace pt::audio {
namespace {

uint16_t U16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

uint32_t U32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

constexpr int kImaSteps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
    1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};
constexpr int kImaIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
constexpr uint32_t kImaBlockBytes = 36;
constexpr uint32_t kImaBlockFrames = 64;

class PcmReader : public MediaReader {
public:
    PcmReader(std::span<const uint8_t> data, uint32_t channels, uint64_t frames) : data_(data), channels_(channels), frames_(frames) {}

    uint32_t Read(float* out, uint32_t frames) override {
        const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(frames, frames_ - position_));
        const size_t count = static_cast<size_t>(n) * channels_;
        const uint8_t* src = data_.data() + position_ * channels_ * 2;
        for (size_t i = 0; i < count; ++i) {
            int16_t s;
            std::memcpy(&s, src + i * 2, 2);
            out[i] = static_cast<float>(s) * (1.0f / 32768.0f);
        }
        position_ += n;
        return n;
    }

    bool Seek(uint64_t frame) override {
        position_ = std::min(frame, frames_);
        return true;
    }

    uint64_t Position() const override { return position_; }

private:
    std::span<const uint8_t> data_;
    uint32_t channels_;
    uint64_t frames_;
    uint64_t position_ = 0;
};

class ImaReader : public MediaReader {
public:
    ImaReader(std::span<const uint8_t> data, uint32_t channels, uint64_t frames)
        : data_(data), channels_(channels), frames_(frames), block_(static_cast<size_t>(kImaBlockFrames) * channels) {}

    uint32_t Read(float* out, uint32_t frames) override {
        uint32_t produced = 0;
        while (produced < frames && position_ < frames_) {
            const uint64_t block = position_ / kImaBlockFrames;
            if (block != decoded_block_) {
                DecodeBlock(block);
            }
            const uint32_t in_block = static_cast<uint32_t>(position_ % kImaBlockFrames);
            const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>({kImaBlockFrames - in_block, frames - produced, frames_ - position_}));
            std::memcpy(out + static_cast<size_t>(produced) * channels_, block_.data() + static_cast<size_t>(in_block) * channels_,
                        sizeof(float) * n * channels_);
            produced += n;
            position_ += n;
        }
        return produced;
    }

    bool Seek(uint64_t frame) override {
        position_ = std::min(frame, frames_);
        return true;
    }

    uint64_t Position() const override { return position_; }

private:
    void DecodeBlock(uint64_t block) {
        decoded_block_ = block;
        std::fill(block_.begin(), block_.end(), 0.0f);
        const size_t frame_bytes = static_cast<size_t>(kImaBlockBytes) * channels_;
        const size_t frame_offset = block * frame_bytes;
        if (frame_offset >= data_.size()) {
            return;
        }
        const size_t available = std::min(frame_bytes, data_.size() - frame_offset);
        const size_t sub_block = available == frame_bytes ? kImaBlockBytes : available / channels_;
        if (sub_block < 4) {
            return;
        }
        const uint32_t samples = static_cast<uint32_t>(std::min<size_t>((sub_block - 4) * 2, kImaBlockFrames));
        for (uint32_t c = 0; c < channels_; ++c) {
            const uint8_t* header = data_.data() + frame_offset + c * sub_block;
            int hist = static_cast<int16_t>(U16(header));
            int index = std::clamp<int>(header[2], 0, 88);
            float* dst = block_.data() + c;
            dst[0] = static_cast<float>(hist) * (1.0f / 32768.0f);
            const uint8_t* nibbles = header + 4;
            for (uint32_t n = 1; n < samples; ++n) {
                const uint32_t k = n - 1;
                const int nibble = (nibbles[k >> 1] >> ((k & 1) ? 4 : 0)) & 0xF;
                const int step = kImaSteps[index];
                int delta = ((2 * (nibble & 7) + 1) * step) >> 3;
                if (nibble & 8) {
                    delta = -delta;
                }
                hist = std::clamp(hist + delta, -32768, 32767);
                index = std::clamp(index + kImaIndex[nibble], 0, 88);
                dst[static_cast<size_t>(n) * channels_] = static_cast<float>(hist) * (1.0f / 32768.0f);
            }
        }
    }

    std::span<const uint8_t> data_;
    uint32_t channels_;
    uint64_t frames_;
    uint64_t position_ = 0;
    uint64_t decoded_block_ = UINT64_MAX;
    std::vector<float> block_;
};

class VorbisReader : public MediaReader {
public:
    explicit VorbisReader(const WwiseVorbisStream& stream) : decoder_(stream) {}

    uint32_t Read(float* out, uint32_t frames) override { return decoder_.Read(out, frames); }
    bool Seek(uint64_t frame) override { return decoder_.Seek(frame); }
    uint64_t Position() const override { return decoder_.Position(); }

private:
    WwiseVorbisDecoder decoder_;
};

}

uint32_t DefaultChannelMask(uint32_t channels) {
    switch (channels) {
        case 1: return 0x4;
        case 2: return 0x3;
        case 3: return 0x7;
        case 4: return 0x33;
        case 5: return 0x37;
        case 6: return 0x3F;
        case 7: return 0x637;
        case 8: return 0x63F;
        default: return 0;
    }
}

bool ParseWem(std::span<const uint8_t> blob, WemInfo& info, std::string* error) {
    info = {};
    if (blob.size() < 12 || std::memcmp(blob.data(), "RIFF", 4) != 0 || std::memcmp(blob.data() + 8, "WAVE", 4) != 0) {
        if (error) {
            *error = "not a RIFF/WAVE file";
        }
        return false;
    }
    bool have_fmt = false;
    bool have_data = false;
    std::vector<std::pair<uint32_t, uint32_t>> cue_positions;
    std::vector<std::pair<uint32_t, std::string>> labels;
    size_t pos = 12;
    while (pos + 8 <= blob.size()) {
        const uint8_t* chunk = blob.data() + pos;
        const uint32_t size = U32(chunk + 4);
        const size_t start = pos + 8;
        const size_t available = std::min<size_t>(size, blob.size() - start);
        if (std::memcmp(chunk, "fmt ", 4) == 0 && available >= 16) {
            const uint8_t* f = blob.data() + start;
            info.codec_tag = U16(f);
            info.channels = U16(f + 2);
            info.sample_rate = U32(f + 4);
            info.avg_bytes_per_second = U32(f + 8);
            info.block_align = U16(f + 12);
            info.bits_per_sample = U16(f + 14);
            if (available >= 0x18) {
                info.channel_mask = U32(f + 0x14);
            }
            info.fmt_offset = start;
            info.fmt_size = available;
            have_fmt = true;
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            info.data_offset = start;
            info.data_size = available;
            have_data = true;
        } else if (std::memcmp(chunk, "smpl", 4) == 0 && available >= 36) {
            const uint32_t loops = U32(blob.data() + start + 28);
            if (loops > 0 && available >= 36 + 24) {
                const uint8_t* loop = blob.data() + start + 36;
                info.looping = true;
                info.loop_start = U32(loop + 8);
                info.loop_end = U32(loop + 12) + 1;
            }
        } else if (std::memcmp(chunk, "cue ", 4) == 0 && available >= 4) {
            const uint32_t count = U32(blob.data() + start);
            for (uint32_t i = 0; i < count && 4 + (i + 1) * 24 <= available; ++i) {
                const uint8_t* cue = blob.data() + start + 4 + i * 24;
                cue_positions.push_back({U32(cue), U32(cue + 20)});
            }
        } else if (std::memcmp(chunk, "LIST", 4) == 0 && available >= 4 && std::memcmp(blob.data() + start, "adtl", 4) == 0) {
            size_t sub = start + 4;
            while (sub + 8 <= start + available) {
                const uint32_t sub_size = U32(blob.data() + sub + 4);
                if (std::memcmp(blob.data() + sub, "labl", 4) == 0 && sub_size >= 4 && sub + 8 + sub_size <= start + available) {
                    const uint32_t cue_id = U32(blob.data() + sub + 8);
                    const char* text = reinterpret_cast<const char*>(blob.data() + sub + 12);
                    const size_t max_length = sub_size - 4;
                    labels.push_back({cue_id, std::string(text, strnlen(text, max_length))});
                }
                sub += 8 + sub_size + (sub_size & 1);
            }
        }
        pos = start + size + (size & 1);
    }
    if (!have_fmt || !have_data || info.channels == 0) {
        if (error) {
            *error = "missing fmt or data chunk";
        }
        return false;
    }
    if (info.channel_mask == 0) {
        info.channel_mask = DefaultChannelMask(info.channels);
    }
    for (const auto& [id, sample] : cue_positions) {
        WemMarker marker;
        marker.id = id;
        marker.sample = sample;
        for (const auto& [label_id, text] : labels) {
            if (label_id == id) {
                marker.label = text;
            }
        }
        info.markers.push_back(std::move(marker));
    }
    std::sort(info.markers.begin(), info.markers.end(), [](const WemMarker& a, const WemMarker& b) { return a.sample < b.sample; });
    return true;
}

Media::~Media() = default;

std::shared_ptr<Media> Media::Create(uint32_t id, std::shared_ptr<const std::vector<uint8_t>> storage, size_t offset, size_t size,
                                     std::string* error) {
    if (!storage || offset + size > storage->size()) {
        if (error) {
            *error = "media range outside its storage";
        }
        return nullptr;
    }
    std::shared_ptr<Media> media(new Media());
    media->id_ = id;
    media->storage_ = std::move(storage);
    media->offset_ = offset;
    media->size_ = size;
    const auto blob = media->Blob();
    if (!ParseWem(blob, media->info_, error)) {
        return nullptr;
    }
    auto& info = media->info_;
    switch (info.codec_tag) {
        case wem_codec::Pcm:
        case wem_codec::PcmExtensible:
            if (info.bits_per_sample != 16) {
                if (error) {
                    *error = std::format("unsupported PCM bit depth {}", info.bits_per_sample);
                }
                return nullptr;
            }
            media->frames_ = info.data_size / (2ull * info.channels);
            break;
        case wem_codec::WwiseImaAdpcm: {
            const size_t frame_bytes = static_cast<size_t>(kImaBlockBytes) * info.channels;
            const size_t blocks = info.data_size / frame_bytes;
            const size_t rest = info.data_size % frame_bytes;
            media->frames_ = blocks * kImaBlockFrames;
            if (rest > 4ull * info.channels) {
                media->frames_ += (rest - 4ull * info.channels) * 2 / info.channels;
            }
            break;
        }
        case wem_codec::WwiseVorbis: {
            media->vorbis_ = std::make_unique<WwiseVorbisStream>();
            if (!media->vorbis_->Init(blob, info, error)) {
                return nullptr;
            }
            media->frames_ = media->vorbis_->SampleCount();
            break;
        }
        default:
            if (error) {
                *error = std::format("unsupported codec tag 0x{:04X}", info.codec_tag);
            }
            return nullptr;
    }
    if (info.looping) {
        info.loop_end = static_cast<uint32_t>(std::min<uint64_t>(info.loop_end, media->frames_));
        if (info.loop_start >= info.loop_end) {
            info.looping = false;
        }
    }
    return media;
}

std::span<const uint8_t> Media::Blob() const {
    return std::span<const uint8_t>(storage_->data() + offset_, size_);
}

std::unique_ptr<MediaReader> Media::OpenReader() const {
    const auto blob = Blob();
    switch (info_.codec_tag) {
        case wem_codec::Pcm:
        case wem_codec::PcmExtensible:
            return std::make_unique<PcmReader>(blob.subspan(info_.data_offset, info_.data_size), info_.channels, frames_);
        case wem_codec::WwiseImaAdpcm:
            return std::make_unique<ImaReader>(blob.subspan(info_.data_offset, info_.data_size), info_.channels, frames_);
        case wem_codec::WwiseVorbis:
            return std::make_unique<VorbisReader>(*vorbis_);
        default:
            return nullptr;
    }
}

std::vector<float> Media::DecodeAll() const {
    std::vector<float> out(static_cast<size_t>(frames_) * info_.channels);
    auto reader = OpenReader();
    if (!reader) {
        return {};
    }
    uint64_t done = 0;
    while (done < frames_) {
        const uint32_t chunk = static_cast<uint32_t>(std::min<uint64_t>(frames_ - done, 65536));
        const uint32_t got = reader->Read(out.data() + done * info_.channels, chunk);
        if (got == 0) {
            break;
        }
        done += got;
    }
    out.resize(static_cast<size_t>(done) * info_.channels);
    return out;
}

const char* Media::CodecName() const {
    switch (info_.codec_tag) {
        case wem_codec::Pcm:
        case wem_codec::PcmExtensible: return "PCM";
        case wem_codec::WwiseImaAdpcm: return "IMA ADPCM";
        case wem_codec::WwiseVorbis: return "Vorbis";
        default: return "unknown";
    }
}

}
