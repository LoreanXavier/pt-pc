#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pt::ui {

enum class UigbActionKind { PlayAnimation, SetVisible };

struct UigbAction {
    UigbActionKind kind = UigbActionKind::PlayAnimation;
    uint64_t animation = 0;
    uint64_t model = 0;
    float speed = 1.0f;
    bool loop = false;
    uint64_t layout = 0;
    uint64_t target = 0;
    bool visible = false;
};

struct UigbEvent {
    uint64_t name = 0;
    std::vector<UigbAction> actions;
};

class UigbGraph {
public:
    static constexpr uint64_t kNodePlayAnimation = 0x61BAC8085A1C;
    static constexpr uint64_t kNodeSetVisible = 0x8317F8087C1A;
    static constexpr uint64_t kNodeEvent = 0xC36617182039;
    static constexpr uint64_t kNodeStart = 0xF48D5D63E340;
    static constexpr uint64_t kEmptyName = 0xB8A0BF169F98;

    bool Parse(std::span<const uint8_t> data, std::string* error = nullptr);

    const std::vector<std::string>& Layouts() const { return layouts_; }
    const std::vector<UigbAction>& StartActions() const { return start_; }
    const std::vector<UigbEvent>& Events() const { return events_; }
    const UigbEvent* FindEvent(uint64_t name) const;

private:
    std::vector<std::string> layouts_;
    std::vector<UigbAction> start_;
    std::vector<UigbEvent> events_;
};

}
