#include "engine/platform/os.h"
#include "engine/core/crash_report.h"

#include <algorithm>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>

#include <SDL3/SDL.h>

#include "engine/core/log.h"
#include "engine/core/memory_status.h"

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#else
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace pt {
namespace {

std::filesystem::path g_dump_dir;
std::string g_build = "dev";
std::string g_crash_note;
char g_tools_note[2][512] = {};
std::atomic<int> g_tools_note_index{-1};
std::atomic<bool> g_exit_logged{false};
std::atomic<int> g_dumping{0};

#ifdef _WIN32
void DescribeCppException(const EXCEPTION_RECORD* record, char* out, size_t size) {
    out[0] = 0;
    if (record->ExceptionCode != 0xE06D7363 || record->NumberParameters < 4 || record->ExceptionInformation[0] != 0x19930520) return;
    __try {
        const uintptr_t object = record->ExceptionInformation[1];
        const uintptr_t base = record->ExceptionInformation[3];
        const auto* throw_info = reinterpret_cast<const int32_t*>(record->ExceptionInformation[2]);
        if (!throw_info || !base) return;
        const auto* types = reinterpret_cast<const int32_t*>(base + throw_info[3]);
        const char* thrown = nullptr;
        const std::exception* standard = nullptr;
        for (int32_t i = 0; i < types[0] && i < 64; ++i) {
            const auto* type = reinterpret_cast<const int32_t*>(base + types[1 + i]);
            const char* name = reinterpret_cast<const char*>(base + type[1] + 16);
            if (!thrown) thrown = name;
            if (type[3] == -1 && std::strcmp(name, ".?AVexception@std@@") == 0) standard = reinterpret_cast<const std::exception*>(object + type[2]);
        }
        const char* what = standard ? standard->what() : nullptr;
        std::snprintf(out, size, " (C++ %s%s%s)", thrown ? thrown : "?", what ? ": " : "", what ? what : "");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::snprintf(out, size, " (C++ exception, unreadable)");
    }
}

const char* OutOfMemoryText() {
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
        case LANG_ARABIC: return "نفدت ذاكرة ويندوز (الذاكرة الافتراضية). أغلق البرامج الأخرى أو كبّر ملف الترحيل (page file) من إعدادات ويندوز، ثم شغّل P.T. مرة أخرى.";
        case LANG_CZECH: return "Windows došla paměť (virtuální paměť). Zavřete ostatní programy nebo zvětšete stránkovací soubor v nastavení Windows a spusťte P.T. znovu.";
        case LANG_CHINESE: return "Windows 内存不足（虚拟内存已用尽）。请关闭其他程序，或在 Windows 设置中增大页面文件，然后重新启动 P.T.。";
        case LANG_FRENCH: return "Windows n’a plus de mémoire (mémoire virtuelle). Fermez d’autres programmes ou agrandissez le fichier d’échange dans les paramètres de Windows, puis relancez P.T.";
        case LANG_GERMAN: return "Windows hat keinen Arbeitsspeicher mehr (virtueller Speicher). Schließen Sie andere Programme oder vergrößern Sie die Auslagerungsdatei in den Windows-Einstellungen und starten Sie P.T. neu.";
        case LANG_ITALIAN: return "Windows ha esaurito la memoria (memoria virtuale). Chiudi altri programmi o ingrandisci il file di paging nelle impostazioni di Windows, poi riavvia P.T.";
        case LANG_JAPANESE: return "Windows のメモリ（仮想メモリ）が不足しています。他のプログラムを閉じるか、Windows の設定でページファイルを大きくしてから、P.T. をもう一度起動してください。";
        case LANG_POLISH: return "Systemowi Windows zabrakło pamięci (pamięci wirtualnej). Zamknij inne programy lub powiększ plik stronicowania w ustawieniach Windows, a następnie uruchom P.T. ponownie.";
        case LANG_PORTUGUESE: return "O Windows ficou sem memória (memória virtual). Feche outros programas ou aumente o arquivo de paginação nas configurações do Windows e inicie o P.T. novamente.";
        case LANG_RUSSIAN: return "В Windows закончилась память (виртуальная память). Закройте другие программы или увеличьте файл подкачки в настройках Windows и запустите P.T. снова.";
        case LANG_SPANISH: return "Windows se ha quedado sin memoria (memoria virtual). Cierra otros programas o aumenta el archivo de paginación en la configuración de Windows y vuelve a iniciar P.T.";
        case LANG_TURKISH: return "Windows’un belleği (sanal bellek) tükendi. Diğer programları kapatın veya Windows ayarlarından sayfa dosyasını büyütün, sonra P.T.’yi yeniden başlatın.";
        case LANG_UKRAINIAN: return "У Windows закінчилася пам’ять (віртуальна пам’ять). Закрийте інші програми або збільште файл підкачки в налаштуваннях Windows і запустіть P.T. знову.";
        default: return "Windows has run out of memory (virtual memory). Close other programs or enlarge the page file in Windows settings, then start P.T. again.";
    }
}

void TellOutOfMemory(const char* thrown) {
    if (!std::strstr(thrown, "bad_alloc")) return;
    if (QuerySystemMemory().commit_headroom_mb > 400.0) return;
    const char* driver = SDL_GetCurrentVideoDriver();
    if (!driver || !std::strcmp(driver, "offscreen") || !std::strcmp(driver, "dummy")) return;
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "P.T.", OutOfMemoryText(), nullptr);
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* info) {
    char thrown[384];
    DescribeCppException(info->ExceptionRecord, thrown, sizeof(thrown));
    char reason[512];
    std::snprintf(reason, sizeof(reason), "unhandled exception %#lx at %p%s", info->ExceptionRecord->ExceptionCode,
                  info->ExceptionRecord->ExceptionAddress, thrown);
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        struct Job {
            const char* reason;
            EXCEPTION_POINTERS* info;
            DWORD thread;
        } job{reason, info, GetCurrentThreadId()};
        const HANDLE writer = CreateThread(
            nullptr, 256 * 1024,
            [](void* p) -> DWORD {
                auto* j = static_cast<Job*>(p);
                WriteCrashDumpFor(j->reason, j->info, j->thread);
                return 0;
            },
            &job, 0, nullptr);
        if (writer) {
            WaitForSingleObject(writer, 30000);
            CloseHandle(writer);
        }
    } else {
        WriteCrashDump(reason, info);
    }
    LogExit(static_cast<int>(info->ExceptionRecord->ExceptionCode), "crash");
    TellOutOfMemory(thrown);
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

#ifndef _WIN32
void OnFatalSignal(int sig, siginfo_t* info, void*) {
    const char* name = sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : sig == SIGILL ? "SIGILL" : sig == SIGFPE ? "SIGFPE"
                       : sig == SIGABRT ? "SIGABRT (abort)" : "signal";
    if (g_dumping.fetch_add(1) == 0) {
        LogError("crash: {} ({}) at address {}", name, sig, info ? info->si_addr : nullptr);
        void* frames[64];
        const int count = backtrace(frames, 64);
        if (char** lines = backtrace_symbols(frames, count)) {
            for (int i = 0; i < count; ++i) LogError("crash: #{} {}", i, lines[i]);
        }
        LogExit(128 + sig, "crash");
    }
    raise(sig);
}

void InstallFaultHandlers() {
    static char alt_stack[64 * 1024];
    stack_t ss{};
    ss.ss_sp = alt_stack;
    ss.ss_size = sizeof(alt_stack);
    sigaltstack(&ss, nullptr);
    struct sigaction action{};
    action.sa_sigaction = OnFatalSignal;
    action.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(sig, &action, nullptr);
}

void RedirectStderr(const std::filesystem::path& dir) {
    if (isatty(STDERR_FILENO)) return;
    const std::filesystem::path path = (dir.empty() ? std::filesystem::path(".") : dir) / "pt-stderr.log";
    if (FILE* file = std::fopen(path.c_str(), "w")) {
        std::fflush(stderr);
        dup2(fileno(file), STDERR_FILENO);
        std::fclose(file);
        setvbuf(stderr, nullptr, _IONBF, 0);
        LogInfo("crash reporting: driver and library messages (stderr) go to {}", path.string());
    }
}
#else
void OnSignalAbort(int) {
    WriteCrashDump("abort()");
    LogExit(3, "abort");
}
#endif

void OnAtExit() {
    LogExit(-1, "exit without a recorded code");
}

}

void InstallCrashReporting(const std::filesystem::path& dump_dir, const std::string& build) {
    g_dump_dir = dump_dir;
    g_build = build.empty() ? "dev" : build;
#ifdef _WIN32
    SetUnhandledExceptionFilter(OnUnhandledException);
    ReserveCrashStack();
    _set_purecall_handler(OnPureCall);
    _set_invalid_parameter_handler(OnInvalidParameter);
    SetConsoleCtrlHandler(OnConsoleControl, TRUE);
    SDL_SetWindowsMessageHook(OnWindowsMessage, nullptr);
    std::signal(SIGABRT, OnSignalAbort);
#else
    InstallFaultHandlers();
    RedirectStderr(dump_dir);
#endif
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
    LogInfo("crash reporting: build {}, dumps to {}", g_build, g_dump_dir.empty() ? std::string(".") : pt::os::PathToUtf8(g_dump_dir));
}

std::filesystem::path CrashDumpDir() {
    return g_dump_dir;
}

const std::string& CrashBuildName() {
    return g_build;
}

namespace {
std::filesystem::path WriteDumpFile(const char* suffix, void* exception_pointers, unsigned long thread_id) {
    std::filesystem::path path;
#ifdef _WIN32
    SYSTEMTIME t;
    GetLocalTime(&t);
    char name[128];
    std::snprintf(name, sizeof(name), "pt-crash-%s-%04u%02u%02u-%02u%02u%02u%s.dmp", g_build.c_str(), t.wYear, t.wMonth, t.wDay, t.wHour,
                  t.wMinute, t.wSecond, suffix);
    path = g_dump_dir / name;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exception{thread_id, static_cast<EXCEPTION_POINTERS*>(exception_pointers), FALSE};
        const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
                                                     MiniDumpWithUnloadedModules);
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, exception_pointers ? &exception : nullptr, nullptr,
                          nullptr);
        CloseHandle(file);
    } else {
        path.clear();
    }
#else
    (void)suffix;
    (void)exception_pointers;
    (void)thread_id;
#endif
    return path;
}
}

std::filesystem::path WriteCrashDump(const char* reason, void* exception_pointers) {
#ifdef _WIN32
    return WriteCrashDumpFor(reason, exception_pointers, GetCurrentThreadId());
#else
    return WriteCrashDumpFor(reason, exception_pointers, 0);
#endif
}

std::filesystem::path WriteCrashDumpFor(const char* reason, void* exception_pointers, unsigned long thread_id) {
    if (g_dumping.fetch_add(1) != 0) {
        LogError("crash: {} (dump already written)", reason);
        return {};
    }
    const std::filesystem::path path = WriteDumpFile("", exception_pointers, thread_id);
    LogError("crash: {}, dump {}", reason, path.empty() ? std::string("not written") : pt::os::PathToUtf8(path));
    if (!g_crash_note.empty()) LogError("crash: {}", g_crash_note);
    if (const int tools = g_tools_note_index.load(); tools >= 0 && g_tools_note[tools][0]) LogError("crash: third party: {}", g_tools_note[tools]);
    return path;
}

std::filesystem::path WriteHangDump(const char* reason) {
    #ifdef _WIN32
    const std::filesystem::path path = WriteDumpFile("-hang", nullptr, GetCurrentThreadId());
#else
    const std::filesystem::path path = WriteDumpFile("-hang", nullptr, 0);
#endif
    LogWarn("hang: {}, dump {}", reason, path.empty() ? std::string("not written") : pt::os::PathToUtf8(path));
    return path;
}

void CheckCrashFilter(const char* after) {
#ifdef _WIN32
    const LPTOP_LEVEL_EXCEPTION_FILTER previous = SetUnhandledExceptionFilter(OnUnhandledException);
    if (previous == OnUnhandledException) return;
    HMODULE module = nullptr;
    wchar_t file[MAX_PATH] = L"";
    if (previous && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCWSTR>(reinterpret_cast<void*>(previous)), &module)) {
        GetModuleFileNameW(module, file, MAX_PATH);
    }
    LogWarn("crash reporting: the unhandled exception filter was replaced during {} ({}{}); the game's is set again", after,
            previous ? "by " : "removed", previous ? (file[0] ? pt::os::PathToUtf8(std::filesystem::path(file)) : std::string("an unknown module")) : "");
#else
    (void)after;
#endif
}

void ReserveCrashStack() {
#ifdef _WIN32
    ULONG size = 64 * 1024;
    SetThreadStackGuarantee(&size);
#endif
}

bool ExitLogged() {
    return g_exit_logged.load();
}

void SetCrashNote(const std::string& note) {
    g_crash_note = note;
}

void SetThirdPartyNote(const std::string& tools) {
    const int next = g_tools_note_index.load() == 0 ? 1 : 0;
    const size_t n = std::min(tools.size(), sizeof(g_tools_note[next]) - 1);
    std::memcpy(g_tools_note[next], tools.data(), n);
    g_tools_note[next][n] = '\0';
    g_tools_note_index.store(next);
}

void FatalError(const std::string& reason, bool show_message, int code) {
    LogError("fatal: {}", reason);
    const std::filesystem::path dump = WriteCrashDump(reason.c_str());
    if (show_message) {
        const std::string text = "P.T. stopped: " + reason + "\n\nThe log and a crash dump were written to:\n" +
                                 (dump.empty() ? pt::os::PathToUtf8(g_dump_dir) : pt::os::PathToUtf8(dump.parent_path())) +
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
