#include "engine/script/mod_lua.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace pt {
namespace {

// what a mod's environment gets of the base library: no load, loadstring, dofile, require, getfenv/setfenv, io or debug, so
// a script reaches nothing beyond its own tables and the Mod API
constexpr const char* kBaseFunctions[] = {"assert", "error",  "ipairs", "next",     "pairs",    "pcall",  "rawequal", "rawget",
                                          "rawset", "select", "tonumber", "tostring", "type", "unpack", "xpcall", "setmetatable"};
constexpr const char* kLibraries[] = {"string", "table", "math", "coroutine"};
constexpr const char* kOsFunctions[] = {"clock", "time", "date", "difftime"};
constexpr const char* kEventNames[] = {"FloorEnter", "StepChange", "Tick", "Message"};
constexpr int kHookEvery = 1000;

void OpenLibrary(lua_State* L, lua_CFunction open, const char* name) {
    lua_pushcfunction(L, open);
    lua_pushstring(L, name);
    lua_call(L, 1, 0);
}

// a shallow copy of the global table `name` into the table at the top of the stack, so one mod's changes stay its own
void CopyLibrary(lua_State* L, const char* name, const char* const* only = nullptr, size_t only_count = 0) {
    lua_newtable(L);
    lua_getglobal(L, name);
    if (lua_istable(L, -1)) {
        if (only) {
            for (size_t i = 0; i < only_count; ++i) {
                lua_getfield(L, -1, only[i]);
                lua_setfield(L, -3, only[i]);
            }
        } else {
            lua_pushnil(L);
            while (lua_next(L, -2) != 0) {
                lua_pushvalue(L, -2);
                lua_insert(L, -2);
                lua_settable(L, -5);
            }
        }
    }
    lua_pop(L, 1);
    lua_setfield(L, -2, name);
}

}

ModLua::ModLua(LogSink sink) : sink_(std::move(sink)) {
    L_ = lua_newstate(&ModLua::Allocate, this);
    if (!L_) {
        return;
    }
    OpenLibrary(L_, luaopen_base, "");
    OpenLibrary(L_, luaopen_table, LUA_TABLIBNAME);
    OpenLibrary(L_, luaopen_string, LUA_STRLIBNAME);
    OpenLibrary(L_, luaopen_math, LUA_MATHLIBNAME);
    OpenLibrary(L_, luaopen_os, LUA_OSLIBNAME);
    lua_sethook(L_, &ModLua::CountHook, LUA_MASKCOUNT, kHookEvery);
}

ModLua::~ModLua() {
    if (L_) {
        lua_close(L_);
    }
}

const char* ModLua::EventName(Event event) {
    const int i = static_cast<int>(event);
    return i >= 0 && i < static_cast<int>(Event::Count) ? kEventNames[i] : "";
}

ModLua& ModLua::Self(lua_State* L) {
    void* ud = nullptr;
    lua_getallocf(L, &ud);
    return *static_cast<ModLua*>(ud);
}

void* ModLua::Allocate(void* ud, void* ptr, size_t old_size, size_t new_size) {
    auto* self = static_cast<ModLua*>(ud);
    if (new_size == 0) {
        std::free(ptr);
        self->memory_ -= old_size;
        return nullptr;
    }
    // growth past the limit fails as a Lua memory error inside the protected call; shrinking never fails
    if (new_size > old_size && self->memory_ + (new_size - old_size) > kMemoryLimit) {
        return nullptr;
    }
    void* out = std::realloc(ptr, new_size);
    if (out) {
        self->memory_ = self->memory_ - old_size + new_size;
    }
    return out;
}

void ModLua::CountHook(lua_State* L, lua_Debug*) {
    ModLua& self = Self(L);
    self.instructions_ += kHookEvery;
    if (self.instructions_ > kInstructionBudget) {
        luaL_error(L, "instruction budget exceeded (an endless loop?)");
    }
}

void ModLua::Log(size_t mod, bool error, std::string_view text) {
    ModState& state = mods_[mod];
    if (!sink_ || (!error && state.log_lines >= kLogLinesPerMod)) {
        return;
    }
    std::string line = state.name + ": " + std::string(text);
    if (!error && ++state.log_lines == kLogLinesPerMod) {
        line += " (further Mod.Log output of this mod is not written)";
    }
    sink_(error, line);
}

// The C functions below raise Lua errors (a longjmp) only while no C++ object with a destructor is alive in them
int ModLua::LuaLog(lua_State* L) {
    const int count = lua_gettop(L);
    for (int i = 1; i <= count; ++i) {
        lua_getglobal(L, "tostring");
        lua_pushvalue(L, i);
        lua_call(L, 1, 1);
        if (!lua_isstring(L, -1)) {
            lua_pop(L, 1);
            lua_pushliteral(L, "?");
        }
        lua_replace(L, i);
    }
    for (int i = count; i > 1; --i) {
        lua_pushliteral(L, " ");
        lua_insert(L, i);
    }
    lua_concat(L, lua_gettop(L));
    size_t length = 0;
    const char* text = lua_tolstring(L, -1, &length);
    ModLua& self = Self(L);
    const size_t mod = static_cast<size_t>(lua_tointeger(L, lua_upvalueindex(1)));
    if (mod < self.mods_.size()) {
        self.Log(mod, false, std::string_view(text ? text : "", text ? length : 0));
    }
    return 0;
}

int ModLua::LuaOn(lua_State* L) {
    ModLua& self = Self(L);
    const size_t mod = static_cast<size_t>(lua_tointeger(L, lua_upvalueindex(1)));
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    int event = -1;
    for (int i = 0; i < static_cast<int>(Event::Count); ++i) {
        if (std::strcmp(name, kEventNames[i]) == 0) {
            event = i;
        }
    }
    if (event < 0) {
        return luaL_error(L, "Mod.On: unknown event '%s' (FloorEnter, StepChange, Tick, Message)", name);
    }
    if (mod >= self.mods_.size() || self.mods_[mod].failed) {
        return 0;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, self.mods_[mod].handlers);
    lua_getfield(L, -1, name);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, name);
    }
    lua_pushvalue(L, 2);
    lua_rawseti(L, -2, static_cast<int>(lua_objlen(L, -2)) + 1);
    lua_pop(L, 2);
    self.mods_[mod].hooks[event] = true;
    return 0;
}

int ModLua::LuaFloor(lua_State* L) {
    ModLua& self = Self(L);
    char floor[64] = {};
    if (self.queries_.floor) {
        const std::string name = self.queries_.floor();
        std::snprintf(floor, sizeof(floor), "%s", name.c_str());
    }
    lua_pushstring(L, floor);
    return 1;
}

int ModLua::LuaLoop(lua_State* L) {
    ModLua& self = Self(L);
    lua_pushinteger(L, self.queries_.loop ? self.queries_.loop() : 0);
    return 1;
}

int ModLua::LuaStep(lua_State* L) {
    ModLua& self = Self(L);
    lua_pushinteger(L, self.queries_.step ? self.queries_.step() : 0);
    return 1;
}

// getmetatable of tables only: the strings' shared metatable would hand out the real string library
int ModLua::LuaGetMetatable(lua_State* L) {
    if (!lua_istable(L, 1) || !lua_getmetatable(L, 1)) {
        lua_pushnil(L);
        return 1;
    }
    lua_getfield(L, -1, "__metatable");
    if (!lua_isnil(L, -1)) {
        return 1;
    }
    lua_pop(L, 1);
    return 1;
}

void ModLua::BuildEnvironment(size_t mod) {
    lua_State* L = L_;
    lua_newtable(L);
    for (const char* name : kBaseFunctions) {
        lua_getglobal(L, name);
        lua_setfield(L, -2, name);
    }
    lua_pushcfunction(L, &ModLua::LuaGetMetatable);
    lua_setfield(L, -2, "getmetatable");
    lua_pushstring(L, LUA_VERSION);
    lua_setfield(L, -2, "_VERSION");
    for (const char* name : kLibraries) {
        CopyLibrary(L, name);
    }
    CopyLibrary(L, "os", kOsFunctions, std::size(kOsFunctions));

    lua_newtable(L);
    lua_pushinteger(L, static_cast<lua_Integer>(mod));
    lua_pushcclosure(L, &ModLua::LuaLog, 1);
    lua_pushvalue(L, -1);
    lua_setfield(L, -4, "print");
    lua_setfield(L, -2, "Log");
    lua_pushinteger(L, static_cast<lua_Integer>(mod));
    lua_pushcclosure(L, &ModLua::LuaOn, 1);
    lua_setfield(L, -2, "On");
    lua_pushcfunction(L, &ModLua::LuaFloor);
    lua_setfield(L, -2, "Floor");
    lua_pushcfunction(L, &ModLua::LuaLoop);
    lua_setfield(L, -2, "Loop");
    lua_pushcfunction(L, &ModLua::LuaStep);
    lua_setfield(L, -2, "Step");
    lua_pushlstring(L, mods_[mod].name.data(), mods_[mod].name.size());
    lua_setfield(L, -2, "Name");
    lua_setfield(L, -2, "Mod");

    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "_G");
}

void ModLua::Fail(size_t mod, std::string_view error) {
    ModState& state = mods_[mod];
    if (state.failed) {
        return;
    }
    state.failed = true;
    for (bool& hook : state.hooks) {
        hook = false;
    }
    Log(mod, true, std::string(error) + "; this mod's hooks are off until the next start");
    if (state.handlers != LUA_NOREF) {
        luaL_unref(L_, LUA_REGISTRYINDEX, state.handlers);
        state.handlers = LUA_NOREF;
    }
    // what the failed mod held (all of it after a memory error) goes now: Lua 5.1 collects nothing when an allocation fails
    lua_gc(L_, LUA_GCCOLLECT, 0);
}

bool ModLua::Call(size_t mod, int arg_count) {
    instructions_ = 0;
    const int status = lua_pcall(L_, arg_count, 0, 0);
    if (status == 0) {
        return true;
    }
    std::string error = status == LUA_ERRMEM ? "out of memory (the mods' limit is 64 MB)" : "error";
    if (status != LUA_ERRMEM) {
        size_t length = 0;
        const char* text = lua_tolstring(L_, -1, &length);
        error = text ? std::string(text, length) : std::string("error object is not a string");
    }
    lua_pop(L_, 1);
    Fail(mod, error);
    return false;
}

bool ModLua::AddMod(std::string name, std::string_view chunk_name, std::span<const uint8_t> code) {
    const size_t mod = mods_.size();
    mods_.push_back(ModState{std::move(name)});
    if (!L_) {
        Fail(mod, "the mods' Lua state could not be created");
        return false;
    }
    // precompiled chunks skip the parser's checks (Lua 5.1 has no bytecode verifier): source text only
    if (code.size() >= 4 && std::memcmp(code.data(), LUA_SIGNATURE, 4) == 0) {
        Fail(mod, std::string(chunk_name) + ": precompiled Lua is not loaded, ship the source");
        return false;
    }
    const std::string chunk = "@" + std::string(chunk_name);
    if (luaL_loadbuffer(L_, reinterpret_cast<const char*>(code.data()), code.size(), chunk.c_str()) != 0) {
        size_t length = 0;
        const char* text = lua_tolstring(L_, -1, &length);
        const std::string error = text ? std::string(text, length) : std::string("cannot be loaded");
        lua_pop(L_, 1);
        Fail(mod, error);
        return false;
    }
    lua_newtable(L_);
    mods_[mod].handlers = luaL_ref(L_, LUA_REGISTRYINDEX);
    BuildEnvironment(mod);
    lua_setfenv(L_, -2);
    if (!Call(mod, 0)) {
        return false;
    }
    lua_gc(L_, LUA_GCCOLLECT, 0);
    return true;
}

bool ModLua::Wants(Event event) const {
    for (const ModState& state : mods_) {
        if (state.hooks[static_cast<int>(event)]) {
            return true;
        }
    }
    return false;
}

void ModLua::Emit(Event event, const std::function<int(lua_State*)>& push_args) {
    if (!L_ || dispatching_ || !Wants(event)) {
        return;
    }
    dispatching_ = true;
    const int top = lua_gettop(L_);
    for (size_t mod = 0; mod < mods_.size(); ++mod) {
        if (!mods_[mod].hooks[static_cast<int>(event)]) {
            continue;
        }
        lua_rawgeti(L_, LUA_REGISTRYINDEX, mods_[mod].handlers);
        lua_getfield(L_, -1, EventName(event));
        const int list = lua_gettop(L_);
        const int count = lua_istable(L_, list) ? static_cast<int>(lua_objlen(L_, list)) : 0;
        for (int i = 1; i <= count && !mods_[mod].failed; ++i) {
            lua_rawgeti(L_, list, i);
            if (!lua_isfunction(L_, -1)) {
                lua_pop(L_, 1);
                continue;
            }
            const int args = push_args(L_);
            Call(mod, args);
        }
        lua_settop(L_, top);
    }
    dispatching_ = false;
}

void ModLua::FloorEnter(std::string_view floor, int loop) {
    Emit(Event::FloorEnter, [&](lua_State* L) {
        lua_pushlstring(L, floor.data(), floor.size());
        lua_pushinteger(L, loop);
        return 2;
    });
}

void ModLua::StepChange(int old_step, int new_step) {
    Emit(Event::StepChange, [&](lua_State* L) {
        lua_pushinteger(L, old_step);
        lua_pushinteger(L, new_step);
        return 2;
    });
}

void ModLua::Tick(float dt) {
    Emit(Event::Tick, [&](lua_State* L) {
        lua_pushnumber(L, dt);
        return 1;
    });
}

void ModLua::Message(std::string_view name, std::string_view sender) {
    Emit(Event::Message, [&](lua_State* L) {
        lua_pushlstring(L, name.data(), name.size());
        lua_pushlstring(L, sender.data(), sender.size());
        return 2;
    });
}

}
