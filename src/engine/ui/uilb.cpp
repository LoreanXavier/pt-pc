#include "engine/ui/uilb.h"

#include <cstring>

#include "engine/core/strcode.h"

namespace pt::ui {
namespace {

constexpr size_t kModelRecordSize = 0x64;
constexpr size_t kAnimationRecordSize = 0x14;
constexpr uint16_t kNone = 0xFFFF;

template <typename T>
T Read(std::span<const uint8_t> d, size_t at) {
    T v;
    std::memcpy(&v, d.data() + at, sizeof(T));
    return v;
}

}

bool UilbLayout::Parse(std::span<const uint8_t> d, std::string* error) {
    auto fail = [&](const char* message) {
        if (error) {
            *error = message;
        }
        return false;
    };
    models_.clear();
    animations_.clear();
    camera_distance_ = 0.0f;
    if (d.size() < 0x30 || std::memcmp(d.data(), "UILB", 4) != 0) {
        return fail("not a UILB file");
    }
    const uint16_t model_count = Read<uint16_t>(d, 0x08);
    const uint16_t animation_count = Read<uint16_t>(d, 0x0A);
    const uint16_t name_count = Read<uint16_t>(d, 0x10);
    const uint16_t string_count = Read<uint16_t>(d, 0x12);
    const uint32_t models = Read<uint32_t>(d, 0x14);
    const uint32_t animations = Read<uint32_t>(d, 0x18);
    const uint32_t camera = Read<uint32_t>(d, 0x1C);
    const uint32_t data_size = Read<uint32_t>(d, 0x24);
    const uint32_t strings = Read<uint32_t>(d, 0x28);
    const uint32_t base = Read<uint32_t>(d, 0x2C);
    auto in_range = [&](size_t at, size_t bytes) { return at + bytes <= d.size(); };
    if (!in_range(strings, string_count * 8ull) || !in_range(static_cast<size_t>(base) + data_size, name_count * 8ull) ||
        !in_range(models, model_count * kModelRecordSize) || !in_range(animations, animation_count * kAnimationRecordSize)) {
        return fail("UILB tables out of range");
    }
    std::vector<std::string> texts;
    for (uint16_t i = 0; i < string_count; ++i) {
        const uint32_t length = Read<uint32_t>(d, strings + i * 8ull);
        const uint32_t offset = Read<uint32_t>(d, strings + i * 8ull + 4);
        if (!in_range(static_cast<size_t>(base) + offset, length)) {
            return fail("UILB string out of range");
        }
        const char* text = reinterpret_cast<const char*>(d.data() + base + offset);
        texts.emplace_back(text, strnlen(text, length));
    }
    std::vector<uint64_t> names;
    for (uint16_t i = 0; i < name_count; ++i) {
        names.push_back(Read<uint64_t>(d, base + data_size + i * 8ull) & kStrCode64Mask);
    }
    auto text_at = [&](uint16_t index) { return index < texts.size() ? texts[index] : std::string(); };
    auto name_at = [&](uint16_t index) { return index < names.size() ? names[index] : 0; };
    for (uint16_t i = 0; i < model_count; ++i) {
        const size_t r = models + i * kModelRecordSize;
        UilbModel model;
        model.path = text_at(Read<uint16_t>(d, r + 2));
        // the model's placement: scale +0x0C, rotation quaternion +0x18, translation +0x2C (UI units; UI_sys_loading puts its icon at
        // (54.39, -26.66), low on the right), colour +0x3C
        model.translate = glm::vec3(Read<float>(d, r + 0x2C), Read<float>(d, r + 0x30), Read<float>(d, r + 0x34));
        const uint32_t list = Read<uint32_t>(d, r + 0x5C);
        const uint16_t count = Read<uint16_t>(d, r + 0x60);
        for (uint16_t k = 0; k < count && list + (k + 1) * 2ull <= data_size; ++k) {
            model.animations.push_back(name_at(Read<uint16_t>(d, base + list + k * 2ull)));
        }
        models_.push_back(std::move(model));
    }
    for (uint16_t i = 0; i < animation_count; ++i) {
        const size_t r = animations + i * kAnimationRecordSize;
        UilbAnimation animation;
        animation.name = name_at(Read<uint16_t>(d, r));
        const uint16_t main = Read<uint16_t>(d, r + 2);
        const uint16_t shader = Read<uint16_t>(d, r + 4);
        animation.main = main == kNone ? std::string() : text_at(main);
        animation.shader = shader == kNone ? std::string() : text_at(shader);
        animation.speed = Read<float>(d, r + 0x10);
        animations_.push_back(std::move(animation));
    }
    if (camera != 0xFFFFFFFFu && in_range(camera, 0x20)) {
        camera_distance_ = Read<float>(d, camera + 0x1C);
    }
    return true;
}

const UilbAnimation* UilbLayout::FindAnimation(uint64_t name) const {
    for (const UilbAnimation& a : animations_) {
        if (a.name == (name & kStrCode64Mask)) {
            return &a;
        }
    }
    return nullptr;
}

}
