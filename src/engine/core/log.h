#pragma once

#include <cstdint>
#include <filesystem>
#include <format>
#include <string_view>
#include <utility>

namespace pt {

enum class LogLevel { Debug, Info, Warn, Error };

void LogWrite(LogLevel level, std::string_view text);
void LogSetFile(const std::filesystem::path& path);
// The main loop's frame; with PT_LOG_TICKS=1 every line carries it ("#2444" after the level), for timing events against captures
void LogSetTick(uint64_t tick);

template <typename... Args>
void LogDebug(std::format_string<Args...> fmt, Args&&... args) {
    LogWrite(LogLevel::Debug, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void LogInfo(std::format_string<Args...> fmt, Args&&... args) {
    LogWrite(LogLevel::Info, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void LogWarn(std::format_string<Args...> fmt, Args&&... args) {
    LogWrite(LogLevel::Warn, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void LogError(std::format_string<Args...> fmt, Args&&... args) {
    LogWrite(LogLevel::Error, std::format(fmt, std::forward<Args>(args)...));
}

}
