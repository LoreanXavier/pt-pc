#pragma once

#include <filesystem>
#include <string>

namespace pt {

void InstallCrashReporting(const std::filesystem::path& dump_dir, const std::string& build);

std::filesystem::path CrashDumpDir();
const std::string& CrashBuildName();

std::filesystem::path WriteCrashDump(const char* reason, void* exception_pointers = nullptr);
std::filesystem::path WriteCrashDumpFor(const char* reason, void* exception_pointers, unsigned long thread_id);

void SetCrashNote(const std::string& note);
void SetThirdPartyNote(const std::string& tools);

[[noreturn]] void FatalError(const std::string& reason, bool show_message, int code = 3);

std::filesystem::path WriteHangDump(const char* reason);

void CheckCrashFilter(const char* after);

void ReserveCrashStack();

bool ExitLogged();

void LogExit(int code, const char* how);

}
