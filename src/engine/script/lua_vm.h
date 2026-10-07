#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace pt {

class LuaVm {
public:
    LuaVm();
    ~LuaVm();
    LuaVm(const LuaVm&) = delete;
    LuaVm& operator=(const LuaVm&) = delete;

    lua_State* State() const { return state_; }

    bool RunChunk(std::string_view chunk_name, std::span<const uint8_t> code);
    bool CallTableFunction(std::string_view table, std::string_view function, int arg_count, int result_count,
                           const std::function<void(lua_State*)>& push_args, int* first_result = nullptr);
    bool HasTableFunction(std::string_view table, std::string_view function);

    void RegisterModule(const char* module, const luaL_Reg* functions);
    void SetModuleNumber(const char* module, const char* name, double value);
    void SetModuleString(const char* module, const char* name, const char* value);

    static std::string ToString(lua_State* L, int index);

private:
    bool ProtectedCall(int arg_count, int result_count);

    lua_State* state_ = nullptr;
};

}
