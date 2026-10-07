#pragma once

#include <cstdint>
#include <string_view>

namespace pt {

uint64_t SeededHash(std::string_view text);
uint32_t ExtensionType(std::string_view extension);
uint64_t PathCode64(std::string_view path);
uint64_t PathCodeWithExtension(uint64_t code, std::string_view extension);

}
