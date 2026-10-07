#include "engine/audio/wwise_vorbis.h"

#include <vorbis/codec.h>

#include <algorithm>
#include <cstring>
#include <format>

#include "engine/audio/wem.h"

extern "C" const unsigned char pt_wwise_codebooks[];
extern "C" const size_t pt_wwise_codebooks_size;

namespace pt::audio {
namespace {

class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_bits_(size * 8) {}

    uint32_t Read(int bits) {
        uint32_t value = 0;
        for (int i = 0; i < bits; ++i) {
            if (pos_ >= size_bits_) {
                overflow_ = true;
                return value;
            }
            value |= static_cast<uint32_t>((data_[pos_ >> 3] >> (pos_ & 7)) & 1) << i;
            ++pos_;
        }
        return value;
    }

    size_t BitsRead() const { return pos_; }
    bool Overflow() const { return overflow_; }

private:
    const uint8_t* data_;
    size_t size_bits_;
    size_t pos_ = 0;
    bool overflow_ = false;
};

class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& out) : out_(out) {}

    void Write(uint32_t value, int bits) {
        for (int i = 0; i < bits; ++i) {
            if ((pos_ & 7) == 0) {
                out_.push_back(0);
            }
            out_.back() |= static_cast<uint8_t>(((value >> i) & 1) << (pos_ & 7));
            ++pos_;
        }
    }

    void WriteByte(uint8_t value) {
        const size_t shift = pos_ & 7;
        if (shift == 0) {
            out_.push_back(value);
        } else {
            out_.back() |= static_cast<uint8_t>(value << shift);
            out_.push_back(static_cast<uint8_t>(value >> (8 - shift)));
        }
        pos_ += 8;
    }

private:
    std::vector<uint8_t>& out_;
    size_t pos_ = 0;
};

int ILog(uint32_t v) {
    int ret = 0;
    while (v) {
        ++ret;
        v >>= 1;
    }
    return ret;
}

uint32_t BookMaptype1Quantvals(uint32_t entries, uint32_t dimensions) {
    const int bits = ILog(entries);
    int64_t vals = entries >> ((bits - 1) * (dimensions - 1) / dimensions);
    for (int guard = 0; guard < 100000; ++guard) {
        uint64_t acc = 1;
        uint64_t acc1 = 1;
        for (uint32_t i = 0; i < dimensions; ++i) {
            acc *= static_cast<uint64_t>(vals);
            acc1 *= static_cast<uint64_t>(vals + 1);
        }
        if (acc <= entries && acc1 > entries) {
            return static_cast<uint32_t>(vals);
        }
        if (acc > entries) {
            --vals;
        } else {
            ++vals;
        }
    }
    return 0;
}

uint32_t ReadU32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

uint16_t ReadU16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

bool RebuildCodebook(uint32_t id, BitWriter& w, std::string* error) {
    const uint8_t* lib = pt_wwise_codebooks;
    const size_t lib_size = pt_wwise_codebooks_size;
    if (lib_size < 8) {
        *error = "codebook library missing";
        return false;
    }
    const uint32_t offset_offset = ReadU32(lib + lib_size - 4);
    if (offset_offset >= lib_size) {
        *error = "bad codebook library";
        return false;
    }
    const uint32_t count = static_cast<uint32_t>((lib_size - offset_offset) / 4);
    if (id + 1 >= count) {
        *error = std::format("invalid codebook id {}", id);
        return false;
    }
    const uint32_t begin = ReadU32(lib + offset_offset + id * 4);
    const uint32_t end = ReadU32(lib + offset_offset + (id + 1) * 4);
    if (begin > end || end > offset_offset) {
        *error = std::format("bad codebook {} range", id);
        return false;
    }
    const size_t cb_size = end - begin;
    BitReader in(lib + begin, cb_size);
    const uint32_t dimensions = in.Read(4);
    const uint32_t entries = in.Read(14);
    w.Write(0x564342, 24);
    w.Write(dimensions, 16);
    w.Write(entries, 24);
    const uint32_t ordered = in.Read(1);
    w.Write(ordered, 1);
    if (ordered) {
        const uint32_t initial_length = in.Read(5);
        w.Write(initial_length, 5);
        uint32_t current = 0;
        while (current < entries) {
            const int bits = ILog(entries - current);
            const uint32_t number = in.Read(bits);
            w.Write(number, bits);
            current += number;
            if (in.Overflow()) {
                break;
            }
        }
        if (current > entries) {
            *error = "codebook entry count out of range";
            return false;
        }
    } else {
        const uint32_t length_bits = in.Read(3);
        const uint32_t sparse = in.Read(1);
        if (length_bits == 0 || length_bits > 5) {
            *error = "nonsense codeword length";
            return false;
        }
        w.Write(sparse, 1);
        for (uint32_t i = 0; i < entries; ++i) {
            bool present = true;
            if (sparse) {
                const uint32_t flag = in.Read(1);
                w.Write(flag, 1);
                present = flag != 0;
            }
            if (present) {
                w.Write(in.Read(static_cast<int>(length_bits)), 5);
            }
        }
    }
    const uint32_t lookup_type = in.Read(1);
    w.Write(lookup_type, 4);
    if (lookup_type == 1) {
        const uint32_t min = in.Read(32);
        const uint32_t max = in.Read(32);
        const uint32_t value_length = in.Read(4);
        const uint32_t sequence_flag = in.Read(1);
        w.Write(min, 32);
        w.Write(max, 32);
        w.Write(value_length, 4);
        w.Write(sequence_flag, 1);
        const uint32_t quantvals = BookMaptype1Quantvals(entries, dimensions);
        for (uint32_t i = 0; i < quantvals; ++i) {
            w.Write(in.Read(static_cast<int>(value_length + 1)), static_cast<int>(value_length + 1));
        }
    } else if (lookup_type != 0) {
        *error = "unexpected codebook lookup type";
        return false;
    }
    if (in.Overflow() || in.BitsRead() / 8 + 1 != cb_size) {
        *error = std::format("codebook {} size mismatch", id);
        return false;
    }
    return true;
}

void WriteHeaderStart(BitWriter& w, uint8_t type) {
    w.Write(type, 8);
    for (char c : std::string_view("vorbis")) {
        w.Write(static_cast<uint8_t>(c), 8);
    }
}

}

WwiseVorbisStream::WwiseVorbisStream() = default;

WwiseVorbisStream::~WwiseVorbisStream() {
    if (vorbis_info_) {
        vorbis_info_clear(static_cast<vorbis_info*>(vorbis_info_));
        delete static_cast<vorbis_info*>(vorbis_info_);
    }
    if (vorbis_comment_) {
        vorbis_comment_clear(static_cast<vorbis_comment*>(vorbis_comment_));
        delete static_cast<vorbis_comment*>(vorbis_comment_);
    }
}

bool WwiseVorbisStream::BuildSetup(std::span<const uint8_t> setup, std::vector<uint8_t>& out, std::string* error) {
    BitReader in(setup.data(), setup.size());
    BitWriter w(out);
    WriteHeaderStart(w, 5);
    const uint32_t codebook_count = in.Read(8) + 1;
    w.Write(codebook_count - 1, 8);
    for (uint32_t i = 0; i < codebook_count; ++i) {
        const uint32_t id = in.Read(10);
        if (!RebuildCodebook(id, w, error)) {
            return false;
        }
    }
    w.Write(0, 6);
    w.Write(0, 16);

    const uint32_t floor_count = in.Read(6) + 1;
    w.Write(floor_count - 1, 6);
    for (uint32_t i = 0; i < floor_count; ++i) {
        w.Write(1, 16);
        const uint32_t partitions = in.Read(5);
        w.Write(partitions, 5);
        std::vector<uint32_t> partition_class(partitions);
        uint32_t maximum_class = 0;
        for (auto& c : partition_class) {
            c = in.Read(4);
            w.Write(c, 4);
            maximum_class = std::max(maximum_class, c);
        }
        std::vector<uint32_t> class_dimensions(maximum_class + 1);
        for (uint32_t j = 0; j <= maximum_class; ++j) {
            const uint32_t dims = in.Read(3);
            w.Write(dims, 3);
            class_dimensions[j] = dims + 1;
            const uint32_t subclasses = in.Read(2);
            w.Write(subclasses, 2);
            if (subclasses) {
                const uint32_t masterbook = in.Read(8);
                w.Write(masterbook, 8);
                if (masterbook >= codebook_count) {
                    *error = "invalid floor1 masterbook";
                    return false;
                }
            }
            for (uint32_t k = 0; k < (1u << subclasses); ++k) {
                const uint32_t book = in.Read(8);
                w.Write(book, 8);
                if (book > 0 && book - 1 >= codebook_count) {
                    *error = "invalid floor1 subclass book";
                    return false;
                }
            }
        }
        w.Write(in.Read(2), 2);
        const uint32_t rangebits = in.Read(4);
        w.Write(rangebits, 4);
        for (uint32_t j = 0; j < partitions; ++j) {
            for (uint32_t k = 0; k < class_dimensions[partition_class[j]]; ++k) {
                w.Write(in.Read(static_cast<int>(rangebits)), static_cast<int>(rangebits));
            }
        }
    }

    const uint32_t residue_count = in.Read(6) + 1;
    w.Write(residue_count - 1, 6);
    for (uint32_t i = 0; i < residue_count; ++i) {
        const uint32_t residue_type = in.Read(2);
        w.Write(residue_type, 16);
        if (residue_type > 2) {
            *error = "invalid residue type";
            return false;
        }
        const uint32_t begin = in.Read(24);
        const uint32_t end = in.Read(24);
        const uint32_t partition_size = in.Read(24);
        const uint32_t classifications = in.Read(6) + 1;
        const uint32_t classbook = in.Read(8);
        w.Write(begin, 24);
        w.Write(end, 24);
        w.Write(partition_size, 24);
        w.Write(classifications - 1, 6);
        w.Write(classbook, 8);
        if (classbook >= codebook_count) {
            *error = "invalid residue classbook";
            return false;
        }
        std::vector<uint32_t> cascade(classifications);
        for (auto& c : cascade) {
            const uint32_t low = in.Read(3);
            w.Write(low, 3);
            const uint32_t flag = in.Read(1);
            w.Write(flag, 1);
            uint32_t high = 0;
            if (flag) {
                high = in.Read(5);
                w.Write(high, 5);
            }
            c = high * 8 + low;
        }
        for (uint32_t c : cascade) {
            for (int k = 0; k < 8; ++k) {
                if (c & (1u << k)) {
                    const uint32_t book = in.Read(8);
                    w.Write(book, 8);
                    if (book >= codebook_count) {
                        *error = "invalid residue book";
                        return false;
                    }
                }
            }
        }
    }

    const uint32_t mapping_count = in.Read(6) + 1;
    w.Write(mapping_count - 1, 6);
    for (uint32_t i = 0; i < mapping_count; ++i) {
        w.Write(0, 16);
        const uint32_t submaps_flag = in.Read(1);
        w.Write(submaps_flag, 1);
        uint32_t submaps = 1;
        if (submaps_flag) {
            const uint32_t less1 = in.Read(4);
            w.Write(less1, 4);
            submaps = less1 + 1;
        }
        const uint32_t square_polar = in.Read(1);
        w.Write(square_polar, 1);
        if (square_polar) {
            const uint32_t steps = in.Read(8) + 1;
            w.Write(steps - 1, 8);
            const int bits = ILog(channels_ - 1);
            for (uint32_t j = 0; j < steps; ++j) {
                const uint32_t magnitude = in.Read(bits);
                const uint32_t angle = in.Read(bits);
                w.Write(magnitude, bits);
                w.Write(angle, bits);
                if (angle == magnitude || magnitude >= channels_ || angle >= channels_) {
                    *error = "invalid coupling";
                    return false;
                }
            }
        }
        const uint32_t reserved = in.Read(2);
        w.Write(reserved, 2);
        if (reserved != 0) {
            *error = "mapping reserved field nonzero";
            return false;
        }
        if (submaps > 1) {
            for (uint32_t j = 0; j < channels_; ++j) {
                const uint32_t mux = in.Read(4);
                w.Write(mux, 4);
                if (mux >= submaps) {
                    *error = "mapping_mux >= submaps";
                    return false;
                }
            }
        }
        for (uint32_t j = 0; j < submaps; ++j) {
            w.Write(in.Read(8), 8);
            const uint32_t floor_number = in.Read(8);
            w.Write(floor_number, 8);
            if (floor_number >= floor_count) {
                *error = "invalid floor mapping";
                return false;
            }
            const uint32_t residue_number = in.Read(8);
            w.Write(residue_number, 8);
            if (residue_number >= residue_count) {
                *error = "invalid residue mapping";
                return false;
            }
        }
    }

    const uint32_t mode_count = in.Read(6) + 1;
    w.Write(mode_count - 1, 6);
    mode_blockflags_.assign(mode_count, false);
    mode_bits_ = ILog(mode_count - 1);
    for (uint32_t i = 0; i < mode_count; ++i) {
        const uint32_t block_flag = in.Read(1);
        w.Write(block_flag, 1);
        mode_blockflags_[i] = block_flag != 0;
        w.Write(0, 16);
        w.Write(0, 16);
        const uint32_t mapping = in.Read(8);
        w.Write(mapping, 8);
        if (mapping >= mapping_count) {
            *error = "invalid mode mapping";
            return false;
        }
    }
    w.Write(1, 1);
    if (in.Overflow() || (in.BitsRead() + 7) / 8 != setup.size()) {
        *error = "setup packet not consumed exactly";
        return false;
    }
    return true;
}

bool WwiseVorbisStream::Init(std::span<const uint8_t> blob, const WemInfo& info, std::string* error) {
    std::string local_error;
    if (!error) {
        error = &local_error;
    }
    blob_ = blob;
    channels_ = info.channels;
    sample_rate_ = info.sample_rate;
    if (info.fmt_size < 0x18 + 0x2A || channels_ == 0) {
        *error = std::format("unsupported Wwise Vorbis fmt size {}", info.fmt_size);
        return false;
    }
    const uint8_t* vorb = blob.data() + info.fmt_offset + 0x18;
    sample_count_ = ReadU32(vorb + 0x00);
    const uint32_t setup_offset = ReadU32(vorb + 0x10);
    const uint32_t audio_offset = ReadU32(vorb + 0x14);
    blocksize_small_ = vorb[0x28];
    blocksize_large_ = vorb[0x29];
    modified_packets_ = blocksize_small_ != blocksize_large_;
    const size_t data_begin = info.data_offset;
    const size_t data_end = info.data_offset + info.data_size;
    if (data_begin + setup_offset + 2 > data_end || data_begin + audio_offset > data_end) {
        *error = "Wwise Vorbis offsets outside data";
        return false;
    }
    const size_t setup_pos = data_begin + setup_offset;
    const uint16_t setup_size = ReadU16(blob.data() + setup_pos);
    if (setup_pos + 2 + setup_size != data_begin + audio_offset) {
        *error = "first audio packet does not follow the setup packet";
        return false;
    }

    std::vector<uint8_t> ident;
    {
        BitWriter w(ident);
        WriteHeaderStart(w, 1);
        w.Write(0, 32);
        w.Write(channels_, 8);
        w.Write(sample_rate_, 32);
        w.Write(0, 32);
        w.Write(info.avg_bytes_per_second * 8, 32);
        w.Write(0, 32);
        w.Write(blocksize_small_, 4);
        w.Write(blocksize_large_, 4);
        w.Write(1, 1);
    }
    std::vector<uint8_t> comment;
    {
        BitWriter w(comment);
        WriteHeaderStart(w, 3);
        const std::string_view vendor = "pt-port";
        w.Write(static_cast<uint32_t>(vendor.size()), 32);
        for (char c : vendor) {
            w.Write(static_cast<uint8_t>(c), 8);
        }
        w.Write(0, 32);
        w.Write(1, 1);
    }
    std::vector<uint8_t> setup;
    if (!BuildSetup(blob.subspan(setup_pos + 2, setup_size), setup, error)) {
        return false;
    }

    auto* vi = new vorbis_info;
    auto* vc = new vorbis_comment;
    vorbis_info_init(vi);
    vorbis_comment_init(vc);
    vorbis_info_ = vi;
    vorbis_comment_ = vc;
    ogg_packet op{};
    op.packet = ident.data();
    op.bytes = static_cast<long>(ident.size());
    op.b_o_s = 1;
    if (vorbis_synthesis_headerin(vi, vc, &op) != 0) {
        *error = "identification header rejected";
        return false;
    }
    op = {};
    op.packet = comment.data();
    op.bytes = static_cast<long>(comment.size());
    if (vorbis_synthesis_headerin(vi, vc, &op) != 0) {
        *error = "comment header rejected";
        return false;
    }
    op = {};
    op.packet = setup.data();
    op.bytes = static_cast<long>(setup.size());
    if (vorbis_synthesis_headerin(vi, vc, &op) != 0) {
        *error = "setup header rejected";
        return false;
    }
    {
        vorbis_dsp_state vd;
        if (vorbis_synthesis_init(&vd, vi) != 0) {
            *error = "vorbis_synthesis_init failed";
            return false;
        }
        vorbis_dsp_clear(&vd);
    }

    const uint32_t small = 1u << blocksize_small_;
    const uint32_t large = 1u << blocksize_large_;
    size_t pos = data_begin + audio_offset;
    uint64_t samples = 0;
    bool have_previous = false;
    bool previous_long = false;
    while (pos + 2 <= data_end) {
        Packet packet;
        packet.offset = static_cast<uint32_t>(pos);
        packet.size = ReadU16(blob.data() + pos);
        if (pos + 2 + packet.size > data_end) {
            break;
        }
        if (packet.size > 0) {
            packet.long_block = ModeBlockFlag(PacketMode(packet));
            packet.sample_start = samples;
            if (have_previous) {
                packet.sample_count = (previous_long ? large : small) / 4 + (packet.long_block ? large : small) / 4;
            }
            samples += packet.sample_count;
            have_previous = true;
            previous_long = packet.long_block;
        } else {
            packet.sample_start = samples;
        }
        packets_.push_back(packet);
        pos += 2 + packet.size;
    }
    if (samples < sample_count_) {
        sample_count_ = samples;
    }
    return true;
}

uint32_t WwiseVorbisStream::PacketMode(const Packet& packet) const {
    if (packet.size == 0) {
        return 0;
    }
    const uint8_t first = blob_[packet.offset + 2];
    const uint32_t mask = (1u << mode_bits_) - 1;
    return modified_packets_ ? (first & mask) : ((first >> 1) & mask);
}

size_t WwiseVorbisStream::RebuildPacket(size_t index, bool prev_long, std::vector<uint8_t>& out) const {
    out.clear();
    const Packet& packet = packets_[index];
    const uint8_t* payload = blob_.data() + packet.offset + 2;
    if (!modified_packets_) {
        out.assign(payload, payload + packet.size);
        return out.size();
    }
    out.reserve(packet.size + 2);
    BitWriter w(out);
    w.Write(0, 1);
    BitReader r(payload, packet.size);
    const uint32_t mode = r.Read(mode_bits_);
    w.Write(mode, mode_bits_);
    const uint32_t remainder = r.Read(8 - mode_bits_);
    if (ModeBlockFlag(mode)) {
        bool next_long = false;
        if (index + 1 < packets_.size() && packets_[index + 1].size > 0) {
            next_long = ModeBlockFlag(PacketMode(packets_[index + 1]));
        }
        w.Write(prev_long ? 1 : 0, 1);
        w.Write(next_long ? 1 : 0, 1);
    }
    w.Write(remainder, 8 - mode_bits_);
    for (uint32_t i = 1; i < packet.size; ++i) {
        w.WriteByte(payload[i]);
    }
    return out.size();
}

struct WwiseVorbisDecoder::State {
    vorbis_dsp_state vd{};
    vorbis_block vb{};
    bool ready = false;
};

WwiseVorbisDecoder::WwiseVorbisDecoder(const WwiseVorbisStream& stream) : stream_(stream), state_(std::make_unique<State>()) {
    auto* vi = static_cast<vorbis_info*>(stream_.VorbisInfo());
    if (vi && vorbis_synthesis_init(&state_->vd, vi) == 0) {
        vorbis_block_init(&state_->vd, &state_->vb);
        state_->ready = true;
    }
    packet_buffer_.reserve(8192);
}

WwiseVorbisDecoder::~WwiseVorbisDecoder() {
    if (state_->ready) {
        vorbis_block_clear(&state_->vb);
        vorbis_dsp_clear(&state_->vd);
    }
}

bool WwiseVorbisDecoder::DecodeNextPacket() {
    const auto& packets = stream_.Packets();
    while (next_packet_ < packets.size()) {
        const size_t index = next_packet_++;
        const auto& packet = packets[index];
        if (packet.size == 0) {
            continue;
        }
        const size_t bytes = stream_.RebuildPacket(index, prev_long_, packet_buffer_);
        prev_long_ = packet.long_block;
        ogg_packet op{};
        op.packet = packet_buffer_.data();
        op.bytes = static_cast<long>(bytes);
        op.granulepos = -1;
        op.packetno = static_cast<ogg_int64_t>(index + 3);
        if (vorbis_synthesis(&state_->vb, &op) != 0) {
            continue;
        }
        vorbis_synthesis_blockin(&state_->vd, &state_->vb);
        return true;
    }
    return false;
}

uint32_t WwiseVorbisDecoder::Read(float* out, uint32_t frames) {
    if (!state_->ready) {
        return 0;
    }
    const uint32_t channels = stream_.Channels();
    const uint64_t total = stream_.SampleCount();
    uint32_t produced = 0;
    while (produced < frames && position_ < total) {
        float** pcm = nullptr;
        const int available = vorbis_synthesis_pcmout(&state_->vd, &pcm);
        if (available <= 0) {
            if (!DecodeNextPacket()) {
                break;
            }
            continue;
        }
        if (skip_ > 0) {
            const int n = static_cast<int>(std::min<uint64_t>(skip_, static_cast<uint64_t>(available)));
            vorbis_synthesis_read(&state_->vd, n);
            skip_ -= static_cast<uint64_t>(n);
            continue;
        }
        const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>({static_cast<uint64_t>(available), frames - produced, total - position_}));
        for (uint32_t c = 0; c < channels; ++c) {
            const float* src = pcm[c];
            float* dst = out + static_cast<size_t>(produced) * channels + c;
            for (uint32_t i = 0; i < n; ++i) {
                dst[static_cast<size_t>(i) * channels] = src[i];
            }
        }
        vorbis_synthesis_read(&state_->vd, static_cast<int>(n));
        produced += n;
        position_ += n;
    }
    return produced;
}

bool WwiseVorbisDecoder::Seek(uint64_t frame) {
    if (!state_->ready) {
        return false;
    }
    const auto& packets = stream_.Packets();
    vorbis_synthesis_restart(&state_->vd);
    skip_ = 0;
    if (frame >= stream_.SampleCount()) {
        position_ = stream_.SampleCount();
        next_packet_ = packets.size();
        return true;
    }
    size_t target = static_cast<size_t>(
        std::upper_bound(packets.begin(), packets.end(), frame, [](uint64_t f, const WwiseVorbisStream::Packet& p) { return f < p.sample_start; }) -
        packets.begin());
    while (target > 0) {
        --target;
        const auto& p = packets[target];
        if (p.size > 0 && p.sample_count > 0) {
            break;
        }
    }
    if (target >= packets.size() || packets[target].sample_count == 0 || frame >= packets[target].sample_start + packets[target].sample_count) {
        position_ = stream_.SampleCount();
        next_packet_ = packets.size();
        return true;
    }
    size_t previous = target;
    while (previous > 0) {
        --previous;
        if (packets[previous].size > 0) {
            break;
        }
    }
    size_t before = previous;
    bool before_long = false;
    while (before > 0) {
        --before;
        if (packets[before].size > 0) {
            before_long = packets[before].long_block;
            break;
        }
    }
    next_packet_ = previous;
    prev_long_ = before_long;
    skip_ = frame - packets[target].sample_start;
    position_ = frame;
    return true;
}

}
