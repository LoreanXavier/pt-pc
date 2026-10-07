#pragma once

#include <filesystem>
#include <string_view>

namespace pt {

std::filesystem::path ExecutableDir();
std::filesystem::path ResourceDir(std::string_view name, const std::filesystem::path& build_dir);

}
