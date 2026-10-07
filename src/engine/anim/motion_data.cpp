#include "engine/anim/motion_data.h"

#include <algorithm>
#include <cstring>

#include "engine/core/strcode.h"

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

std::string CString(std::span<const uint8_t> data, size_t offset) {
    std::string out;
    while (offset < data.size() && data[offset] != 0 && out.size() < 256) {
        out.push_back(static_cast<char>(data[offset++]));
    }
    return out;
}

void ParseParams(std::span<const uint8_t> data, size_t offset, std::vector<NodeParam>& out) {
    for (int guard = 0; guard < 64 && offset + 16 <= data.size(); ++guard) {
        NodeParam p;
        p.type = At<uint16_t>(data, offset);
        const uint16_t next = At<uint16_t>(data, offset + 2);
        p.key = At<uint32_t>(data, offset + 4);
        const uint32_t key_offset = At<uint32_t>(data, offset + 8);
        if (key_offset) {
            p.key_name = CString(data, offset + 4 + key_offset);
        }
        if (p.type == 1) {
            p.value_hash = At<uint32_t>(data, offset + 12);
            const uint32_t value_offset = At<uint32_t>(data, offset + 16);
            if (value_offset) {
                p.text = CString(data, offset + 12 + value_offset);
            }
        } else if (p.type == 2) {
            p.number = At<float>(data, offset + 12);
        }
        out.push_back(std::move(p));
        if (!next) {
            return;
        }
        offset += next;
    }
}

void ParseNameTable(std::span<const uint8_t> data, size_t offset, std::vector<NameEntry>& out) {
    const uint32_t count = At<uint32_t>(data, offset);
    for (uint32_t i = 0; i < count && i < 4096; ++i) {
        const size_t e = offset + 4 + 8 * static_cast<size_t>(i);
        NameEntry entry;
        entry.hash = At<uint32_t>(data, e);
        const uint32_t text = At<uint32_t>(data, e + 4);
        if (text) {
            entry.name = CString(data, e + text);
        }
        out.push_back(std::move(entry));
    }
}

bool VisitNodes(std::span<const uint8_t> data, size_t offset, int depth, int parent, std::vector<MotionNode>& nodes) {
    for (int guard = 0; guard < 65536; ++guard) {
        if (offset + 0x30 > data.size() || depth > 64 || nodes.size() > 100000) {
            return false;
        }
        MotionNode node;
        node.offset = offset;
        node.hash = At<uint32_t>(data, offset);
        const uint32_t name_offset = At<uint32_t>(data, offset + 4);
        node.data_type = At<uint32_t>(data, offset + 8);
        node.data_offset = At<int32_t>(data, offset + 0x0C);
        node.data_size = At<uint32_t>(data, offset + 0x10);
        const int32_t child = At<int32_t>(data, offset + 0x18);
        const int32_t next = At<int32_t>(data, offset + 0x20);
        const uint32_t extra_size = At<uint32_t>(data, offset + 0x24);
        node.name = name_offset ? CString(data, offset + name_offset) : std::string(KnownNodeName(node.hash));
        node.depth = depth;
        node.parent = parent;
        const size_t body = offset + (name_offset ? 0x40 : 0x30);
        size_t params_at = body;
        const size_t data_at = offset + static_cast<size_t>(static_cast<int64_t>(node.data_offset));
        const uint32_t unit_count = node.data_type && node.data_offset ? At<uint32_t>(data, data_at) : 0;
        if (node.data_type && unit_count > 0 && unit_count < 4096 && data_at + 20 + 4 * static_cast<size_t>(unit_count) <= data.size()) {
            UnitsTable table;
            if (ParseUnitsTable(data, data_at, table)) {
                params_at = (table.end + 15) & ~size_t(15);
                node.units = std::move(table);
            }
        } else if (node.data_type) {
            node.raw_data = data_at;
        } else if (node.data_offset && node.data_size) {
            ParseNameTable(data, data_at, node.name_table);
        }
        if (extra_size && params_at < body + extra_size) {
            ParseParams(data, params_at, node.params);
        }
        const int index = static_cast<int>(nodes.size());
        nodes.push_back(std::move(node));
        if (parent >= 0) {
            nodes[static_cast<size_t>(parent)].children.push_back(index);
        }
        if (child) {
            if (!VisitNodes(data, offset + static_cast<size_t>(static_cast<int64_t>(child)), depth + 1, index, nodes)) {
                return false;
            }
        }
        if (!next) {
            return true;
        }
        offset += static_cast<size_t>(static_cast<int64_t>(next));
    }
    return false;
}

struct ParamTypeInfo {
    ParamType type;
    int components;
    int array;
};

ParamTypeInfo TypeInfo(uint16_t raw) {
    switch (raw) {
    case 0x04:
    case 0x05: return {ParamType::Int, 1, 0};
    case 0x0A: return {ParamType::Bool, 1, 0};
    case 0x08: return {ParamType::Float, 1, 1};
    case 0x0B:
    case 0x16: return {ParamType::String, 1, 2};
    case 0x0E:
    case 0x020E:
    case 0x040E: return {ParamType::Vector3, 3, 1};
    case 0x0F:
    case 0x10: return {ParamType::Vector4, 4, 1};
    case 0x0210: return {ParamType::Quat, 4, 1};
    case 0x13: return {ParamType::Color, 4, 1};
    case 0x14: return {ParamType::File, 1, 2};
    default: return {ParamType::Unknown, 1, 0};
    }
}

struct Footer {
    uint32_t count = 0;
    uint32_t interpolated = 0;
    uint32_t curve = 0;
    int32_t length = -1;
    uint32_t words = 0;
};

bool ParseFooter(const std::vector<int32_t>& ints_signed, Footer& out) {
    const size_t n = ints_signed.size();
    auto u = [&](size_t back) { return static_cast<uint32_t>(ints_signed[n - back]); };
    if (n < 2 || (u(1) & 0xFFFF) != 6) {
        return false;
    }
    const uint32_t flags = u(1) >> 16;
    if (flags & 2) {
        if (n >= 5 && (u(2) & 0xFFFF) == 4) {
            out = {u(5) >> 16, 0, 0, ints_signed[n - 3], 5};
            return true;
        }
        return false;
    }
    const uint32_t tag = u(2) & 0xFFFF;
    const uint32_t blocks = u(2) >> 16;
    if (tag != 2) {
        return false;
    }
    if (blocks == 0) {
        out = {0, 0, 0, -1, 2};
        return true;
    }
    if (blocks == 1 && n >= 3 && ((u(3) & 0xFFFF) == 3 || (u(3) & 0xFFFF) == 5)) {
        out = {u(3) >> 16, 0, 0, -1, 3};
        return true;
    }
    if (blocks == 2 && n >= 4 && (u(3) & 0xFFFF) == 4 && (u(4) >> 24) == 1) {
        const uint32_t k = (u(4) >> 16) & 0xFF;
        out = {k + (u(3) >> 16), k, u(4) & 0xFFFF, -1, 4};
        return true;
    }
    return false;
}

}

uint32_t StrCode32(std::string_view text) {
    return static_cast<uint32_t>(StrCode64(text) & 0xFFFFFFFFull);
}

std::string_view KnownNodeName(uint32_t hash) {
    static const std::vector<std::pair<uint32_t, std::string_view>> kNames = [] {
        std::vector<std::pair<uint32_t, std::string_view>> names;
        for (std::string_view n : {"ROOT", "DEMO", "CAMERA", "MOVE", "CameraParam", "SI Frame", "LOCATOR", "MESH_EVENT", "MOTION", "SKEL", "MODEL",
                                   "SKELINFO", "MTPINFO", "MTEV", "SHADER", "MTP", "MTP_LIST", "MTP_PARENT_LIST", "UNIT", "SKL_LIST", "Transform"}) {
            names.emplace_back(StrCode32(n), n);
        }
        return names;
    }();
    for (const auto& [h, n] : kNames) {
        if (h == hash) {
            return n;
        }
    }
    return {};
}

const NodeParam* MotionNode::Param(uint32_t key) const {
    for (const NodeParam& p : params) {
        if (p.key == key) {
            return &p;
        }
    }
    return nullptr;
}

std::string MotionNode::TargetName() const {
    const NodeParam* p = Param(kHashTargetName);
    return p ? p->text : std::string();
}

bool ParseUnitsTable(std::span<const uint8_t> data, size_t offset, UnitsTable& out) {
    out.unit_count = At<uint32_t>(data, offset);
    out.track_count = At<uint32_t>(data, offset + 4);
    out.rig_word = At<uint32_t>(data, offset + 8);
    out.frames = At<uint32_t>(data, offset + 12);
    out.ticks_per_frame = At<uint32_t>(data, offset + 16);
    out.end = offset + 20 + 4 * static_cast<size_t>(out.unit_count);
    if (out.end > data.size()) {
        return false;
    }
    for (uint32_t i = 0; i < out.unit_count; ++i) {
        const size_t u = offset + At<uint32_t>(data, offset + 20 + 4 * static_cast<size_t>(i));
        if (u + 8 > data.size()) {
            return false;
        }
        UnitDesc unit;
        unit.hash = At<uint32_t>(data, u);
        const uint8_t count = At<uint8_t>(data, u + 4);
        unit.flags = At<uint8_t>(data, u + 5);
        for (uint8_t t = 0; t < count; ++t) {
            const size_t e = u + 8 + 8 * static_cast<size_t>(t);
            TrackDesc track;
            const int32_t data_offset = At<int32_t>(data, e);
            track.index = At<uint16_t>(data, e + 4);
            const uint8_t kind_byte = At<uint8_t>(data, e + 6);
            track.kind = kind_byte & 0x0F;
            track.more = (kind_byte & 0x80) != 0;
            track.bits = At<uint8_t>(data, e + 7);
            track.data_offset = data_offset ? static_cast<int64_t>(e) + data_offset : -1;
            unit.tracks.push_back(track);
        }
        out.end = std::max(out.end, u + 8 + 8 * static_cast<size_t>(count));
        out.units.push_back(std::move(unit));
    }
    return true;
}

bool ParseMotionTree(std::span<const uint8_t> data, size_t root, std::vector<MotionNode>& nodes) {
    nodes.clear();
    return VisitNodes(data, root, 0, -1, nodes);
}

const ExecParam* StreamEvent::Param(uint32_t key) const {
    for (const ExecParam& p : params) {
        if (p.key == key) {
            return &p;
        }
    }
    return nullptr;
}

uint64_t StreamEvent::String(uint32_t key, uint64_t fallback) const {
    const ExecParam* p = Param(key);
    if (!p || (p->type != ParamType::String && p->type != ParamType::File) || p->index >= strings.size()) {
        return fallback;
    }
    return strings[p->index];
}

int32_t StreamEvent::Int(uint32_t key, int32_t fallback) const {
    const ExecParam* p = Param(key);
    if (!p || (p->type != ParamType::Int && p->type != ParamType::Bool) || p->index >= raw_int_count || p->index >= ints.size()) {
        return fallback;
    }
    return ints[p->index];
}

bool StreamEvent::Bool(uint32_t key, bool fallback) const {
    const ExecParam* p = Param(key);
    if (!p) {
        return fallback;
    }
    return Int(key, fallback ? 1 : 0) != 0;
}

float StreamEvent::Float(uint32_t key, float fallback, bool end_value) const {
    const ExecParam* p = Param(key);
    if (!p || p->type != ParamType::Float) {
        return fallback;
    }
    const size_t i = p->index + ((end_value && p->interpolated) ? 1u : 0u);
    return i < floats.size() ? floats[i] : fallback;
}

glm::vec4 StreamEvent::Vector(uint32_t key, glm::vec4 fallback, bool end_value) const {
    const ExecParam* p = Param(key);
    if (!p || (p->type != ParamType::Vector3 && p->type != ParamType::Vector4 && p->type != ParamType::Quat && p->type != ParamType::Color &&
               p->type != ParamType::Float)) {
        return fallback;
    }
    glm::vec4 out = fallback;
    const size_t base = p->index + ((end_value && p->interpolated) ? static_cast<size_t>(p->components) : 0u);
    for (int c = 0; c < p->components && c < 4; ++c) {
        if (base + c < floats.size()) {
            out[c] = floats[base + c];
        }
    }
    return out;
}

bool DecodeEvent(std::span<const uint8_t> data, size_t offset, StreamEvent& out) {
    if (offset + 8 > data.size()) {
        return false;
    }
    out = StreamEvent{};
    out.type = At<uint32_t>(data, offset);
    const uint8_t info = At<uint8_t>(data, offset + 4);
    const uint32_t n_ints = At<uint8_t>(data, offset + 5);
    const uint32_t n_floats = At<uint8_t>(data, offset + 6);
    const uint32_t n_strings = At<uint8_t>(data, offset + 7);
    const uint32_t n_sections = info & 0x3F;
    const uint32_t size_class = info >> 6;
    static constexpr uint32_t kElement[4] = {8, 4, 2, 0};
    for (uint32_t i = 0; i < n_sections; ++i) {
        int32_t s = -1;
        int32_t e = -1;
        if (size_class == 0) {
            s = At<int32_t>(data, offset + 8 + 8 * i);
            e = At<int32_t>(data, offset + 12 + 8 * i);
        } else if (size_class == 1) {
            s = At<int16_t>(data, offset + 8 + 4 * i);
            e = At<int16_t>(data, offset + 10 + 4 * i);
        } else if (size_class == 2) {
            s = At<int8_t>(data, offset + 8 + 2 * i);
            e = At<int8_t>(data, offset + 9 + 2 * i);
        }
        EventSection section;
        section.start = s < 0 ? s : (s & ~0x40000000);
        section.start_outside = s >= 0 && (s & 0x40000000);
        section.end = e < 0 ? e : (e & ~0x40000000);
        section.end_outside = e >= 0 && (e & 0x40000000);
        out.sections.push_back(section);
    }
    const size_t p = offset + ((n_sections * kElement[size_class] + 0xB) & 0x7FC);
    for (uint32_t i = 0; i < n_ints; ++i) {
        out.ints.push_back(At<int32_t>(data, p + 4 * i));
    }
    for (uint32_t i = 0; i < n_floats; ++i) {
        out.floats.push_back(At<float>(data, p + 4 * (n_ints + i)));
    }
    for (uint32_t i = 0; i < n_strings; ++i) {
        out.strings.push_back(At<uint64_t>(data, p + 4 * (n_ints + n_floats) + 8 * i) & 0xFFFFFFFFFFFFull);
    }
    out.raw_int_count = n_ints;
    if (out.type == kEventExecCommand) {
        DecodeExecCommand(out);
    }
    return true;
}

void DecodeExecCommand(StreamEvent& event) {
    event.exec = true;
    event.params.clear();
    event.raw_int_count = static_cast<uint32_t>(event.ints.size());
    if (event.strings.size() >= 5) {
        const size_t n = event.strings.size();
        event.functor = event.strings[n - 2];
        event.category = event.strings[n - 1];
        event.clip = event.strings[n - 5];
    }
    if (event.ints.empty()) {
        return;
    }
    const uint32_t last = static_cast<uint32_t>(event.ints.back());
    event.footer_version = static_cast<uint16_t>(last & 0xFFFF);
    event.footer_flags = static_cast<uint16_t>(last >> 16);
    Footer footer;
    if (!ParseFooter(event.ints, footer)) {
        return;
    }
    const int64_t first = static_cast<int64_t>(event.ints.size()) - footer.words - 2 * static_cast<int64_t>(footer.count);
    if (first < 0) {
        return;
    }
    event.footer_known = true;
    event.raw_int_count = static_cast<uint32_t>(first);
    event.interpolated = footer.interpolated;
    event.curve = footer.curve;
    event.length = footer.length;
    for (uint32_t n = 0; n < footer.count; ++n) {
        const size_t at = static_cast<size_t>(first) + 2 * n;
        const uint32_t word = static_cast<uint32_t>(event.ints[at + 1]);
        ExecParam param;
        param.key = static_cast<uint32_t>(event.ints[at]);
        param.raw_type = static_cast<uint16_t>(word >> 16);
        param.index = static_cast<uint16_t>(word & 0xFFFF);
        const ParamTypeInfo info = TypeInfo(param.raw_type);
        param.type = info.type;
        param.components = info.components;
        param.interpolated = n < footer.interpolated;
        event.params.push_back(param);
    }
}

bool ParseEventTable(std::span<const uint8_t> data, size_t table, uint32_t& table_hash, std::vector<StreamEvent>& out) {
    if (table + 8 > data.size()) {
        return false;
    }
    table_hash = At<uint32_t>(data, table);
    const uint32_t count = At<uint16_t>(data, table + 4);
    for (uint32_t i = 0; i < count; ++i) {
        const int32_t record = At<int32_t>(data, table + 8 + 4 * static_cast<size_t>(i));
        StreamEvent event;
        if (DecodeEvent(data, table + static_cast<size_t>(static_cast<int64_t>(record)), event)) {
            out.push_back(std::move(event));
        }
    }
    return true;
}

std::string_view EventTypeName(uint32_t type) {
    switch (type) {
    case kEventExecCommand: return "ExecCommand";
    case kEventDemoStart: return "DemoStart";
    case kEventDemoEnd: return "DemoEnd";
    case kEventClipEnd: return "ClipEnd";
    case kEventCreateCamera: return "CreateCamera";
    case kEventDeleteCamera: return "DeleteCamera";
    case kEventCreateModel: return "CreateModel";
    case kEventDeleteModel: return "DeleteModel";
    case kEventCreateLocator: return "CreateLocator";
    case kEventDeleteLocator: return "DeleteLocator";
    case kEventVisibleModel: return "VisibleModel";
    case kEventVisibleMesh: return "VisibleMesh";
    default: return {};
    }
}

}
