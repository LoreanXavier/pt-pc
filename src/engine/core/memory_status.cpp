#include "engine/core/memory_status.h"

#include <chrono>
#include <cstdio>
#include <new>

#include "engine/core/log.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace pt {

ThreadCost QueryThreadCost() {
    ThreadCost cost;
#ifdef _WIN32
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    cost.cycles = cycles;
    PROCESS_MEMORY_COUNTERS process{};
    process.cb = sizeof(process);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &process, sizeof(process))) {
        cost.page_faults = process.PageFaultCount;
    }
#endif
    return cost;
}

void LogMemoryStatus(const char* what, size_t bytes) {
    /* Stack buffer on purpose: this runs right after an allocation has failed. */
    char line[320];
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX process{};
    process.cb = sizeof(process);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process), sizeof(process));
    MEMORYSTATUSEX system{};
    system.dwLength = sizeof(system);
    GlobalMemoryStatusEx(&system);
    const unsigned long long mb = 1024ull * 1024ull;
    std::snprintf(line, sizeof(line),
                  "memory: %s (%zu bytes): process commit %llu MB, working set %llu MB; system commit %llu MB used of %llu MB, "
                  "%llu MB physical free",
                  what, bytes, static_cast<unsigned long long>(process.PrivateUsage) / mb,
                  static_cast<unsigned long long>(process.WorkingSetSize) / mb,
                  (system.ullTotalPageFile - system.ullAvailPageFile) / mb, system.ullTotalPageFile / mb, system.ullAvailPhys / mb);
#else
    std::snprintf(line, sizeof(line), "memory: %s (%zu bytes)", what, bytes);
#endif
    std::fprintf(stderr, "%s\n", line);
    try {
        LogError("{}", line);
    } catch (...) {
    }
}

SystemMemory QuerySystemMemory() {
    SystemMemory out;
#ifdef _WIN32
    MEMORYSTATUSEX system{};
    system.dwLength = sizeof(system);
    if (GlobalMemoryStatusEx(&system)) {
        out.free_physical_mb = static_cast<double>(system.ullAvailPhys) / 1048576.0;
        out.commit_headroom_mb = static_cast<double>(system.ullAvailPageFile) / 1048576.0;
    }
#else
    out.free_physical_mb = out.commit_headroom_mb = 1.0e9;
#endif
    return out;
}

bool WaitForFreeMemory(double free_mb, int seconds) {
    for (int waited = 0;; ++waited) {
        const SystemMemory m = QuerySystemMemory();
        if (m.free_physical_mb >= free_mb && m.commit_headroom_mb >= free_mb) {
            if (waited > 0) {
                LogInfo("memory: start after {} s, {:.0f} MB free, {:.0f} MB commit headroom", waited, m.free_physical_mb, m.commit_headroom_mb);
            }
            return true;
        }
        if (waited == 0) {
            LogWarn("memory: {:.0f} MB free, {:.0f} MB commit headroom, waiting up to {} s for {:.0f} MB",
                    m.free_physical_mb, m.commit_headroom_mb, seconds, free_mb);
        }
        if (waited >= seconds) {
            LogError("memory: {:.0f} MB free, {:.0f} MB commit headroom after {} s, not starting", m.free_physical_mb,
                     m.commit_headroom_mb, seconds);
            return false;
        }
#ifdef _WIN32
        Sleep(1000);
#endif
    }
}

bool MemoryLow(double low_mb, double vram_used_mb, double vram_budget_mb, double vram_fraction) {
    using clock = std::chrono::steady_clock;
    static clock::time_point last_check{};
    static clock::time_point last_log{};
    static bool low = false;
    const clock::time_point now = clock::now();
    if (now - last_check < std::chrono::seconds(2)) {
        return low;
    }
    last_check = now;
    const SystemMemory m = QuerySystemMemory();
    const bool vram_low = vram_budget_mb > 0.0 && vram_used_mb > vram_budget_mb * vram_fraction;
    low = m.free_physical_mb < low_mb || m.commit_headroom_mb < low_mb || vram_low;
    if (low && (last_log.time_since_epoch().count() == 0 || now - last_log > std::chrono::seconds(30))) {
        last_log = now;
        LogMemoryStatus(vram_low ? "low on GPU memory, backing off" : "low on system memory, backing off", 0);
        if (vram_budget_mb > 0.0) {
            LogWarn("memory: device local {:.0f} of {:.0f} MB budget", vram_used_mb, vram_budget_mb);
        }
    }
    return low;
}

void InstallAllocationFailureLog() {
    std::set_new_handler([] {
        /* One shot: after the log the retry throws bad_alloc, as it would have without a handler. */
        std::set_new_handler(nullptr);
        LogMemoryStatus("allocation failed", 0);
    });
}

}
