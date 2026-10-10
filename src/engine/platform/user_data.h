#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <cstddef>

namespace pt::platform {

struct UserDataReport {
    bool success{};
    bool migrated_legacy{};
    bool removed_legacy{};
    bool declined_legacy{};
    std::size_t files_copied{};
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

// Creates destination, verifies it is writable, and offers the legacy profile (1.0.1's %APPDATA% / ~/.local/share folder) once.
// `ask` (when given) decides: true moves it (missing files copied, existing destination files win, then the legacy folder is
// deleted), false leaves it where it is. Either answer writes the migration marker, so the question never comes again; without
// `ask` the files are copied and the legacy folder kept, as before.
UserDataReport PrepareUserDataDirectory(const std::filesystem::path& destination,
                                       const std::filesystem::path& legacy,
                                       bool migrate_legacy = true,
                                       const std::function<bool(const std::filesystem::path&)>& ask = {});

} // namespace pt::platform
