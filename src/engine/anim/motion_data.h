#pragma once

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pt::anim {

uint32_t StrCode32(std::string_view text);

constexpr uint32_t kHashTargetName = 0x9932327B;
constexpr uint32_t kHashSlopeDir = 0xCC39A1F6;
constexpr uint32_t kHashSlopeAngle = 0x021922A7;
constexpr uint32_t kHashSklList = 0x91E4534B;
constexpr uint32_t kHashUnit = 0xC6E937B9;
constexpr uint32_t kHashGaniEvents = 0x1622762D;
constexpr uint32_t kHashModelRootUnit = 0x01AD535D;
constexpr uint32_t kHashRigRoot = 0xD3C3FF90;

struct TrackDesc {
    uint16_t index = 0;
    uint8_t kind = 0;
    uint8_t bits = 0;
    bool more = false;
    int64_t data_offset = -1;
};

struct UnitDesc {
    uint32_t hash = 0;
    uint8_t flags = 0;
    std::vector<TrackDesc> tracks;
};

struct UnitsTable {
    uint32_t unit_count = 0;
    uint32_t track_count = 0;
    uint32_t rig_word = 0;
    uint32_t frames = 0;
    uint32_t ticks_per_frame = 5;
    std::vector<UnitDesc> units;
    size_t end = 0;
};

struct NodeParam {
    uint32_t key = 0;
    std::string key_name;
    uint16_t type = 0;
    uint32_t value_hash = 0;
    std::string text;
    float number = 0.0f;
};

struct NameEntry {
    uint32_t hash = 0;
    std::string name;
};

struct MotionNode {
    size_t offset = 0;
    uint32_t hash = 0;
    std::string name;
    uint32_t data_type = 0;
    int32_t data_offset = 0;
    uint32_t data_size = 0;
    int depth = 0;
    int parent = -1;
    std::vector<int> children;
    std::optional<UnitsTable> units;
    std::vector<NodeParam> params;
    std::vector<NameEntry> name_table;
    size_t raw_data = 0;

    const NodeParam* Param(uint32_t key) const;
    std::string TargetName() const;
};

bool ParseUnitsTable(std::span<const uint8_t> data, size_t offset, UnitsTable& out);
bool ParseMotionTree(std::span<const uint8_t> data, size_t root, std::vector<MotionNode>& nodes);
std::string_view KnownNodeName(uint32_t hash);

enum class ParamType : uint8_t { Int, Bool, Float, String, Vector3, Vector4, Quat, Color, File, Unknown };

struct ExecParam {
    uint32_t key = 0;
    uint16_t raw_type = 0;
    ParamType type = ParamType::Unknown;
    uint16_t index = 0;
    int components = 1;
    bool interpolated = false;
};

struct EventSection {
    int32_t start = -1;
    int32_t end = -1;
    bool start_outside = false;
    bool end_outside = false;
};

struct StreamEvent {
    uint32_t type = 0;
    std::vector<EventSection> sections;
    std::vector<int32_t> ints;
    std::vector<float> floats;
    std::vector<uint64_t> strings;
    bool exec = false;
    bool footer_known = false;
    uint16_t footer_version = 0;
    uint16_t footer_flags = 0;
    uint64_t functor = 0;
    uint64_t category = 0;
    uint64_t clip = 0;
    uint32_t interpolated = 0;
    uint32_t curve = 0;
    int32_t length = -1;
    uint32_t raw_int_count = 0;
    std::vector<ExecParam> params;

    int32_t Start() const { return sections.empty() ? 0 : sections[0].start; }
    int32_t End() const { return sections.empty() ? -1 : sections[0].end; }
    bool Continuation() const { return !sections.empty() && sections[0].start_outside; }
    const ExecParam* Param(uint32_t key) const;
    bool Has(uint32_t key) const { return Param(key) != nullptr; }
    uint64_t String(uint32_t key, uint64_t fallback = 0) const;
    int32_t Int(uint32_t key, int32_t fallback = 0) const;
    bool Bool(uint32_t key, bool fallback = false) const;
    float Float(uint32_t key, float fallback = 0.0f, bool end_value = false) const;
    glm::vec4 Vector(uint32_t key, glm::vec4 fallback = glm::vec4(0.0f), bool end_value = false) const;
    uint64_t StringArg(size_t index) const { return index < strings.size() ? strings[index] : 0; }
};

bool DecodeEvent(std::span<const uint8_t> data, size_t offset, StreamEvent& out);
void DecodeExecCommand(StreamEvent& event);
bool ParseEventTable(std::span<const uint8_t> data, size_t table, uint32_t& table_hash, std::vector<StreamEvent>& out);

constexpr uint32_t kEventExecCommand = 0x2379C011;
constexpr uint32_t kEventDemoStart = 0xD3185DFF;
constexpr uint32_t kEventDemoEnd = 0xD2A6999A;
constexpr uint32_t kEventClipEnd = 0x2A36C514;
constexpr uint32_t kEventCreateCamera = 0x7F4D8E71;
constexpr uint32_t kEventDeleteCamera = 0xF4603510;
constexpr uint32_t kEventCreateModel = 0xC5806267;
constexpr uint32_t kEventDeleteModel = 0x75586EE7;
constexpr uint32_t kEventCreateLocator = 0x966D8EC3;
constexpr uint32_t kEventDeleteLocator = 0x3148F136;
constexpr uint32_t kEventVisibleModel = 0x3E09E7B9;
constexpr uint32_t kEventVisibleMesh = 0x6B491E18;

std::string_view EventTypeName(uint32_t type);

}
