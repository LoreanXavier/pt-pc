#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace pt::update {

std::string ManifestUrl();
std::string_view CurrentVersion();
std::string_view Platform();

struct Release {
    std::string version;
    std::string url;
    std::string notes;
};

int CompareVersions(std::string_view a, std::string_view b);
std::optional<Release> ParseManifest(std::string_view json, std::string_view platform);

class Checker {
public:
    void Start();
    void Fake(std::string_view version);
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
