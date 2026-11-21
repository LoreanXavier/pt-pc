#ifdef NDEBUG
#undef NDEBUG
#endif
#include "engine/platform/third_party.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace pt::third_party;

int main() {
    assert(ProxyToolForPdb("D:\\a\\OptiScaler\\x64\\Release\\OptiScaler.pdb") == "OptiScaler");
    assert(ProxyToolForPdb("C:\\dev\\Ultimate-ASI-Loader\\bin\\x64\\Release\\dinput8.pdb") == "ASI loader");
    assert(ProxyToolForPdb("D:\\b\\DLSSEnabler - 4.9.0.15 - released\\x64\\Release\\DLSSEnabler.pdb") == "DLSS Enabler");
    assert(ProxyToolForPdb("E:\\src\\release-20261020\\native-release\\Release\\MFGUnlocker.pdb") == "MFG Unlocker");
    assert(ProxyToolForPdb("C:\\reshade\\bin\\x64\\Release App\\ReShade64.pdb") == "ReShade");
    assert(ProxyToolForPdb("dinput8.pdb").empty());
    assert(ProxyToolForPdb("").empty());

    assert(ToolNames("OBS (graphics-hook64.dll), OptiScaler (dxgi.dll, 26 MB), RTSS (RTSSHooks64.dll)") == "OBS, OptiScaler, RTSS");
    assert(ToolNames("Overwolf (owclient.dll), Overwolf (ow-graphics-hook64.dll)") == "Overwolf");
    assert(ToolNames("").empty());

#ifdef _WIN32
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const std::filesystem::path dir = std::filesystem::path(self).parent_path();
    wchar_t system[MAX_PATH] = {};
    GetSystemDirectoryW(system, MAX_PATH);
    const std::filesystem::path proxy = dir / "dinput8.dll";
    std::error_code ec;
    std::filesystem::copy_file(std::filesystem::path(system) / "dinput8.dll", proxy, std::filesystem::copy_options::overwrite_existing, ec);
    assert(!ec);
    assert(LoadedTools().find("dinput8.dll") == std::string::npos);
    HMODULE module = LoadLibraryW(proxy.c_str());
    assert(module);
    const std::string tools = LoadedTools();
    std::printf("loaded tools: %s\n", tools.c_str());
    assert(tools.find("an unknown proxy (dinput8.dll, ") != std::string::npos);
    assert(LoadedTools({"dinput8.dll"}).find("dinput8.dll") == std::string::npos);
    assert(LoadedTools({"DINPUT8.DLL"}).find("dinput8.dll") == std::string::npos);
    FreeLibrary(module);
    std::filesystem::remove(proxy, ec);
#endif
    std::printf("third_party_test: ok\n");
    return 0;
}
