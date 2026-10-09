#pragma once

#include <cstddef>

namespace pt {

// One line in the log with the process's committed memory and the system's commit charge and limit (Windows), for allocation
// failures: a std::bad_alloc while the system's commit is spent is another process's doing, one with commit to spare is a bad
// size here. `what` names the allocation and `bytes` its size (0 when unknown).
void LogMemoryStatus(const char* what, size_t bytes);

// Installs a new handler that logs LogMemoryStatus once on the first failed allocation and lets it throw as before.
void InstallAllocationFailureLog();

// The system's free physical memory and commit headroom (commit limit minus commit charge), in MB.
struct SystemMemory {
    double free_physical_mb = 0.0;
    double commit_headroom_mb = 0.0;
};
SystemMemory QuerySystemMemory();

// The calling thread's CPU cycles and the process's page fault count (Windows; zeros elsewhere): taken around a section that
// sometimes runs long, the deltas tell work (cycles) from waiting (few cycles: the thread was descheduled or faulted pages in)
struct ThreadCost {
    unsigned long long cycles = 0;
    unsigned long page_faults = 0;
};
ThreadCost QueryThreadCost();

// Start gate for a headless run: waits up to `seconds` for `free_mb` of free physical memory and `free_mb` of commit headroom,
// logging the wait; false when they never came (the caller exits instead of adding a 2 to 3 GB process to a full machine).
bool WaitForFreeMemory(double free_mb, int seconds);

// Memory guard, called once a frame (cheap: it looks every 2 s). Below `low_mb` of free physical memory or commit headroom,
// or with the GPU's device local memory over `vram_fraction` of the driver's budget, it logs a "memory: low" line (at most
// every 30 s) and returns true, so the caller backs off (drops what it can rebuild).
bool MemoryLow(double low_mb, double vram_used_mb, double vram_budget_mb, double vram_fraction);

}
