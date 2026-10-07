#pragma once

#include <cstddef>

namespace pt {

void LogMemoryStatus(const char* what, size_t bytes);

void InstallAllocationFailureLog();

struct SystemMemory {
    double free_physical_mb = 0.0;
    double commit_headroom_mb = 0.0;
};
SystemMemory QuerySystemMemory();

struct ThreadCost {
    unsigned long long cycles = 0;
    unsigned long page_faults = 0;
};
ThreadCost QueryThreadCost();

bool WaitForFreeMemory(double free_mb, int seconds);

bool MemoryLow(double low_mb, double vram_used_mb, double vram_budget_mb, double vram_fraction);

}
