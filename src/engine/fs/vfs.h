#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/fs/fpk.h"
#include "engine/fs/psarc.h"
#include "engine/fs/qar.h"

namespace pt {

class Vfs {
public:
    bool Mount(const std::filesystem::path& game_dir);

    const std::filesystem::path& GameDir() const { return game_dir_; }
    const Psarc& Archive() const { return archive_; }
    const QarArchive& Textures() const { return textures_; }

    static std::string ToArchivePath(std::string_view asset_path);

    std::shared_ptr<FoxPackage> LoadPackage(std::string_view path);
    std::vector<std::shared_ptr<FoxPackage>> LoadedPackages() const;
    void UnloadPackage(std::string_view path);

    std::optional<std::vector<uint8_t>> ReadFile(std::string_view path) const;

private:
    void ReportDataDifferences() const;

    std::filesystem::path game_dir_;
    Psarc archive_;
    QarArchive textures_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<FoxPackage>> packages_;
};

}
