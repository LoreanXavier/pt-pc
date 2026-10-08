#pragma once

#include <filesystem>
#include <string_view>

namespace pt {

std::filesystem::path ExecutableDir();
#ifdef __APPLE__
/* The running executable. In the macOS app bundle ExecutableDir() is Contents/Resources, where the data is, and the
   executable is in Contents/MacOS (docs/macos.md). */
std::filesystem::path ExecutablePath();
#endif
std::filesystem::path ResourceDir(std::string_view name, const std::filesystem::path& build_dir);

}
