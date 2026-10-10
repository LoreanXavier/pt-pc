#include "engine/platform/user_data.h"
#include "engine/platform/settings.h"
#include "game/save_data.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {
void Write(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << value;
}

std::string Read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const auto root = fs::absolute(argv[1]) / fs::path(u8"ユーザーデータ-éğ-Ж");
    if (fs::exists(root)) return 2;
    fs::create_directories(root);
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("%s: %s\n", label, ok ? "PASS" : "FAIL");
        failures += !ok;
    };

    // Import the whole ordinary data tree, including less common siblings and caches.
    const auto legacy = root / "legacy";
    const auto dest = root / "game" / "data";
    Write(legacy / "pt.ini", "legacy settings");
    Write(legacy / "PT_Save_Data0", "legacy save");
    Write(legacy / "logs" / "pt.log", "legacy log");
    Write(legacy / "cache" / "previews" / "thumb.bin", "preview");
    Write(dest / "pt.ini", "destination settings");
    auto report = pt::platform::PrepareUserDataDirectory(dest, legacy);
    check(report.success && report.migrated_legacy && report.files_copied == 3,
          "new Unicode destination imports ordinary files and subfolders");
    check(Read(dest / "pt.ini") == "destination settings" && Read(dest / "PT_Save_Data0") == "legacy save" &&
          Read(dest / "logs" / "pt.log") == "legacy log" &&
          Read(dest / "cache" / "previews" / "thumb.bin") == "preview",
          "destination settings win and migrated file contents are preserved");
    check(Read(legacy / "PT_Save_Data0") == "legacy save", "legacy data remains untouched");

    // Destination wins, and the marker prevents a later import from resurrecting reset saves.
    Write(dest / "pt.ini", "new settings");
    fs::remove(dest / "PT_Save_Data0");
    report = pt::platform::PrepareUserDataDirectory(dest, legacy);
    check(report.success && Read(dest / "pt.ini") == "new settings" && !fs::exists(dest / "PT_Save_Data0"),
          "repeat call preserves destination and does not resurrect deleted save");

    // A fresh install with no legacy tree records the decision once, preventing later unrelated imports.
    const auto fresh = root / "fresh" / "data";
    const auto absent = root / "not-yet-present";
    report = pt::platform::PrepareUserDataDirectory(fresh, absent);
    Write(absent / "PT_Save_Data0", "unrelated machine save");
    report = pt::platform::PrepareUserDataDirectory(fresh, absent);
    check(report.success && !report.migrated_legacy && !fs::exists(fresh / "PT_Save_Data0"),
          "missing legacy path is marked checked and later unrelated data is ignored");

    // Valid native settings and save files survive migration and remain loadable by game code.
    const auto nativeLegacy = root / "native-legacy";
    const auto nativeDest = root / "native-game" / "data";
    pt::AppSettings oldSettings;
    oldSettings.audio.volume = 0.42f;
    oldSettings.voice.key = "J";
    check(pt::SaveAppSettings(nativeLegacy / "pt.ini", oldSettings), "legacy settings fixture created");
    pt::game::SaveStore oldStore;
    oldStore.SetDirectory(nativeLegacy, "PT_Save_Data");
    pt::game::SaveFile oldSave;
    oldSave.progress.floor = "f050";
    check(oldStore.Save(oldSave), "legacy save fixture created");
    report = pt::platform::PrepareUserDataDirectory(nativeDest, nativeLegacy, false);
    check(report.success && !report.migrated_legacy &&
          !fs::exists(nativeDest / ".pt-user-data-migration-v1") &&
          !fs::exists(nativeDest / "pt.ini"),
          "headless preparation leaves migration pending and writes no marker");
    report = pt::platform::PrepareUserDataDirectory(nativeDest, nativeLegacy);
    pt::AppSettings loadedSettings;
    pt::game::SaveStore newStore;
    newStore.SetDirectory(nativeDest, "PT_Save_Data");
    const auto loadedSave = newStore.Load();
    check(report.success && pt::LoadAppSettings(nativeDest / "pt.ini", loadedSettings) &&
          loadedSettings.voice.key == "J" && loadedSettings.audio.volume == oldSettings.audio.volume &&
          loadedSave && loadedSave->progress.floor == "f050",
          "migrated settings and save files restore through native loaders");

    // A conflict that prevents publication must leave the marker absent; retry must finish cleanly.
    const auto retryDest = root / "retry" / "data";
    const auto retryLegacy = root / "retry-legacy";
    Write(retryLegacy / "nested" / "save", "retry save");
    fs::create_directories(retryDest / "nested" / "save");
    Write(retryDest / "nested" / "save" / "blocker", "destination conflict");
    report = pt::platform::PrepareUserDataDirectory(retryDest, retryLegacy);
    const bool failedWithoutMarker = !report.success && !report.errors.empty() &&
        !fs::exists(retryDest / ".pt-user-data-migration-v1");
    fs::remove_all(retryDest / "nested" / "save");
    report = pt::platform::PrepareUserDataDirectory(retryDest, retryLegacy);
    check(failedWithoutMarker && report.success && Read(retryDest / "nested" / "save") == "retry save",
          "interrupted migration retries before writing completion marker");

    const auto invalidMarkerDest = root / "invalid-marker" / "data";
    const auto invalidMarkerLegacy = root / "invalid-marker-legacy";
    Write(invalidMarkerLegacy / "PT_Save_Data0", "must not import under invalid marker");
    fs::create_directories(invalidMarkerDest / ".pt-user-data-migration-v1");
    report = pt::platform::PrepareUserDataDirectory(invalidMarkerDest, invalidMarkerLegacy);
    check(!report.success && !report.errors.empty() && !fs::exists(invalidMarkerDest / "PT_Save_Data0"),
          "directory at marker path cannot suppress migration");

    const auto sameDest = root / "same-path" / "data";
    Write(sameDest / "PT_Save_Data0", "already in destination");
    report = pt::platform::PrepareUserDataDirectory(sameDest, sameDest);
    check(report.success && !report.migrated_legacy && Read(sameDest / "PT_Save_Data0") == "already in destination",
          "identical legacy and destination paths do not self-migrate");

    // A destination that is a file cannot pass the actual write probe.
    const auto blocked = root / "blocked";
    Write(blocked, "not a directory");
    report = pt::platform::PrepareUserDataDirectory(blocked, legacy);
    check(!report.success && !report.errors.empty(), "destination write probe failure is reported");
#ifndef _WIN32
    const auto readOnly = root / "read-only";
    fs::create_directories(readOnly);
    if (::geteuid() == 0) {
        check(true, "read-only permission check skipped for privileged test user");
    } else {
        fs::permissions(readOnly, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
        report = pt::platform::PrepareUserDataDirectory(readOnly, legacy);
        fs::permissions(readOnly, fs::perms::owner_all, fs::perm_options::replace);
        check(!report.success && !report.errors.empty(), "read-only directory fails real write probe");
    }
#endif

    // Symlinked directories must never be traversed into or imported.
    const auto linkDest = root / "links" / "data";
    const auto linkLegacy = root / "link-legacy";
    Write(root / "outside" / "secret", "outside");
    fs::create_directories(linkLegacy);
    std::error_code linkError;
    fs::create_directory_symlink(root / "outside", linkLegacy / "linked", linkError);
    if (linkError) {
        check(true, "symlink fixture unavailable on this platform");
    } else {
        report = pt::platform::PrepareUserDataDirectory(linkDest, linkLegacy);
        check(report.success && !fs::exists(linkDest / "linked" / "secret") && !report.warnings.empty(),
              "legacy symlink directory is skipped");
    }

    // The question (1.0.2): Yes moves the saves and deletes the old folder, No leaves it, and neither asks again.
    const auto askLegacy = root / "ask-legacy" / "pt-port" / "pt";
    const auto askDest = root / "ask-game" / "data";
    Write(askLegacy / "PT_Save_Data0", "old save");
    int asked = 0;
    report = pt::platform::PrepareUserDataDirectory(askDest, askLegacy, true, [&](const fs::path&) { ++asked; return true; });
    check(report.success && asked == 1 && report.migrated_legacy && report.removed_legacy && Read(askDest / "PT_Save_Data0") == "old save" &&
              !fs::exists(askLegacy) && !fs::exists(askLegacy.parent_path()),
          "Yes moves the saves and deletes the old folder");
    const auto noLegacy = root / "no-legacy";
    const auto noDest = root / "no-game" / "data";
    Write(noLegacy / "PT_Save_Data0", "old save");
    asked = 0;
    report = pt::platform::PrepareUserDataDirectory(noDest, noLegacy, true, [&](const fs::path&) { ++asked; return false; });
    report = pt::platform::PrepareUserDataDirectory(noDest, noLegacy, true, [&](const fs::path&) { ++asked; return false; });
    check(report.success && asked == 1 && !fs::exists(noDest / "PT_Save_Data0") && Read(noLegacy / "PT_Save_Data0") == "old save",
          "No keeps the old folder and is not asked again");

    fs::remove_all(root);
    return failures ? 1 : 0;
}
