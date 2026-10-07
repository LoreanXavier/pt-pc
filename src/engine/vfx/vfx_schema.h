#pragma once

#include <cstdint>
#include <span>

namespace pt::vfx {

enum class PropType : uint8_t { Bool = 0, UInt32 = 1, Float = 2, Vector4 = 3, String = 4, StrCode = 5, PathCode = 6 };

struct PropDef {
    uint32_t hash = 0;
    uint8_t type = 0;
};

struct ClassDef {
    uint32_t hash = 0;
    const char* name = nullptr;
    std::span<const PropDef> props;
};

const ClassDef* FindClass(uint32_t hash);
std::span<const ClassDef> Classes();

}
