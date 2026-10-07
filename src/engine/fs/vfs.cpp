#include "engine/fs/vfs.h"

#include <cstdio>
#include <fstream>
#include <iterator>

#include "engine/core/log.h"
#include "engine/platform/os.h"
#include "engine/fs/mods.h"

namespace pt {

bool Vfs::Mount(const std::filesystem::path& game_dir) {
    game_dir_ = game_dir;
    const auto psarc_path = game_dir / "chunk1.psarc";
    if (!std::filesystem::exists(psarc_path)) {
        LogError("vfs: {} not found, game dir must be the extracted CUSA01127 folder", psarc_path.string());
        return false;
    }
    if (!archive_.Open(psarc_path)) {
        return false;
    }
    const auto qar_path = game_dir / "texture.qar";
    if (!std::filesystem::exists(qar_path) || !textures_.Open(qar_path)) {
        LogError("vfs: {} missing or unreadable", qar_path.string());
        return false;
    }
    ReportDataDifferences();
    return true;
}

void Vfs::ReportDataDifferences() const {
    std::error_code ec;
    const auto source = game_dir_ / "source.txt";
    if (std::filesystem::is_regular_file(source, ec)) {
        std::ifstream in(source);
        for (std::string line; std::getline(in, line);) {
            if (line.starts_with("warning=")) {
                LogWarn("data: {}", line.substr(8));
            } else if (!line.empty()) {
                LogInfo("data: {}", line);
            }
        }
    }
    static constexpr const char* kCore[] = {
        "as/sh/level/common/resident.fpk",
        "as/sh/level/promotion/pt_2014/start/pt14_start.fpk",
        "as/sh/level/promotion/pt_2014/hallway/pt14_hallway.fpk",
        "as/sh/level/promotion/pt_2014/hallway_maze_A/pt14_hallway_maze_A.fpk",
        "as/sh/level/promotion/pt_2014/hallway_maze_B/pt14_hallway_maze_B.fpk",
        "as/sh/level/promotion/pt_2014/hallway_maze_C/pt14_hallway_maze_C.fpk",
        "as/sh/level/promotion/pt_2014/ending/ending.fpk",
        "as/sh/level_asset/chara/player/game_object/player2_common_motion.fpk",
        "as/sh/level_asset/chara/player/game_object/plparts_normal.fpk",
        "as/sh/ui/ui_default_data.fpk",
        "as/sh/ui/ui_default_lang.fpk",
        "as/sh/level/ui/subtitles/EngVoice/EngText/subtitle.fpk",
    };
    int missing = 0;
    for (const char* path : kCore) {
        if (!archive_.Contains(path)) {
            LogWarn("data: {} missing from chunk1.psarc, dependent content will not load", path);
            ++missing;
        }
    }
    if (missing == 0) {
        LogInfo("data: {} core packages present", std::size(kCore));
    }
}

std::string Vfs::ToArchivePath(std::string_view asset_path) {
    std::string path(asset_path);
    for (char& c : path) {
        if (c == '\\') {
            c = '/';
        }
    }
    constexpr std::string_view kAssets = "/Assets/";
    if (path.starts_with(kAssets)) {
        return "as/" + path.substr(kAssets.size());
    }
    while (path.starts_with('/')) {
        path.erase(0, 1);
    }
    return path;
}

std::shared_ptr<FoxPackage> Vfs::LoadPackage(std::string_view path) {
    const std::string archive_path = ToArchivePath(path);
    {
        std::lock_guard lock(mutex_);
        auto it = packages_.find(archive_path);
        if (it != packages_.end()) {
            return it->second;
        }
    }
    auto data = mods::ReadOverride(archive_path);
    if (data) {
        LogInfo("vfs: {} from a mod", archive_path);
    } else {
        data = archive_.Read(archive_path);
    }
    if (!data) {
        LogError("vfs: package {} not found", archive_path);
        return nullptr;
    }
    auto package = std::make_shared<FoxPackage>();
    if (!package->Load(archive_path, std::move(*data))) {
        return nullptr;
    }
    LogInfo("vfs: loaded {} ({} files)", archive_path, package->Entries().size());
    std::lock_guard lock(mutex_);
    packages_[archive_path] = package;
    return package;
}

std::vector<std::shared_ptr<FoxPackage>> Vfs::LoadedPackages() const {
    std::lock_guard lock(mutex_);
    std::vector<std::shared_ptr<FoxPackage>> out;
    for (const auto& [name, package] : packages_) {
        out.push_back(package);
    }
    return out;
}

void Vfs::UnloadPackage(std::string_view path) {
    std::lock_guard lock(mutex_);
    packages_.erase(ToArchivePath(path));
}

std::optional<std::vector<uint8_t>> Vfs::ReadFile(std::string_view path) const {
    if (auto data = mods::ReadOverride(path)) {
        return data;
    }
    {
        std::lock_guard lock(mutex_);
        for (const auto& [name, package] : packages_) {
            if (const auto* entry = package->Find(path)) {
                return package->Read(*entry);
            }
        }
    }
    if (auto data = archive_.Read(ToArchivePath(path))) {
        return data;
    }
    const auto loose = game_dir_ / std::filesystem::path(ToArchivePath(path));
    if (std::filesystem::is_regular_file(loose)) {
        FILE* f = os::OpenFile(loose, "rb");
        if (f) {
            std::vector<uint8_t> data(static_cast<size_t>(std::filesystem::file_size(loose)));
            const size_t read = std::fread(data.data(), 1, data.size(), f);
            std::fclose(f);
            if (read == data.size()) {
                return data;
            }
        }
    }
    return std::nullopt;
}

}
