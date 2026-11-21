#include "engine/platform/third_party.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace pt::third_party {
namespace {

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

#ifdef _WIN32
std::string Narrow(const wchar_t* text) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
    return out;
}

std::string PdbPath(const BYTE* base, DWORD size) {
    if (!base || size < sizeof(IMAGE_DOS_HEADER)) {
        return {};
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || static_cast<DWORD>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > size) {
        return {};
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG) {
        return {};
    }
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (!dir.VirtualAddress || dir.VirtualAddress + dir.Size > size) {
        return {};
    }
    const auto* entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base + dir.VirtualAddress);
    for (DWORD i = 0; i < dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY); ++i) {
        const IMAGE_DEBUG_DIRECTORY& e = entries[i];
        if (e.Type != IMAGE_DEBUG_TYPE_CODEVIEW || !e.AddressOfRawData || e.SizeOfData < 25 || e.AddressOfRawData + e.SizeOfData > size) {
            continue;
        }
        const BYTE* cv = base + e.AddressOfRawData;
        if (std::memcmp(cv, "RSDS", 4) != 0) {
            continue;
        }
        const char* path = reinterpret_cast<const char*>(cv + 24);
        return std::string(path, strnlen(path, e.SizeOfData - 24));
    }
    return {};
}
#endif

}

std::string ProxyToolForPdb(const std::string& pdb) {
    const std::string p = Lower(pdb);
    static const std::pair<const char*, const char*> kTools[] = {
        {"optiscaler", "OptiScaler"},   {"reshade", "ReShade"},         {"asi-loader", "ASI loader"}, {"asiloader", "ASI loader"},
        {"dlssenabler", "DLSS Enabler"}, {"mfgunlocker", "MFG Unlocker"}, {"specialk", "Special K"},   {"dlssg_to_fsr3", "DLSSG to FSR3"},
        {"nukem", "DLSSG to FSR3"},      {"uniscaler", "Uniscaler"},     {"lossless", "Lossless Scaling"}};
    for (const auto& [key, tool] : kTools) {
        if (p.find(key) != std::string::npos) {
            return tool;
        }
    }
    return {};
}

std::string LoadedTools(const std::vector<std::string>& own_proxies) {
#ifdef _WIN32
    static const std::pair<const char*, const char*> kHooks[] = {
        {"graphics-hook64.dll", "OBS"},
        {"rtsshooks64.dll", "RTSS"},
        {"gameoverlayrenderer64.dll", "Steam overlay"},
        {"discordhook64.dll", "Discord overlay"},
        {"medal-hook64.dll", "Medal"},
        {"ow-graphics-hook64.dll", "Overwolf"},
        {"owclient.dll", "Overwolf"},
        {"specialk64.dll", "Special K"},
        {"reshade64.dll", "ReShade"},
        {"bdcam64.dll", "Bandicam"},
        {"bdcamvk64.dll", "Bandicam"},
        {"action_x64.dll", "Mirillis Action"},
        {"mirillisactionvulkanlayer.dll", "Mirillis Action"},
        {"aswhook.dll", "Avast"},
        {"eosovh-win64-shipping.dll", "Epic overlay"},
        {"lossless.dll", "Lossless Scaling"},
    };
    static const char* kProxyNames[] = {"dxgi.dll",  "version.dll", "winmm.dll",     "d3d12.dll",     "d3d11.dll",   "dinput8.dll",
                                        "winhttp.dll", "dsound.dll", "xinput1_3.dll", "xinput1_4.dll", "wininet.dll", "nvngx.dll"};
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::string exe_dir = Lower(Narrow(exe));
    exe_dir = exe_dir.substr(0, exe_dir.find_last_of("\\/") + 1);
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::vector<std::string> found;
    MODULEENTRY32W m{};
    m.dwSize = sizeof(m);
    for (BOOL ok = Module32FirstW(snapshot, &m); ok; ok = Module32NextW(snapshot, &m)) {
        const std::string name = Lower(Narrow(m.szModule));
        const std::string path = Lower(Narrow(m.szExePath));
        for (const auto& [module, tool] : kHooks) {
            if (name == module) {
                found.push_back(std::format("{} ({})", tool, Narrow(m.szModule)));
            }
        }
        const bool proxy_name = std::any_of(std::begin(kProxyNames), std::end(kProxyNames), [&](const char* n) { return name == n; });
        const bool own = std::any_of(own_proxies.begin(), own_proxies.end(), [&](const std::string& n) { return Lower(n) == name; });
        if (proxy_name && !own && path.substr(0, path.find_last_of("\\/") + 1) == exe_dir) {
            const std::string tool = ProxyToolForPdb(PdbPath(m.modBaseAddr, m.modBaseSize));
            const std::string size = m.modBaseSize >= (1u << 20) ? std::format("{} MB", (m.modBaseSize + (1u << 19)) >> 20)
                                                                 : std::format("{} KB", (m.modBaseSize + 512u) >> 10);
            found.push_back(std::format("{} ({}, {})", tool.empty() ? "an unknown proxy" : tool, Narrow(m.szModule), size));
        }
    }
    CloseHandle(snapshot);
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    std::string out;
    for (const std::string& f : found) {
        out += out.empty() ? f : ", " + f;
    }
    return out;
#else
    (void)own_proxies;
    return {};
#endif
}

std::string ToolNames(const std::string& loaded_tools) {
    std::string out;
    size_t at = 0;
    while (at < loaded_tools.size()) {
        const size_t paren = loaded_tools.find(" (", at);
        const size_t close = paren == std::string::npos ? std::string::npos : loaded_tools.find(')', paren);
        const std::string name = loaded_tools.substr(at, (paren == std::string::npos ? loaded_tools.size() : paren) - at);
        if (out.find(name) == std::string::npos) {
            out += out.empty() ? name : ", " + name;
        }
        if (close == std::string::npos) {
            break;
        }
        at = close + 1;
        while (at < loaded_tools.size() && (loaded_tools[at] == ',' || loaded_tools[at] == ' ')) {
            ++at;
        }
    }
    return out;
}

}
