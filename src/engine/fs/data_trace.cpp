#include "engine/fs/data_trace.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "engine/platform/os.h"

namespace pt {
namespace {

std::mutex g_mutex;
std::FILE* g_out = nullptr;
std::unordered_set<std::string> g_seen;
std::vector<std::string> g_files;

void Write(std::string line) {
    std::lock_guard lock(g_mutex);
    if (g_out && g_seen.insert(line).second) {
        std::fputs(line.c_str(), g_out);
        std::fputc('\n', g_out);
        std::fflush(g_out);
    }
}

}

bool DataTraceOn() {
    static const bool on = [] {
        const char* path = std::getenv("PT_DATA_TRACE");
        if (!path || !*path) {
            return false;
        }
        std::filesystem::path file(path);
        std::error_code error;
        if (std::filesystem::is_directory(file, error)) {
            file /= std::format("data_trace_{}.txt", os::ProcessId());
        }
        g_out = os::OpenFile(file, "ab");
        return g_out != nullptr;
    }();
    return on;
}

void DataTraceFile(std::string_view path) {
    if (DataTraceOn()) {
        Write(std::format("file\t{}", path));
    }
}

uint32_t DataTraceFileId(std::string_view file) {
    if (!DataTraceOn()) {
        return 0;
    }
    std::lock_guard lock(g_mutex);
    for (size_t i = 0; i < g_files.size(); ++i) {
        if (g_files[i] == file) {
            return static_cast<uint32_t>(i);
        }
    }
    g_files.emplace_back(file);
    return static_cast<uint32_t>(g_files.size() - 1);
}

void DataTraceProperty(uint32_t file, std::string_view class_name, std::string_view property) {
    if (!DataTraceOn()) {
        return;
    }
    std::string name;
    {
        std::lock_guard lock(g_mutex);
        if (file < g_files.size()) {
            name = g_files[file];
        }
    }
    Write(std::format("prop\t{}\t{}\t{}", name, class_name, property));
}

}
