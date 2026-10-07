#include "engine/core/crash_report.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>

#include <SDL3/SDL.h>

#include "engine/core/log.h"

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#endif

namespace pt {
namespace {

std::filesystem::path g_dump_dir;
std::string g_build = "dev";
std::atomic<bool> g_exit_logged{false};
std::atomic<int> g_dumping{0};

#ifdef _WIN32
LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* info) {
    char reason[96];
    std::snprintf(reason, sizeof(reason), "unhandled exception %#lx at %p", info->ExceptionRecord->ExceptionCode,
                  info->ExceptionRecord->ExceptionAddress);
    WriteCrashDump(reason, info);
    LogExit(static_cast<int>(info->ExceptionRecord->ExceptionCode), "crash");
    return EXCEPTION_CONTINUE_SEARCH;
}

void OnPureCall() {
    WriteCrashDump("pure virtual function call");
    LogExit(3, "pure virtual call");
    std::_Exit(3);
}

void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
    WriteCrashDump("invalid parameter passed to a C runtime function");
    LogExit(3, "invalid parameter");
    std::_Exit(3);
}

BOOL WINAPI OnConsoleControl(DWORD event) {
    const char* names[] = {"Ctrl+C", "Ctrl+Break", "console close", "?", "?", "logoff", "shutdown"};
    LogWarn("exit: console control event {} ({})", event, event < 7 ? names[event] : "?");
    LogExit(static_cast<int>(0xC000013A), "console control event");
    return FALSE;
}

bool SDLCALL OnWindowsMessage(void*, MSG* msg) {
    if (msg->message == WM_QUERYENDSESSION) {
        LogWarn("exit: Windows session ending (WM_QUERYENDSESSION)");
    } else if (msg->message == WM_ENDSESSION && msg->wParam) {
        LogExit(0, "Windows session end");
    }
    return true;
}
#endif

void OnSignalAbort(int) {
    WriteCrashDump("abort()");
    LogExit(3, "abort");
}

void OnAtExit() {
    LogExit(-1, "exit without a recorded code");
}

}

void InstallCrashReporting(const std::filesystem::path& dump_dir, const std::string& build) {
    g_dump_dir = dump_dir;
    g_build = build.empty() ? "dev" : build;
#ifdef _WIN32
    SetUnhandledExceptionFilter(OnUnhandledException);
    _set_purecall_handler(OnPureCall);
    _set_invalid_parameter_handler(OnInvalidParameter);
    SetConsoleCtrlHandler(OnConsoleControl, TRUE);
    SDL_SetWindowsMessageHook(OnWindowsMessage, nullptr);
#endif
    std::signal(SIGABRT, OnSignalAbort);
    std::atexit(OnAtExit);
    std::set_terminate([] {
        if (const std::exception_ptr error = std::current_exception()) {
            try {
                std::rethrow_exception(error);
            } catch (const std::exception& e) {
                LogError("terminate: uncaught exception: {}", e.what());
            } catch (...) {
                LogError("terminate: uncaught exception of an unknown type");
            }
        } else {
            LogError("terminate: called with no active exception");
        }
        WriteCrashDump("std::terminate");
        LogExit(3, "terminate");
        std::_Exit(3);
    });
    LogInfo("crash reporting: build {}, dumps to {}", g_build, g_dump_dir.empty() ? std::string(".") : g_dump_dir.string());
}

std::filesystem::path WriteCrashDump(const char* reason, void* exception_pointers) {
    if (g_dumping.fetch_add(1) != 0) {
        LogError("crash: {} (dump already written)", reason);
        return {};
    }
    std::filesystem::path path;
#ifdef _WIN32
    SYSTEMTIME t;
    GetLocalTime(&t);
    char name[96];
    std::snprintf(name, sizeof(name), "pt-crash-%s-%04u%02u%02u-%02u%02u%02u.dmp", g_build.c_str(), t.wYear, t.wMonth, t.wDay, t.wHour,
                  t.wMinute, t.wSecond);
    path = g_dump_dir / name;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), static_cast<EXCEPTION_POINTERS*>(exception_pointers), FALSE};
        const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
                                                     MiniDumpWithUnloadedModules);
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, exception_pointers ? &exception : nullptr, nullptr,
                          nullptr);
        CloseHandle(file);
    } else {
        path.clear();
    }
#endif
    LogError("crash: {}, dump {}", reason, path.empty() ? std::string("not written") : path.string());
    return path;
}

void FatalError(const std::string& reason, bool show_message, int code) {
    LogError("fatal: {}", reason);
    const std::filesystem::path dump = WriteCrashDump(reason.c_str());
    if (show_message) {
        const std::string text = "P.T. stopped: " + reason + "\n\nThe log and a crash dump were written to:\n" +
                                 (dump.empty() ? g_dump_dir.string() : dump.parent_path().string()) +
                                 "\n\nPlease send pt.log and the .dmp file with your report.";
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "P.T.", text.c_str(), nullptr);
    }
    LogExit(code, "fatal error");
    std::_Exit(code);
}

void LogExit(int code, const char* how) {
    if (g_exit_logged.exchange(true)) {
        return;
    }
    LogInfo("exit: code {} ({})", code, how);
}

}
