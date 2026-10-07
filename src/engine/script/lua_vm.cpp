#include "engine/script/lua_vm.h"

#include <string>

#include "engine/core/log.h"

namespace pt {
namespace {

int Traceback(lua_State* L) {
    const char* message = lua_tostring(L, 1);
    lua_getglobal(L, "debug");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return 1;
    }
    lua_getfield(L, -1, "traceback");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return 1;
    }
    lua_pushstring(L, message ? message : "(error object is not a string)");
    lua_pushinteger(L, 2);
    lua_call(L, 2, 1);
    return 1;
}

void GetOrCreateTable(lua_State* L, const char* name) {
    lua_getglobal(L, name);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, name);
    }
}

}

LuaVm::LuaVm() {
    state_ = luaL_newstate();
    luaL_openlibs(state_);
}

LuaVm::~LuaVm() {
    if (state_) {
        lua_close(state_);
    }
}

std::string LuaVm::ToString(lua_State* L, int index) {
    size_t length = 0;
    const char* text = lua_tolstring(L, index, &length);
    if (text) {
        return std::string(text, length);
    }
    if (lua_isboolean(L, index)) {
        return lua_toboolean(L, index) ? "true" : "false";
    }
    if (lua_isnil(L, index)) {
        return "nil";
    }
    return std::string(luaL_typename(L, index));
}

bool LuaVm::ProtectedCall(int arg_count, int result_count) {
    const int base = lua_gettop(state_) - arg_count;
    lua_pushcfunction(state_, Traceback);
    lua_insert(state_, base);
    const int status = lua_pcall(state_, arg_count, result_count, base);
    lua_remove(state_, base);
    if (status != 0) {
        LogError("lua: {}", ToString(state_, -1));
        lua_pop(state_, 1);
        return false;
    }
    return true;
}

bool LuaVm::RunChunk(std::string_view chunk_name, std::span<const uint8_t> code) {
    const std::string name = "@" + std::string(chunk_name);
    if (luaL_loadbuffer(state_, reinterpret_cast<const char*>(code.data()), code.size(), name.c_str()) != 0) {
        LogError("lua: load {}: {}", chunk_name, ToString(state_, -1));
        lua_pop(state_, 1);
        return false;
    }
    return ProtectedCall(0, 0);
}

bool LuaVm::HasTableFunction(std::string_view table, std::string_view function) {
    lua_getglobal(state_, std::string(table).c_str());
    bool found = false;
    if (lua_istable(state_, -1)) {
        lua_getfield(state_, -1, std::string(function).c_str());
        found = lua_isfunction(state_, -1);
        lua_pop(state_, 1);
    }
    lua_pop(state_, 1);
    return found;
}

bool LuaVm::CallTableFunction(std::string_view table, std::string_view function, int arg_count, int result_count,
                              const std::function<void(lua_State*)>& push_args, int* first_result) {
    lua_getglobal(state_, std::string(table).c_str());
    if (!lua_istable(state_, -1)) {
        lua_pop(state_, 1);
        return false;
    }
    lua_getfield(state_, -1, std::string(function).c_str());
    lua_remove(state_, -2);
    if (!lua_isfunction(state_, -1)) {
        lua_pop(state_, 1);
        return false;
    }
    if (push_args) {
        push_args(state_);
    }
    if (!ProtectedCall(arg_count, result_count)) {
        return false;
    }
    if (first_result) {
        *first_result = lua_gettop(state_) - result_count + 1;
    }
    return true;
}

void LuaVm::RegisterModule(const char* module, const luaL_Reg* functions) {
    GetOrCreateTable(state_, module);
    for (const luaL_Reg* f = functions; f->name; ++f) {
        lua_pushcfunction(state_, f->func);
        lua_setfield(state_, -2, f->name);
    }
    lua_pop(state_, 1);
}

void LuaVm::SetModuleNumber(const char* module, const char* name, double value) {
    GetOrCreateTable(state_, module);
    lua_pushnumber(state_, value);
    lua_setfield(state_, -2, name);
    lua_pop(state_, 1);
}

void LuaVm::SetModuleString(const char* module, const char* name, const char* value) {
    GetOrCreateTable(state_, module);
    lua_pushstring(state_, value);
    lua_setfield(state_, -2, name);
    lua_pop(state_, 1);
}

}
