#include "engine/core/log.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace pt {
namespace {

std::mutex g_log_mutex;
FILE* g_log_file = nullptr;
uint64_t g_tick = 0;
const bool g_log_ticks = [] {
    const char* value = std::getenv("PT_LOG_TICKS");
    return value && *value == '1';
}();

const char* LevelTag(LogLevel level) {
    switch (level) {
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warn: return "warn";
    case LogLevel::Error: return "error";
    }
    return "?";
}

}

void LogSetFile(const char* path) {
    std::lock_guard lock(g_log_mutex);
    if (g_log_file) {
        std::fclose(g_log_file);
    }
    g_log_file = std::fopen(path, "w");
}

void LogWrite(LogLevel level, std::string_view text) {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    const double seconds = duration<double>(steady_clock::now() - start).count();
    std::lock_guard lock(g_log_mutex);
    char tick[32] = "";
    if (g_log_ticks) {
        std::snprintf(tick, sizeof(tick), "#%llu ", static_cast<unsigned long long>(g_tick));
    }
    std::fprintf(stderr, "[%9.3f] %-5s %s%.*s\n", seconds, LevelTag(level), tick, static_cast<int>(text.size()), text.data());
    if (g_log_file) {
        std::fprintf(g_log_file, "[%9.3f] %-5s %s%.*s\n", seconds, LevelTag(level), tick, static_cast<int>(text.size()), text.data());
        std::fflush(g_log_file);
    }
}

void LogSetTick(uint64_t tick) {
    std::lock_guard lock(g_log_mutex);
    g_tick = tick;
}

}
