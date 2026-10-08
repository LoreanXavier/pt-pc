#include "engine/core/log.h"
#include "engine/platform/os.h"
#include "engine/platform/settings.h"
#include "game/save_data.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#endif
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const auto dir = std::filesystem::path(argv[1]) / std::filesystem::path(u8"\u0160-\u00e7\u00c7\u011f\u011e\u0131\u0130\u00f6\u00d6\u015f\u015e\u00fc\u00dc-\u65e5\u672c\u8a9e-\u0416\u0438-\u0627\u0639-\u03a9-\u05d0-\u0915-\ud55c-\u0e01-\U0001f600");
    std::filesystem::create_directories(dir);
    int failed = 0;
    auto check = [&](bool ok, const char* label) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", label); failed += !ok; };
    const auto log = dir / "pt.log";
    const auto utf8 = log.u8string();
    check(pt::os::PathFromUtf8(std::string_view(reinterpret_cast<const char*>(utf8.data()), utf8.size())) == log, "UTF-8 path round trip");
#ifdef _WIN32
    SetEnvironmentVariableW(L"PT_UNICODE_PATH_TEST", dir.c_str());
#else
    setenv("PT_UNICODE_PATH_TEST", dir.c_str(), 1);
#endif
    check(pt::os::PathFromUtf8(pt::os::GetEnv("PT_UNICODE_PATH_TEST")) == dir, "Unicode environment path preserved");
    pt::LogSetFile(log);
    pt::LogInfo("unicode log sentinel");
    std::ifstream in(log);
    std::string text((std::istreambuf_iterator<char>(in)), {});
    check(text.find("unicode log sentinel") != std::string::npos, "log written to actual Unicode directory");
    pt::AppSettings written, loaded;
    written.voice.key = "J";
    written.audio.volume = 0.37f;
    check(pt::SaveAppSettings(dir / "pt.ini", written) && pt::LoadAppSettings(dir / "pt.ini", loaded) && loaded.voice.key == "J" && loaded.audio.volume == written.audio.volume, "Unicode settings and J fallback round trip");
    pt::game::SaveStore store;
    store.SetDirectory(dir, "PT_Save_Data");
    pt::game::SaveFile save;
    save.progress.floor = "f050";
    check(store.Save(save), "Unicode save written");
    const auto read = store.Load();
    check(read && read->progress.floor == "f050", "Unicode save read back");
    return failed ? 1 : 0;
}
