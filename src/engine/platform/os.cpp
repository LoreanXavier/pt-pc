#include "engine/platform/os.h"

#include <cstdlib>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace pt::os {

#ifdef _WIN32

FILE* OpenFile(const std::filesystem::path& path, const char* mode) {
    std::wstring wide;
    for (const char* c = mode; *c; ++c) wide.push_back(static_cast<wchar_t>(*c));
    return _wfopen(path.c_str(), wide.c_str());
}

int SeekFile(FILE* file, int64_t offset, int origin) { return _fseeki64(file, offset, origin); }

std::string GetEnv(const char* name) {
    std::wstring key;
    for (const char* c = name; *c; ++c) key.push_back(static_cast<wchar_t>(*c));
    const DWORD size = GetEnvironmentVariableW(key.c_str(), nullptr, 0);
    if (!size) return {};
    std::wstring value(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(key.c_str(), value.data(), size);
    if (!written || written >= size) return {};
    value.resize(written);
    return PathToUtf8(std::filesystem::path(value));
}

uint32_t ProcessId() { return GetCurrentProcessId(); }

ProcessResult RunProcess(const std::filesystem::path& program, const std::vector<std::string>& args, const std::filesystem::path& working_dir,
                         const std::filesystem::path& log, const std::atomic<bool>& cancel, std::chrono::milliseconds timeout) {
    ProcessResult result;
    std::wstring command = L"\"" + program.wstring() + L"\"";
    for (const auto& arg : args) command += L" \"" + std::filesystem::path(reinterpret_cast<const char8_t*>(arg.c_str())).wstring() + L"\"";
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE log_file = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log_file == INVALID_HANDLE_VALUE) return result;
    HANDLE input_handle = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = log_file;
    startup.hStdError = log_file;
    startup.hStdInput = input_handle;
    PROCESS_INFORMATION process{};
    result.started = CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
                                    working_dir.c_str(), &startup, &process);
    CloseHandle(log_file);
    if (input_handle != INVALID_HANDLE_VALUE) CloseHandle(input_handle);
    if (!result.started) return result;
    CloseHandle(process.hThread);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (WaitForSingleObject(process.hProcess, 100) == WAIT_TIMEOUT) {
        if (cancel || std::chrono::steady_clock::now() > deadline) {
            result.cancelled = cancel;
            result.timed_out = !cancel;
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, INFINITE);
            break;
        }
    }
    DWORD exit = 1;
    GetExitCodeProcess(process.hProcess, &exit);
    CloseHandle(process.hProcess);
    result.exit_code = static_cast<int>(exit);
    return result;
}

FileLock::FileLock(const std::filesystem::path& path) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    held_ = file != INVALID_HANDLE_VALUE;
    handle_ = reinterpret_cast<intptr_t>(file);
}

FileLock::~FileLock() {
    if (held_) CloseHandle(reinterpret_cast<HANDLE>(handle_));
}

#else

FILE* OpenFile(const std::filesystem::path& path, const char* mode) { return std::fopen(path.c_str(), mode); }

int SeekFile(FILE* file, int64_t offset, int origin) { return fseeko(file, static_cast<off_t>(offset), origin); }

std::string GetEnv(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

uint32_t ProcessId() { return static_cast<uint32_t>(getpid()); }

ProcessResult RunProcess(const std::filesystem::path& program, const std::vector<std::string>& args, const std::filesystem::path& working_dir,
                         const std::filesystem::path& log, const std::atomic<bool>& cancel, std::chrono::milliseconds timeout) {
    ProcessResult result;
    const int log_fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (log_fd < 0) return result;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, log_fd, 1);
    posix_spawn_file_actions_adddup2(&actions, log_fd, 2);
    posix_spawn_file_actions_addchdir_np(&actions, working_dir.c_str());
    std::vector<std::string> owned{program.string()};
    owned.insert(owned.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& arg : owned) argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t pid = -1;
    result.started = posix_spawn(&pid, program.c_str(), &actions, nullptr, argv.data(), environ) == 0;
    posix_spawn_file_actions_destroy(&actions);
    close(log_fd);
    if (!result.started) return result;
    setpriority(PRIO_PROCESS, static_cast<id_t>(pid), 10);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int status = 0;
    for (;;) {
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) break;
        if (done < 0) return result;
        if (cancel || std::chrono::steady_clock::now() > deadline) {
            result.cancelled = cancel;
            result.timed_out = !cancel;
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    return result;
}

FileLock::FileLock(const std::filesystem::path& path) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return;
    }
    held_ = true;
    handle_ = fd;
}

FileLock::~FileLock() {
    if (held_) close(static_cast<int>(handle_));
}

#endif

}
