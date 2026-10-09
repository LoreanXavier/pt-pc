#include "engine/platform/http.h"
#include "engine/platform/os.h"
#include "engine/platform/update_check.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <thread>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--child-exit") {
        std::puts("hello");
        return std::atoi(argv[2]);
    }
    if (argc == 3 && std::string_view(argv[1]) == "--child-sleep") {
        std::this_thread::sleep_for(std::chrono::seconds(std::atoi(argv[2])));
        return 0;
    }
    int failed = 0;
    auto check = [&](bool ok, const char* label) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", label); failed += !ok; };
    using namespace pt;
    check(http::ParseHttpDate("Tue, 06 Oct 2026 09:25:00 GMT") == 1791278700, "HTTP date");
    check(http::ParseHttpDate("Thu, 01 Jan 1970 00:00:00 GMT") == 0, "HTTP date epoch");
    check(!http::ParseHttpDate("not a date"), "HTTP date rejects text");
    check(update::CompareVersions("0.10.0", "0.9.2") > 0, "version order by number");
    check(update::CompareVersions("v0.2.0", "0.2.0") == 0, "leading v");
    check(update::CompareVersions("0.2.0-rc1", "0.2.0") < 0, "pre-release before release");
    check(update::CompareVersions("0.1", "0.1.0") == 0, "missing parts are zero");
    const char* manifest = R"({"version": "0.2.0", "notes": "Linux build", "url": "https://example.org/releases/latest",
        "platforms": {"windows": {"url": "https://example.org/setup.exe", "sha256": "00"}, "linux": {"url": "https://example.org/pt-linux.tar.gz"}},
        "published": 1791278700, "prerelease": false, "assets": [1, "two", {"three": 3}]})";
    const auto windows = update::ParseManifest(manifest, "windows");
    const auto lin = update::ParseManifest(manifest, "linux");
    const auto other = update::ParseManifest(manifest, "macos");
    check(windows && windows->version == "0.2.0" && windows->url == "https://example.org/setup.exe" && windows->notes == "Linux build", "manifest, windows");
    check(lin && lin->url == "https://example.org/pt-linux.tar.gz", "manifest, linux");
    check(other && other->url == "https://example.org/releases/latest", "manifest, other platform falls back to the release page");
    check(!update::ParseManifest(R"({"notes": "no version"})", "windows"), "manifest without a version");
    check(!update::ParseManifest("<html>", "windows"), "not JSON");
    check(!update::ParseManifest(R"({"version": "1", )", "windows"), "truncated JSON");
    const char* github = R"({"tag_name":"v1.0.2","html_url":"https://github.com/LoreanXavier/pt-pc/releases/tag/v1.0.2",
        "body":"Fixes\nMore details", "draft":false,"prerelease":false,
        "assets":[
          {"name":"P.T.PC.Port-portable-windows.zip","browser_download_url":"https://example.org/portable.zip"},
          {"name":"P.T.PC.Port.Setup-linux","browser_download_url":"https://example.org/linux"},
          {"name":"P.T.PC.Port-macOS-x64.zip","browser_download_url":"https://example.org/intel.zip"},
          {"name":"P.T.PC.Port.Setup.exe","browser_download_url":"https://example.org/windows.exe"},
          {"name":"P.T.PC.Port-macOS-arm64.zip","browser_download_url":"https://example.org/apple.zip"}
        ]})";
    const auto gh_windows = update::ParseManifest(github, "windows");
    const auto gh_linux = update::ParseManifest(github, "linux");
    const auto gh_arm = update::ParseManifest(github, "macos-arm64");
    const auto gh_intel = update::ParseManifest(github, "macos-x64");
    check(gh_windows && gh_windows->version == "v1.0.2" && gh_windows->url == "https://example.org/windows.exe",
          "GitHub release selects Windows installer instead of portable ZIP");
    check(gh_linux && gh_linux->url == "https://example.org/linux", "GitHub release selects Linux installer");
    check(gh_arm && gh_arm->url == "https://example.org/apple.zip", "GitHub release selects Apple silicon app");
    check(gh_intel && gh_intel->url == "https://example.org/intel.zip", "GitHub release selects Intel app");
    const char* no_asset = R"({"tag_name":"v1.0.2","html_url":"https://example.org/release","assets":[]})";
    const auto gh_fallback = update::ParseManifest(no_asset, "macos-arm64");
    check(gh_fallback && gh_fallback->url == "https://example.org/release", "missing platform asset falls back to release page");
    check(!update::ParseManifest(R"({"tag_name":"v1.0.2","html_url":"https://example.org/r","draft":true})", "windows"),
          "draft GitHub release is ignored");
    check(!update::ParseManifest(R"({"tag_name":"v1.0.2-rc1","html_url":"https://example.org/r","prerelease":true})", "windows"),
          "prerelease GitHub release is ignored");
    check(!update::ParseManifest(R"({"tag_name":"nightly","html_url":"https://example.org/r"})", "windows"),
          "non-version release tag is ignored");
    check(!update::ParseManifest(R"({"message":"API rate limit exceeded"})", "windows"), "API errors are not updates");
    check(!update::ParseManifest(R"({"tag_name":"v1.0.2.","html_url":"https://example.org/r"})", "windows"), "malformed version tag is ignored");
    const auto unsafe_asset = update::ParseManifest(R"({"tag_name":"v1.0.2","html_url":"https://example.org/r",
        "assets":[{"name":"P.T.PC.Port.Setup.exe","browser_download_url":"http://example.org/setup.exe"}]})", "windows");
    check(unsafe_asset && unsafe_asset->url == "https://example.org/r", "non-HTTPS asset is ignored");
    if (os::GetEnv("PT_UPDATE_MANIFEST_URL").empty()) {
#ifdef _WIN32
        _putenv_s("PT_UPDATE_MANIFEST_URL", "https://releases.invalid/pt-port/latest.json");
#else
        setenv("PT_UPDATE_MANIFEST_URL", "https://releases.invalid/pt-port/latest.json", 1);
#endif
        update::Checker placeholder;
        placeholder.Start();
        check(placeholder.Done() && !placeholder.Newer(), "the placeholder URL sends nothing");
#ifdef _WIN32
        _putenv_s("PT_UPDATE_MANIFEST_URL", "");
#else
        unsetenv("PT_UPDATE_MANIFEST_URL");
#endif
        update::Checker built_in;
        built_in.Start();
        for (int i = 0; i < 200 && !built_in.Done(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto newer = built_in.Newer();
        std::printf("update check at %s: %s\n", update::ManifestUrl().c_str(), newer ? (newer->version + " " + newer->url).c_str() : "nothing newer");
        check(built_in.Done(), "the built-in manifest address answers or times out");
    }
    if (argc >= 2 && std::string_view(argv[1]) == "--network") {
        http::Request request;
        request.url = "https://www.google.com/generate_204";
        request.no_cache = true;
        const auto response = http::Get(request);
        check(response && response->status == 204 && response->date && *response->date > 1700000000, "HTTPS GET with a Date header");
        request.url = "https://api.github.com/repos/LoreanXavier/pt-pc/releases/latest";
        request.max_body = 512 * 1024;
        const auto api = http::Get(request);
        const auto published = api && api->status == 200 ? update::ParseManifest(api->body, update::Platform()) : std::nullopt;
        check(published && !published->version.empty() && published->url.starts_with("https://"), "real GitHub release API parses without a manifest asset");
        request.url = "https://nonexistent-host.invalid/";
        check(!http::Get(request), "an unreachable host is no answer");
        if (!os::GetEnv("PT_UPDATE_MANIFEST_URL").empty()) {
            update::Checker checker;
            checker.Start();
            for (int i = 0; i < 200 && !checker.Done(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const auto newer = checker.Newer();
            std::printf("update check: %s\n", newer ? (newer->version + " " + newer->url).c_str() : "nothing newer");
            check(checker.Done(), "update check finishes");
        }
    }

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("pt-platform-test-" + std::to_string(os::ProcessId()));
    std::filesystem::create_directories(dir);
    {
        os::FileLock first(dir / "lock");
        os::FileLock second(dir / "lock");
        check(first.Held() && !second.Held(), "file lock is exclusive");
    }
    {
        os::FileLock again(dir / "lock");
        check(again.Held(), "file lock is released");
    }
    {
        std::ofstream(dir / "data.bin", std::ios::binary) << "0123456789";
        FILE* f = os::OpenFile(dir / "data.bin", "rb");
        char c = 0;
        check(f && os::SeekFile(f, 7, SEEK_SET) == 0 && std::fread(&c, 1, 1, f) == 1 && c == '7', "open and 64-bit seek");
        if (f) std::fclose(f);
    }
    std::atomic<bool> cancel{false};
    const std::filesystem::path self = std::filesystem::absolute(argv[0]);
    const auto exit3 = os::RunProcess(self, {"--child-exit", "3"}, dir, dir / "out.log", cancel, std::chrono::seconds(10));
    const auto slow = os::RunProcess(self, {"--child-sleep", "30"}, dir, dir / "slow.log", cancel, std::chrono::milliseconds(500));
    std::ifstream log(dir / "out.log");
    std::string line;
    std::getline(log, line);
    check(exit3.started && exit3.exit_code == 3 && line.rfind("hello", 0) == 0, "process exit code and output");
    check(slow.started && slow.timed_out, "process timeout kills it");
    log.close();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::printf("%s\n", failed ? "platform test: FAIL" : "platform test: PASS");
    return failed ? 1 : 0;
}
