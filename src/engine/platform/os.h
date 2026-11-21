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
bool SameFileName(const std::filesystem::path& a, const std::filesystem::path& b);
bool IsLink(const std::filesystem::path& path);
bool IsLinkReparseTag(uint32_t tag);
std::filesystem::path ExecutableFile();
int SeekFile(FILE* file, int64_t offset, int origin);
std::string GetEnv(const char* name);
uint32_t ProcessId();
void LowerCurrentThreadPriority();

struct ProcessResult {
    bool started = false;
    bool timed_out = false;
    bool cancelled = false;
    int exit_code = -1;
};
ProcessResult RunProcess(const std::filesystem::path& program, const std::vector<std::string>& args, const std::filesystem::path& working_dir,
                         const std::filesystem::path& log, const std::atomic<bool>& cancel, std::chrono::milliseconds timeout);

bool StartDetached(const std::filesystem::path& program, const std::vector<std::string>& args, const std::filesystem::path& working_dir);
bool ProcessAlive(uint64_t pid);

struct ForegroundWindow {
    uint64_t handle = 0;
    uint32_t pid = 0;
    std::string title;
    std::string window_class;
    std::string process;
};
ForegroundWindow GetForegroundWindowInfo(bool details = true);
bool IsForegroundWindow(void* window);
bool ForegroundIsOwnProcess();
bool ActivateWindow(void* window);
void ClearWindow(void* window);
void AllowForeground(uint32_t pid);

class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    bool Start(const std::filesystem::path& program, const std::vector<std::string>& args, const std::filesystem::path& working_dir);
    bool Started() const { return handle_ != kNone; }
    bool Running(int* exit_code = nullptr);

private:
    static constexpr intptr_t kNone = -1;
    intptr_t handle_ = kNone;
};

std::filesystem::path ExtendedLengthPath(const std::filesystem::path& path);
std::wstring QuoteWindowsArgument(const std::wstring& arg);

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

std::optional<bool> WindowDisplayHdr(void* hwnd);

struct DisplayHdrInfo {
    bool hdr = false;
    float max_luminance = 0.0f;
    float min_luminance = 0.0f;
};
std::optional<DisplayHdrInfo> WindowDisplayHdrInfo(void* hwnd);

bool WindowDisplayHdrCapable(void* hwnd);
bool SetWindowDisplayHdr(void* hwnd, bool on);

}
