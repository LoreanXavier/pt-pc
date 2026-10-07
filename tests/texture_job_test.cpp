#include "engine/assets/enhanced_textures.h"
#include <SDL3/SDL.h>
#include <chrono>
#include <cstdio>
#include <map>

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5) { std::fprintf(stderr, "usage: pt_texture_job_test <game> <runtime> <new-cache-dir> [limit=1, 0=all]\n"); return 2; }
    const uint32_t limit = argc == 5 ? static_cast<uint32_t>(std::stoul(argv[4])) : 1;
    const std::filesystem::path game = argv[1], runtime = argv[2], cache = argv[3];
    if (std::filesystem::exists(cache)) { std::fprintf(stderr, "use a new scratch cache directory\n"); return 2; }
    int failures = 0;
    auto check = [&](bool ok, const char* name) { if (!ok) { std::printf("FAIL: %s\n", name); ++failures; } };
    auto wait = [&](pt::EnhancedTextureJob& job) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(limit ? 150 : 600);
        while (job.GetStatus().state == pt::EnhancedTextureJob::State::Running) {
            if (std::chrono::steady_clock::now() > deadline) { job.Cancel(); check(false, "job deadline"); break; }
            SDL_Delay(50);
        }
        const auto s = job.GetStatus();
        std::printf("job: %d, %u/%u: %s\n", static_cast<int>(s.state), s.done, s.total, s.note.c_str());
        return s;
    };
    pt::EnhancedTextureJob job;
    check(job.Start(game, cache, cache / "missing-runtime", 1), "missing runtime request accepted");
    check(wait(job).state == pt::EnhancedTextureJob::State::Failed, "missing runtime reported");
    const auto started = std::chrono::steady_clock::now();
    check(job.Start(game, cache, runtime, limit), "generation starts");
    auto result = wait(job);
    check(result.state == pt::EnhancedTextureJob::State::Ready && result.done > 0 && result.done == result.total, "textures generated");
    std::printf("generation: %.3f seconds\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    std::map<std::filesystem::path, std::filesystem::file_time_type> times;
    if (std::filesystem::exists(cache)) for (const auto& file : std::filesystem::directory_iterator(cache)) {
        if (file.path().extension() == ".pttex") times[file.path()] = file.last_write_time();
    }
    check(!times.empty(), "cache outputs exist");
    check(job.Start(game, cache, runtime, limit), "cache reuse starts");
    check(wait(job).state == pt::EnhancedTextureJob::State::Ready, "cache reuse ready");
    for (const auto& [file, time] : times) check(std::filesystem::last_write_time(file) == time, "cache file not regenerated");
    check(job.Start(game, cache, runtime, 1), "cancel request starts");
    job.Cancel();
    check(wait(job).state == pt::EnhancedTextureJob::State::Cancelled, "cancel terminates worker");
    const auto active_cache = cache / "cancel-active";
    check(job.Start(game, active_cache, runtime, 0), "active cancellation starts");
    const auto cancel_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!std::filesystem::exists(active_cache / "upscaler.log") &&
           job.GetStatus().state == pt::EnhancedTextureJob::State::Running && std::chrono::steady_clock::now() < cancel_deadline) SDL_Delay(10);
    check(std::filesystem::exists(active_cache / "upscaler.log"), "upscaler reached before cancellation");
    job.Cancel();
    check(wait(job).state == pt::EnhancedTextureJob::State::Cancelled, "active upscaler cancellation completes");
    std::printf("texture job: %d failures\n", failures);
    return failures ? 1 : 0;
}
