#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pt {

uint64_t CityHash64(const void* data, size_t length);
uint64_t CityHash64WithSeeds(const void* data, size_t length, uint64_t seed0, uint64_t seed1);

constexpr uint64_t kStrCode64Mask = 0xFFFFFFFFFFFFull;

uint64_t StrCode64(std::string_view text);

}
