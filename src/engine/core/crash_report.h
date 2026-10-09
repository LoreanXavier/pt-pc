#pragma once

#include <filesystem>
#include <string>

namespace pt {

// Crash and exit reporting. Every way the process ends that it controls leaves a last line in pt.log: an unhandled structured
// exception, std::terminate, a pure virtual call, an invalid CRT parameter and abort() write a minidump next to pt.log
// (pt-crash-<build>-<time>.dmp, threads, stacks and the memory they point to) and log its name; a console control event, a
// Windows session end and a normal exit log what ended the process and the exit code. A process that ends with no last line
// was ended from outside (TerminateProcess, taskkill /F) or by the system.
void InstallCrashReporting(const std::filesystem::path& dump_dir, const std::string& build);

// Writes a minidump now (no exception record when info is null) and logs "crash: <reason>, dump <path>"; returns the path.
std::filesystem::path WriteCrashDump(const char* reason, void* exception_pointers = nullptr);

// Logs the reason, writes a dump, shows a message box when a window exists, logs "exit: <code>" and ends the process.
[[noreturn]] void FatalError(const std::string& reason, bool show_message, int code = 3);

// The exit line of a normal end: "exit: code <code> (<how>)". Logged once; later calls do nothing.
void LogExit(int code, const char* how);

}
