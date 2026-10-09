// The mod override index, its priority rules, mod.json and the mod script sandbox (docs/modding.md). Needs no game files.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "engine/fs/fpk.h"
#include "engine/fs/mods.h"
#include "engine/script/mod_lua.h"

namespace {

int failures = 0;
bool verbose = false;

void Check(bool ok, const char* label) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    failures += ok ? 0 : 1;
}

void WriteText(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

std::string ReadText(std::string_view asset) {
    const auto data = pt::mods::ReadOverride(asset);
    return data ? std::string(data->begin(), data->end()) : std::string("<none>");
}

pt::mods::Mod MakeMod(std::string folder, int priority, std::vector<std::string> files, bool enabled = true) {
    pt::mods::Mod mod;
    mod.folder = std::move(folder);
    mod.root = std::filesystem::path("/mods") / mod.folder;
    mod.manifest.priority = priority;
    mod.enabled = enabled;
    mod.files = std::move(files);
    return mod;
}

std::vector<uint8_t> Bytes(std::string_view text) {
    return std::vector<uint8_t>(text.begin(), text.end());
}

void TestKeys() {
    using pt::mods::AssetKey;
    Check(AssetKey("/Assets/sh/level/A.fpk") == "sh/level/a.fpk", "key: /Assets/ prefix and case");
    Check(AssetKey("as/sh/level/a.fpk") == "sh/level/a.fpk", "key: archive as/ prefix");
    Check(AssetKey("\\Assets\\sh\\x.lua") == "sh/x.lua", "key: backslashes");
    Check(AssetKey("/Assets//sh/./x.lua") == "sh/x.lua", "key: doubled slash and dot");
    Check(AssetKey("/Fox/x.lua").empty(), "key: outside /Assets/ is not overridable");
    Check(AssetKey("").empty(), "key: empty");
}

void TestManifest() {
    pt::mods::Manifest m;
    std::string error;
    const bool ok = pt::mods::ParseManifest(
        "\xEF\xBB\xBF{\n // comment\n \"name\": \"Hall \\\"Mod\\\" \\u00e9\", \"version\": \"1.2\", \"author\": \"me\",\n"
        " \"Priority\": 7, \"enabled\": false, \"extra\": {\"a\": [1, 2, {\"b\": \"}\"}]}, /* trailing */ }",
        m, &error);
    Check(ok, "manifest: tolerant parse");
    Check(m.name == "Hall \"Mod\" \xC3\xA9", "manifest: escapes and \\u");
    Check(m.version == "1.2" && m.author == "me", "manifest: strings");
    Check(m.priority == 7, "manifest: priority (key case ignored)");
    Check(!m.enabled, "manifest: enabled false");
    pt::mods::Manifest d;
    Check(pt::mods::ParseManifest("{}", d) && d.enabled && d.priority == 0, "manifest: defaults");
    Check(pt::mods::ParseManifest("{\"version\": 2, \"priority\": \"-3\"}", d) && d.version == "2" && d.priority == -3,
          "manifest: number version, string priority");
    pt::mods::Manifest bad;
    bad.name = "kept";
    Check(!pt::mods::ParseManifest("[1, 2]", bad, &error) && bad.name == "kept", "manifest: not an object fails, output untouched");
    Check(!pt::mods::ParseManifest("{\"name\": \"x\"", bad, &error), "manifest: unterminated object fails");
}

void TestPriority() {
    std::vector<pt::mods::Mod> mods = {
        MakeMod("b_low", 0, {"sh/a.lua", "sh/only_b.lua"}),
        MakeMod("a_high", 5, {"sh/a.lua", "SH/Tex/Wall.png"}),
        MakeMod("z_tie", 0, {"sh/only_b.lua", "sh/tie.lua"}),
        MakeMod("c_tie", 0, {"sh/tie.lua"}),
        MakeMod("off", 100, {"sh/a.lua"}, false),
    };
    pt::mods::OverrideIndex index;
    index.Build(mods);
    auto winner = [&](const char* path) {
        const auto* hit = index.Find(path);
        return hit ? mods[hit->mod].folder : std::string("<none>");
    };
    Check(winner("/Assets/sh/a.lua") == "a_high", "priority: higher priority wins");
    Check(winner("/Assets/sh/only_b.lua") == "z_tie", "priority: tie goes to the folder name sorting last");
    Check(winner("/Assets/sh/tie.lua") == "z_tie", "priority: tie independent of input order");
    Check(winner("/Assets/sh/tex/wall.png") == "a_high", "index: lookups ignore case");
    Check(winner("/Assets/sh/missing.lua") == "<none>", "index: unknown file");
    Check(index.Size() == 4, "index: disabled mod contributes nothing");
    std::vector<pt::mods::Mod> sorted = mods;
    pt::mods::SortByPriority(sorted);
    Check(sorted[0].folder == "off" && sorted[1].folder == "a_high" && sorted[2].folder == "z_tie" && sorted[3].folder == "c_tie" &&
              sorted[4].folder == "b_low",
          "priority: load order");
    // the same result whatever order Build sees the mods in
    std::vector<pt::mods::Mod> reversed(mods.rbegin(), mods.rend());
    pt::mods::OverrideIndex again;
    again.Build(reversed);
    Check(reversed[again.Find("/Assets/sh/a.lua")->mod].folder == "a_high" &&
              reversed[again.Find("/Assets/sh/tie.lua")->mod].folder == "z_tie",
          "priority: Build order independent");
    pt::mods::OverrideIndex empty;
    empty.Build({});
    Check(empty.Empty() && !empty.Find("/Assets/sh/a.lua"), "index: empty");
}

// a Fox package of one plain entry, the layout FoxPackage::Load reads (48-byte header, 48-byte entries, then the strings)
std::vector<uint8_t> OnePackage(const std::string& entry_path, const std::string& content) {
    std::vector<uint8_t> data(96, 0);
    std::memcpy(data.data(), "foxfpk", 6);
    data[6] = 'd';
    std::memcpy(data.data() + 7, "ps4", 3);
    auto put = [&](size_t at, uint32_t value) { std::memcpy(data.data() + at, &value, 4); };
    put(0x24, 1);
    const uint32_t name_at = static_cast<uint32_t>(data.size());
    data.insert(data.end(), entry_path.begin(), entry_path.end());
    const uint32_t content_at = static_cast<uint32_t>(data.size());
    data.insert(data.end(), content.begin(), content.end());
    put(48, content_at);
    put(48 + 8, static_cast<uint32_t>(content.size()));
    put(48 + 16, name_at);
    put(48 + 24, static_cast<uint32_t>(entry_path.size()));
    return data;
}

void TestPackageEntry(const std::filesystem::path& root) {
    pt::FoxPackage package;
    Check(package.Load("test.fpkd", OnePackage("/Assets/sh/level/f000/x.lua", "original")), "package: test package loads");
    const auto* entry = package.Find("/Assets/sh/level/f000/x.lua");
    auto text = [&] {
        const auto bytes = package.Read(*entry);
        return std::string(bytes.begin(), bytes.end());
    };
    Check(entry && text() == "original", "package: entry read without mods");
    WriteText(root / "m" / "Assets" / "sh" / "level" / "f000" / "X.lua", "modded");
    auto set = pt::mods::Load(root, {});
    pt::mods::SetActive(set.get());
    Check(entry && text() == "modded", "package: a mod's file replaces the entry read from the package");
    pt::mods::SetActive(nullptr);
    Check(entry && text() == "original", "package: original again without mods");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

void TestDiscoverAndRead(const std::filesystem::path& root) {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    Check(pt::mods::Discover(root).empty(), "discover: no mods folder");
    Check(pt::mods::ReadOverride("/Assets/sh/a.lua") == std::nullopt, "read: nothing active, nothing read");
    WriteText(root / "base" / "mod.json", "{\"name\": \"Base\", \"version\": \"1.0\", \"priority\": 1}");
    WriteText(root / "base" / "Assets" / "sh" / "a.lua", "base a");
    WriteText(root / "base" / "Assets" / "sh" / "b.lua", "base b");
    WriteText(root / "patch" / "mod.json", "{\"priority\": 1}");
    WriteText(root / "patch" / "assets" / "sh" / "b.lua", "patch b");
    WriteText(root / "patch" / "init.lua", "Mod.Log('hi')");
    WriteText(root / "broken" / "mod.json", "not json");
    WriteText(root / "broken" / "Assets" / "sh" / "a.lua", "broken a");
    WriteText(root / "off" / "mod.json", "{\"enabled\": false, \"priority\": 99}");
    WriteText(root / "off" / "Assets" / "sh" / "a.lua", "off a");
    WriteText(root / ".hidden" / "Assets" / "sh" / "a.lua", "hidden a");
    WriteText(root / "loose.txt", "not a mod");
    std::vector<std::string> warnings;
    auto set = pt::mods::Load(root, {{"broken", false}}, &warnings);
    Check(set->mods.size() == 4, "discover: four folders, hidden and loose files skipped");
    Check(warnings.size() == 1 && warnings[0].starts_with("broken/mod.json"), "discover: bad mod.json warns");
    const pt::mods::Mod* base = nullptr;
    const pt::mods::Mod* patch = nullptr;
    for (const auto& mod : set->mods) {
        if (mod.folder == "base") base = &mod;
        if (mod.folder == "patch") patch = &mod;
    }
    Check(base && base->Name() == "Base" && base->files.size() == 2 && !base->has_script, "discover: manifest and files");
    Check(patch && patch->Name() == "patch" && patch->has_script && patch->files.size() == 1, "discover: lower-case assets, init.lua");
    pt::mods::SetActive(set.get());
    Check(pt::mods::Active() == set.get(), "active: set");
    Check(ReadText("/Assets/sh/a.lua") == "base a", "read: disabled-by-manifest and disabled-by-ini mods lose");
    Check(ReadText("as/sh/b.lua") == "patch b", "read: tie on priority, later folder name wins");
    Check(ReadText("/Assets/sh/c.lua") == "<none>", "read: not in any mod");
    auto none = pt::mods::Load(root / "nothing", {});
    pt::mods::SetActive(none.get());
    Check(pt::mods::Active() == nullptr, "active: a set without files stays inactive");
    pt::mods::SetActive(nullptr);
    std::filesystem::remove_all(root, ec);
}

void TestScripts() {
    std::vector<std::string> lines;
    pt::ModLua lua([&](bool error, std::string_view text) { if (verbose) std::printf("  log: %.*s\n", int(text.size()), text.data()); lines.push_back(std::string(error ? "E " : "I ") + std::string(text)); });
    int loop = 3;
    lua.SetQueries({[] { return std::string("f040"); }, [&] { return loop; }, [] { return 14; }});
    Check(!lua.Wants(pt::ModLua::Event::Tick), "lua: no mods, no hooks");
    const bool good = lua.AddMod("good", "good/init.lua", Bytes(R"(
        local count = 0
        Mod.On("FloorEnter", function(floor, n) Mod.Log("enter", floor, n, Mod.Floor(), Mod.Loop(), Mod.Step()) end)
        Mod.On("StepChange", function(a, b) Mod.Log("step", a, b) end)
        Mod.On("Tick", function(dt) count = count + 1 end)
        Mod.On("Message", function(name, sender) Mod.Log("message", name, sender, count) end)
        print("loaded", io == nil, loadstring == nil, getfenv == nil, require == nil, debug == nil, os.execute == nil)
        string.upper = nil
    )"));
    Check(good, "lua: good mod loads");
    Check(!lines.empty() && lines[0] == "I good: loaded true true true true true true", "lua: sandbox hides io, load, fenv, require, debug, os.execute");
    lua.FloorEnter("f050", 2);
    lua.StepChange(14, 15);
    lua.Tick(0.016f);
    lua.Tick(0.016f);
    lua.Message("OpenDoor", "controller");
    Check(lines.size() == 4 && lines[1] == "I good: enter f050 2 f040 3 14" && lines[2] == "I good: step 14 15" &&
              lines[3] == "I good: message OpenDoor controller 2",
          "lua: events reach the hooks with their arguments, once each");

    lines.clear();
    Check(lua.AddMod("other", "other/init.lua", Bytes("Mod.Log(string.upper('own string library'))")), "lua: second mod loads");
    Check(lines.size() == 1 && lines[0] == "I other: OWN STRING LIBRARY", "lua: a mod's library changes stay its own");
    Check(lua.AddMod("meta", "meta/init.lua", Bytes("Mod.Log(getmetatable('') == nil)")) && lines.back() == "I meta: true",
          "lua: no string metatable");

    lines.clear();
    Check(!lua.AddMod("syntax", "syntax/init.lua", Bytes("this is not lua")), "lua: syntax error fails the mod");
    Check(lines.size() == 1 && lines[0].starts_with("E syntax: ") && lua.Failed(lua.ModCount() - 1), "lua: syntax error logged once");
    Check(!lua.AddMod("binary", "binary/init.lua", Bytes("\x1bLua\x51")), "lua: precompiled chunk refused");

    lines.clear();
    Check(lua.AddMod("crash", "crash/init.lua", Bytes(R"(Mod.On("Tick", function() error("boom") end))")), "lua: crashing mod loads");
    const size_t crash = lua.ModCount() - 1;
    lua.Tick(0.016f);
    lua.Tick(0.016f);
    lua.Tick(0.016f);
    int crash_lines = 0;
    for (const auto& line : lines) crash_lines += line.starts_with("E crash: ") ? 1 : 0;
    Check(crash_lines == 1 && lua.Failed(crash), "lua: a hook error is logged once and the mod's hooks stop");
    Check(lua.Wants(pt::ModLua::Event::Tick), "lua: the other mods keep their hooks");

    lines.clear();
    Check(lua.AddMod("loop", "loop/init.lua", Bytes(R"(Mod.On("StepChange", function() while true do end end))")), "lua: looping mod loads");
    lua.StepChange(1, 2);
    Check(lua.Failed(lua.ModCount() - 1) && !lines.empty() && lines.back().find("instruction budget") != std::string::npos,
          "lua: an endless loop is stopped");

    lines.clear();
    Check(!lua.AddMod("memory", "memory/init.lua", Bytes("local t = {} for i = 1, 1e9 do t[i] = string.rep('x', 1024) .. i end")),
          "lua: memory hog fails");
    Check(!lines.empty() && (lines.back().find("memory") != std::string::npos || lines.back().find("budget") != std::string::npos),
          "lua: memory hog stopped by a limit");
    Check(!lua.AddMod("event", "event/init.lua", Bytes("Mod.On('Save', function() end)")), "lua: unknown event fails the mod");

    lines.clear();
    lua.FloorEnter("f060", 1);
    Check(lines.size() == 1 && lines[0] == "I good: enter f060 1 f040 3 14", "lua: the good mod still works after the others failed");

    lines.clear();
    Check(lua.AddMod("spam", "spam/init.lua", Bytes("for i = 1, 2000 do Mod.Log(i) end")), "lua: chatty mod loads");
    Check(lines.size() == static_cast<size_t>(pt::ModLua::kLogLinesPerMod), "lua: Mod.Log output is capped");
}

}

int main(int argc, char** argv) {
    if (std::getenv("PT_MODS_TEST_VERBOSE")) verbose = true;
    TestKeys();
    TestManifest();
    TestPriority();
    const std::filesystem::path root = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::temp_directory_path() / "pt_mods_test";
    TestDiscoverAndRead(root);
    TestPackageEntry(root);
    TestScripts();
    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
