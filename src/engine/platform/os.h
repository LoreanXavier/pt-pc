#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace pt::os {

inline std::filesystem::path PathFromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}
inline std::string PathToUtf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

FILE* OpenFile(const std::filesystem::path& path, const char* mode);
int SeekFile(FILE* file, int64_t offset, int origin);
std::string GetEnv(const char* name);
uint32_t ProcessId();

struct ProcessResult {
    bool started = false;
    bool timed_out = false;
    bool cancelled = false;
    int exit_code = -1;
};
ProcessResult RunProcess(const std::filesystem::path& program, const std::vector<std::string>& args, const std::filesystem::path& working_dir,
                         const std::filesystem::path& log, const std::atomic<bool>& cancel, std::chrono::milliseconds timeout);

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
