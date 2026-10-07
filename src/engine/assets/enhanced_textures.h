#pragma once
#include "engine/assets/texture_cache.h"
#include <atomic>
#include <mutex>
#include <thread>

namespace pt {
uint64_t EnhancedTextureKey(const FtexTexture& source, uint64_t model);
uint64_t EnhancedModelKey(const std::filesystem::path& runtime);
std::filesystem::path EnhancedCacheFile(const std::filesystem::path& root, std::string_view stem);

class EnhancedTextureJob {
public:
    enum class State { Idle, Running, Ready, Failed, Cancelled };
    struct Status { State state = State::Idle; uint32_t done = 0; uint32_t total = 0; std::string note; };
    ~EnhancedTextureJob();
    bool Start(const std::filesystem::path& game, const std::filesystem::path& cache,
               const std::filesystem::path& runtime, uint32_t limit = 0, uint32_t max_output = 0);
    void Cancel();
    Status GetStatus() const;
private:
    void Run(std::filesystem::path game, std::filesystem::path cache, std::filesystem::path runtime, uint32_t limit, uint32_t max_output);
    void Publish(State state, uint32_t done, uint32_t total, std::string note);
    std::atomic<bool> cancel_{false};
    std::thread worker_;
    mutable std::mutex mutex_;
    Status status_;
};
}
