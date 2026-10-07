#include "engine/ui/uigb.h"

#include <cstring>
#include <optional>
#include <unordered_map>

#include "engine/core/strcode.h"

namespace pt::ui {
namespace {

constexpr uint32_t kNone = 0xFFFFFFFFu;

template <typename T>
T Read(std::span<const uint8_t> d, size_t at) {
    T v;
    std::memcpy(&v, d.data() + at, sizeof(T));
    return v;
}

struct Record {
    size_t at = 0;
    uint64_t type = 0;
    uint16_t name = 0;
    uint8_t size = 0;
    uint8_t input_count = 0;
    uint32_t inputs = kNone;
    uint8_t param_size = 0;
    uint32_t properties = kNone;
    uint32_t params = kNone;
};

}

bool UigbGraph::Parse(std::span<const uint8_t> d, std::string* error) {
    auto fail = [&](const char* message) {
        if (error) {
            *error = message;
        }
        return false;
    };
    layouts_.clear();
    start_.clear();
    events_.clear();
    if (d.size() < 0x30 || std::memcmp(d.data(), "UIGB", 4) != 0) {
        return fail("not a UIGB file");
    }
    const uint16_t node_count = Read<uint16_t>(d, 0x08);
    const uint16_t name_count = Read<uint16_t>(d, 0x10);
    const uint16_t string_count = Read<uint16_t>(d, 0x12);
    const uint32_t nodes = Read<uint32_t>(d, 0x14);
    const uint32_t data_size = Read<uint32_t>(d, 0x24);
    const uint32_t strings = Read<uint32_t>(d, 0x28);
    const uint32_t base = Read<uint32_t>(d, 0x2C);
    auto in_range = [&](size_t at, size_t bytes) { return at + bytes <= d.size(); };
    if (!in_range(static_cast<size_t>(base) + data_size, name_count * 8ull) || !in_range(strings, string_count * 8ull)) {
        return fail("UIGB tables out of range");
    }
    std::vector<uint64_t> names;
    for (uint16_t i = 0; i < name_count; ++i) {
        names.push_back(Read<uint64_t>(d, base + data_size + i * 8ull) & kStrCode64Mask);
    }
    for (uint16_t i = 0; i < string_count; ++i) {
        const uint32_t length = Read<uint32_t>(d, strings + i * 8ull);
        const uint32_t offset = Read<uint32_t>(d, strings + i * 8ull + 4);
        if (!in_range(static_cast<size_t>(base) + offset, length)) {
            return fail("UIGB string out of range");
        }
        const char* text = reinterpret_cast<const char*>(d.data() + base + offset);
        std::string path(text, strnlen(text, length));
        if (path.ends_with(".uilb")) {
            layouts_.push_back(std::move(path));
        }
    }
    auto name_at = [&](uint32_t index) { return index < names.size() ? names[index] : 0; };
    std::vector<Record> records;
    std::unordered_map<uint16_t, size_t> by_name;
    size_t at = nodes;
    for (uint16_t i = 0; i < node_count; ++i) {
        if (!in_range(at, 8)) {
            return fail("UIGB node table out of range");
        }
        Record r;
        r.at = at;
        r.type = name_at(Read<uint16_t>(d, at));
        r.name = Read<uint16_t>(d, at + 2);
        r.size = d[at + 4];
        r.input_count = d[at + 7];
        if (r.size < 8 || !in_range(at, r.size)) {
            return fail("UIGB node out of range");
        }
        if (r.size >= 0x0C) {
            r.inputs = Read<uint32_t>(d, at + 0x08);
        }
        if (r.size >= 0x10) {
            r.param_size = d[at + 0x0D];
        }
        if (r.size >= 0x14) {
            r.properties = Read<uint32_t>(d, at + 0x10);
        }
        if (r.size >= 0x18) {
            r.params = Read<uint32_t>(d, at + 0x14);
        }
        by_name[r.name] = records.size();
        records.push_back(r);
        at += r.size;
    }
    std::vector<std::vector<size_t>> consumers(records.size());
    for (size_t i = 0; i < records.size(); ++i) {
        const Record& r = records[i];
        if (r.inputs == kNone || !in_range(r.inputs, r.input_count * 8ull)) {
            continue;
        }
        for (uint8_t k = 0; k < r.input_count; ++k) {
            auto it = by_name.find(Read<uint16_t>(d, r.inputs + k * 8ull));
            if (it != by_name.end()) {
                consumers[it->second].push_back(i);
            }
        }
    }
    auto param_at = [&](const Record& r, size_t offset, size_t bytes) -> std::optional<size_t> {
        if (r.params == kNone || offset + bytes > r.param_size || !in_range(static_cast<size_t>(base) + r.params + offset, bytes)) {
            return std::nullopt;
        }
        return static_cast<size_t>(base) + r.params + offset;
    };
    auto property_name = [&](const Record& r) -> uint64_t {
        return r.properties != kNone && in_range(r.properties, 8) ? name_at(Read<uint16_t>(d, r.properties)) : 0;
    };
    auto action_of = [&](const Record& r) -> std::optional<UigbAction> {
        UigbAction action;
        if (r.type == kNodePlayAnimation) {
            action.kind = UigbActionKind::PlayAnimation;
            action.animation = property_name(r);
            if (r.properties != kNone && in_range(r.properties, 8)) {
                const uint32_t model = Read<uint32_t>(d, r.properties + 4);
                if (model + 2ull <= data_size) {
                    action.model = name_at(Read<uint16_t>(d, base + model));
                }
            }
            if (auto speed = param_at(r, 0x0C, 4)) {
                action.speed = Read<float>(d, *speed);
            }
            if (auto loop = param_at(r, 0x14, 4)) {
                action.loop = Read<uint32_t>(d, *loop) != 0;
            }
            return action.animation ? std::optional<UigbAction>(action) : std::nullopt;
        }
        if (r.type == kNodeSetVisible) {
            action.kind = UigbActionKind::SetVisible;
            action.layout = property_name(r);
            auto target = param_at(r, 0, 8);
            auto value = param_at(r, 8, 4);
            if (!target || !value) {
                return std::nullopt;
            }
            action.target = Read<uint64_t>(d, *target) & kStrCode64Mask;
            action.visible = Read<uint32_t>(d, *value) != 0;
            return action;
        }
        return std::nullopt;
    };
    for (size_t i = 0; i < records.size(); ++i) {
        const Record& r = records[i];
        if (r.type != kNodeEvent && r.type != kNodeStart) {
            continue;
        }
        std::vector<UigbAction> actions;
        for (const size_t c : consumers[i]) {
            if (auto action = action_of(records[c])) {
                actions.push_back(*action);
            }
        }
        if (r.type == kNodeStart) {
            start_.insert(start_.end(), actions.begin(), actions.end());
        } else if (auto hash = param_at(r, 0, 8)) {
            events_.push_back({Read<uint64_t>(d, *hash) & kStrCode64Mask, std::move(actions)});
        }
    }
    return true;
}

const UigbEvent* UigbGraph::FindEvent(uint64_t name) const {
    for (const UigbEvent& e : events_) {
        if (e.name == (name & kStrCode64Mask)) {
            return &e;
        }
    }
    return nullptr;
}

}
