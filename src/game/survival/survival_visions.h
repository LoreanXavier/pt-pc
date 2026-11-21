#pragma once

#include <filesystem>
#include <string>

namespace pt::game {

inline constexpr int kSurvivalVisionVersion = 2;
inline constexpr const char* kSurvivalVisionNames[] = {"", "fuse", "radio", "", "lantern", "door"};
std::string SurvivalVisionRoute(const std::filesystem::path& dir);
bool MakeSurvivalVisionCards(const std::filesystem::path& dir);
bool SurvivalVisionsReady(const std::filesystem::path& dir);

}
