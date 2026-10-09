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

// Creates destination, verifies it is writable, and imports missing files from legacy once.
// Existing destination files win. A migration marker prevents later imports from resurrecting
// reset saves or importing unrelated data after the game directory has been copied elsewhere.
UserDataReport PrepareUserDataDirectory(const std::filesystem::path& destination,
                                       const std::filesystem::path& legacy,
                                       bool migrate_legacy = true);

} // namespace pt::platform
