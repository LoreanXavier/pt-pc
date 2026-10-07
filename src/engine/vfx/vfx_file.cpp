#include "engine/vfx/vfx_file.h"

#include <cstring>
#include <format>

#include "engine/core/strcode.h"

namespace pt::vfx {

namespace {

constexpr size_t kHeaderSize = 15;

class Reader {
public:
    explicit Reader(std::span<const uint8_t> data) : data_(data) {}

    bool Has(size_t n) const { return pos_ + n <= data_.size(); }
    size_t Pos() const { return pos_; }

    template <typename T>
    T Read() {
        T v{};
        std::memcpy(&v, data_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return v;
    }

    std::string ReadString(size_t n) {
        std::string s(reinterpret_cast<const char*>(data_.data() + pos_), n);
        pos_ += n;
        return s;
    }

private:
    std::span<const uint8_t> data_;
    size_t pos_ = 0;
};

bool Fail(std::string* error, std::string text) {
    if (error) {
        *error = std::move(text);
    }
    return false;
}

}

uint32_t NameHash(std::string_view name) {
    return static_cast<uint32_t>(StrCode64(name));
}

size_t Prop::Count() const {
    switch (type) {
    case PropType::Float:
    case PropType::Vector4:
        return vectors.size();
    case PropType::String:
        return strings.size();
    default:
        return ints.size();
    }
}

const Prop* Node::Find(uint32_t hash) const {
    for (const Prop& p : props) {
        if (p.hash == hash) {
            return &p;
        }
    }
    return nullptr;
}

float Node::Float(uint32_t hash, float fallback, size_t i) const {
    const Prop* p = Find(hash);
    if (!p) {
        return fallback;
    }
    if (p->type == PropType::Float || p->type == PropType::Vector4) {
        return i < p->vectors.size() ? p->vectors[i].x : fallback;
    }
    return i < p->ints.size() ? static_cast<float>(p->ints[i]) : fallback;
}

uint32_t Node::UInt(uint32_t hash, uint32_t fallback, size_t i) const {
    const Prop* p = Find(hash);
    if (!p || i >= p->ints.size()) {
        return fallback;
    }
    return static_cast<uint32_t>(p->ints[i]);
}

glm::vec4 Node::Vec4(uint32_t hash, const glm::vec4& fallback) const {
    const Prop* p = Find(hash);
    return p && !p->vectors.empty() ? p->vectors[0] : fallback;
}

std::string Node::String(uint32_t hash) const {
    const Prop* p = Find(hash);
    return p && !p->strings.empty() ? p->strings[0] : std::string();
}

uint64_t Node::Code(uint32_t hash) const {
    const Prop* p = Find(hash);
    return p && !p->ints.empty() ? p->ints[0] : 0;
}

std::vector<float> Node::Floats(std::string_view name) const {
    std::vector<float> out;
    if (const Prop* p = Find(name)) {
        for (const glm::vec4& v : p->vectors) {
            out.push_back(v.x);
        }
    }
    return out;
}

const Node* File::Input(uint32_t node, uint8_t port) const {
    for (const Edge& e : edges) {
        if (e.to == node && e.to_port == port && e.from < nodes.size()) {
            return &nodes[e.from];
        }
    }
    return nullptr;
}

bool ParseFile(std::span<const uint8_t> data, File& out, std::string* error) {
    out = File{};
    if (data.size() < kHeaderSize + 1 || std::memcmp(data.data(), "vfx", 3) != 0) {
        return Fail(error, "not a vfx file");
    }
    Reader r(data);
    r.ReadString(3);
    out.version = r.Read<uint16_t>();
    const uint16_t node_count = r.Read<uint16_t>();
    const uint16_t edge_count = r.Read<uint16_t>();
    r.ReadString(6);
    out.nodes.reserve(node_count);
    for (uint16_t n = 0; n < node_count; ++n) {
        if (!r.Has(8)) {
            return Fail(error, std::format("node {} truncated", n));
        }
        const uint64_t code = r.Read<uint64_t>();
        const ClassDef* def = FindClass(static_cast<uint32_t>(code));
        if (!def) {
            return Fail(error, std::format("node {} at {:#x}: unknown class {:#x}", n, r.Pos() - 8, code));
        }
        Node node;
        node.def = def;
        node.index = n;
        node.props.reserve(def->props.size());
        for (const PropDef& pd : def->props) {
            if (!r.Has(1)) {
                return Fail(error, std::format("{} truncated", def->name));
            }
            Prop prop;
            prop.hash = pd.hash;
            prop.type = static_cast<PropType>(pd.type);
            const uint8_t count = r.Read<uint8_t>();
            for (uint8_t i = 0; i < count; ++i) {
                switch (prop.type) {
                case PropType::Bool:
                    if (!r.Has(1)) {
                        return Fail(error, "bool truncated");
                    }
                    prop.ints.push_back(r.Read<uint8_t>());
                    break;
                case PropType::UInt32:
                    if (!r.Has(4)) {
                        return Fail(error, "uint truncated");
                    }
                    prop.ints.push_back(r.Read<uint32_t>());
                    break;
                case PropType::Float:
                    if (!r.Has(4)) {
                        return Fail(error, "float truncated");
                    }
                    prop.vectors.emplace_back(r.Read<float>(), 0.0f, 0.0f, 0.0f);
                    break;
                case PropType::Vector4: {
                    if (!r.Has(16)) {
                        return Fail(error, "vector truncated");
                    }
                    glm::vec4 v;
                    v.x = r.Read<float>();
                    v.y = r.Read<float>();
                    v.z = r.Read<float>();
                    v.w = r.Read<float>();
                    prop.vectors.push_back(v);
                    break;
                }
                case PropType::String: {
                    if (!r.Has(2)) {
                        return Fail(error, "string truncated");
                    }
                    const uint16_t length = r.Read<uint16_t>();
                    if (!r.Has(size_t(length) + 1)) {
                        return Fail(error, "string truncated");
                    }
                    prop.strings.push_back(r.ReadString(length));
                    r.Read<uint8_t>();
                    break;
                }
                case PropType::StrCode:
                case PropType::PathCode:
                    if (!r.Has(8)) {
                        return Fail(error, "code truncated");
                    }
                    prop.ints.push_back(r.Read<uint64_t>());
                    break;
                }
            }
            node.props.push_back(std::move(prop));
        }
        out.nodes.push_back(std::move(node));
    }
    const bool wide = node_count >= 0xFF;
    for (uint16_t i = 0; i < edge_count; ++i) {
        if (!r.Has(wide ? 8 : 6)) {
            return Fail(error, "edges truncated");
        }
        Edge e;
        if (wide) {
            e.from = r.Read<uint16_t>();
            e.to = r.Read<uint16_t>();
        } else {
            e.from = r.Read<uint8_t>();
            e.to = r.Read<uint8_t>();
        }
        e.from_type = r.Read<uint8_t>();
        e.from_port = r.Read<uint8_t>();
        e.to_type = r.Read<uint8_t>();
        e.to_port = r.Read<uint8_t>();
        if (e.from >= node_count || e.to >= node_count) {
            return Fail(error, "edge node index out of range");
        }
        out.edges.push_back(e);
    }
    if (r.Pos() != data.size()) {
        return Fail(error, std::format("{} trailing bytes", data.size() - r.Pos()));
    }
    return true;
}

bool ReadSoundNode(std::span<const uint8_t> data, SoundNode& out) {
    File file;
    if (!ParseFile(data, file)) {
        return false;
    }
    for (const Node& node : file.nodes) {
        if (node.ClassName() != "FxSoundCallProgramEffectNode") {
            continue;
        }
        out.play = node.String("soundEvent");
        out.stop = node.String("soundStop");
        out.stop_playing = node.Bool(0x37CD447Cu);
        out.fade = node.Float(0xD2ECAC68u);
        out.curve = node.UInt(0xE3A9CADAu, 4u);
        if (!out.play.empty()) {
            return true;
        }
    }
    return false;
}

}
