#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include <cstddef>

namespace pt::platform {

struct UserDataReport {
    bool success{};
    bool migrated_legacy{};
    std::size_t files_copied{};
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

UserDataReport PrepareUserDataDirectory(const std::filesystem::path& destination,
                                       const std::filesystem::path& legacy,
                                       bool migrate_legacy = true);

}
