#pragma once

#include <string>
#include <vector>

#include "engine/script/lua_vm.h"

namespace pt::game {

struct ScriptCall {
    std::string module;
    std::string function;
    std::vector<std::string> args;
};

void RegisterStubApi(LuaVm& vm);
const std::vector<ScriptCall>& RecentStubCalls();
void ClearStubCalls();

}
