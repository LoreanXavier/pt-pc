#pragma once

// The check for a newer release, shared by the game and the installer (docs/updates.md). One GET of GitHub release metadata on
// a thread of its own, started once; nothing waits for it, and offline it simply finds nothing.

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace pt::update {

// The release metadata endpoint, set in one place: the CMake cache variable PT_UPDATE_MANIFEST_URL (CMakeLists.txt). The
// environment variable of the same name overrides it at run time (tests). Empty or the placeholder: no check is made.
std::string ManifestUrl();
// this build's version, PT_VERSION in CMakeLists.txt
std::string_view CurrentVersion();
// Windows, Linux or the native macOS CPU architecture: the release package to select
std::string_view Platform();

struct Release {
    std::string version;
    std::string url;    // the download or release page for this platform
    std::string notes;  // one short line
};

// numeric parts compared left to right ("0.10.0" > "0.9.2"); a suffix after '-' sorts before the plain version
int CompareVersions(std::string_view a, std::string_view b);
// GitHub release metadata (or a legacy/custom manifest); nullopt for invalid/unpublished releases
std::optional<Release> ParseManifest(std::string_view json, std::string_view platform);

class Checker {
public:
    // starts the lookup once, on a detached thread (quitting never waits for it); nothing when the URL is empty or the
    // placeholder
    void Start();
    // tests (--fake-update): the answer is this version, no request is made; a version not newer than this build's is "nothing"
    void Fake(std::string_view version);
    // the newer release, once the lookup found one
    std::optional<Release> Newer() const;
    bool Done() const;

private:
    struct State {
        std::mutex mutex;
        std::optional<Release> newer;
        std::atomic<bool> done{false};
    };
    std::shared_ptr<State> state_;
};

}
