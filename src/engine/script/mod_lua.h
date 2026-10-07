#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;
struct lua_Debug;

namespace pt {

class ModLua {
public:
    enum class Event { FloorEnter, StepChange, Tick, Message, Count };

    struct Queries {
        std::function<std::string()> floor;
        std::function<int()> loop;
        std::function<int()> step;
    };
    using LogSink = std::function<void(bool error, std::string_view text)>;

    static constexpr uint64_t kInstructionBudget = 20'000'000;
    static constexpr size_t kMemoryLimit = 64u << 20;
    static constexpr int kLogLinesPerMod = 500;

    explicit ModLua(LogSink sink);
    ~ModLua();
    ModLua(const ModLua&) = delete;
    ModLua& operator=(const ModLua&) = delete;

    void SetQueries(Queries queries) { queries_ = std::move(queries); }
    bool AddMod(std::string name, std::string_view chunk_name, std::span<const uint8_t> code);

    bool Wants(Event event) const;
    void FloorEnter(std::string_view floor, int loop);
    void StepChange(int old_step, int new_step);
    void Tick(float dt);
    void Message(std::string_view name, std::string_view sender);

    size_t ModCount() const { return mods_.size(); }
    bool Failed(size_t mod) const { return mod < mods_.size() && mods_[mod].failed; }
    const std::string& ModName(size_t mod) const { return mods_[mod].name; }

    static const char* EventName(Event event);

private:
    struct ModState {
        std::string name;
        int handlers = -2;
        bool failed = false;
        int log_lines = 0;
        bool hooks[static_cast<int>(Event::Count)] = {};
    };

    static void* Allocate(void* ud, void* ptr, size_t old_size, size_t new_size);
    static void CountHook(lua_State* L, lua_Debug* ar);
    static int LuaLog(lua_State* L);
    static int LuaOn(lua_State* L);
    static int LuaFloor(lua_State* L);
    static int LuaLoop(lua_State* L);
    static int LuaStep(lua_State* L);
    static int LuaGetMetatable(lua_State* L);
    static ModLua& Self(lua_State* L);

    void BuildEnvironment(size_t mod);
    bool Call(size_t mod, int arg_count);
    void Fail(size_t mod, std::string_view error);
    void Emit(Event event, const std::function<int(lua_State*)>& push_args);
    void Log(size_t mod, bool error, std::string_view text);

    LogSink sink_;
    Queries queries_;
    lua_State* L_ = nullptr;
    std::vector<ModState> mods_;
    size_t memory_ = 0;
    uint64_t instructions_ = 0;
    bool dispatching_ = false;
};

}
