#pragma once

// The operating system layer: everything the engine needs from Windows or Linux beyond SDL (docs/linux.md). Each call has
// one implementation per platform in os.cpp; nothing else in the engine includes windows.h or POSIX headers for these.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace pt::os {

// SDL and external text paths use UTF-8; filesystem paths retain the platform's native encoding.
inline std::filesystem::path PathFromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}
inline std::string PathToUtf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

// fopen with a Unicode path (Windows needs the wide call for paths outside the ANSI code page)
FILE* OpenFile(const std::filesystem::path& path, const char* mode);
// fseek with 64-bit offsets
int SeekFile(FILE* file, int64_t offset, int origin);
// an environment variable, empty when unset
std::string GetEnv(const char* name);
uint32_t ProcessId();

// Runs a program without a console or shell, its output and errors into `log`, its input empty, at below-normal priority.
// Waits until it ends, `cancel` turns true (then it is killed) or `timeout` passes (killed, timed_out set).
struct ProcessResult {
    bool started = false;
    bool timed_out = false;
    bool cancelled = false;
    int exit_code = -1;
};
ProcessResult RunProcess(const std::filesystem::path& program, const std::vector<std::string>& args, const std::filesystem::path& working_dir,
                         const std::filesystem::path& log, const std::atomic<bool>& cancel, std::chrono::milliseconds timeout);

// An exclusive lock on a file, held until destroyed: a second holder (another running copy of the game) fails to get it.
class FileLock {
public:
    explicit FileLock(const std::filesystem::path& path);
    ~FileLock();
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    bool Held() const { return held_; }

private:
    bool held_ = false;
    intptr_t handle_ = -1;
};

}
