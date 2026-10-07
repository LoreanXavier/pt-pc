#pragma once

#include <filesystem>
#include <string>

namespace pt {

void InstallCrashReporting(const std::filesystem::path& dump_dir, const std::string& build);

std::filesystem::path WriteCrashDump(const char* reason, void* exception_pointers = nullptr);

[[noreturn]] void FatalError(const std::string& reason, bool show_message, int code = 3);

void LogExit(int code, const char* how);

}
