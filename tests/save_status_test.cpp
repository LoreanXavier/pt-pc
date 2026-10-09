// The boot load's outcomes that SaveRequest_OnBootLoadDone (0x947C60) branches on, from the slot files (save_data.cpp):
// no file, a broken one (shorter than a save), a wrong magic or newer version, an older version, and a valid one next to a bad one.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "game/save_data.h"

int main(int argc, char** argv) {
    using namespace pt::game;
    if (argc != 2) return 2;
    const auto root = std::filesystem::absolute(argv[1]);
    if (std::filesystem::exists(root)) return 2;
    std::filesystem::create_directories(root);
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
        failures += !ok;
    };
    const auto write = [&](const char* name, const std::vector<uint8_t>& bytes) {
        std::ofstream(root / name, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    };
    const auto header = [](uint64_t magic, uint32_t version) {
        std::vector<uint8_t> bytes(0x80, 0);
        std::memcpy(bytes.data(), &magic, 8);
        std::memcpy(bytes.data() + 8, &version, 4);
        return bytes;
    };
    SaveStore store;
    store.SetDirectory(root, "PT_Save_Data");
    check(store.LoadDetailed().status == SaveLoadStatus::NotFound, "no file: not found (first boot)");
    write("PT_Save_Data0", {0x70, 0x74, 0x77});
    check(store.LoadDetailed().status == SaveLoadStatus::Broken, "short file: broken (0xD, new save after the dialog)");
    write("PT_Save_Data0", header(0x1234, 3));
    check(store.LoadDetailed().status == SaveLoadStatus::Unreadable, "wrong magic: unreadable (0x14)");
    write("PT_Save_Data0", header(SaveStore::kMagic, 4));
    check(store.LoadDetailed().status == SaveLoadStatus::Unreadable, "newer version: unreadable (0x13)");
    write("PT_Save_Data0", header(SaveStore::kMagic, 2));
    const SaveLoadResult old = store.LoadDetailed();
    check(old.status == SaveLoadStatus::Old && !old.file, "older version: nothing loaded, no dialog");
    std::filesystem::remove(root / "PT_Save_Data0");
    SaveFile file;
    file.progress.floor = "f050";
    check(store.Save(file) && store.LastWrite() == SaveWriteStatus::Ok, "a save writes");
    // the first save goes to slot 0; slot 1 broken next to it
    write("PT_Save_Data1", {1, 2, 3});
    const SaveLoadResult mixed = store.LoadDetailed();
    check(mixed.status == SaveLoadStatus::Ok && mixed.file && mixed.file->progress.floor == "f050", "a valid slot wins over a broken one");
    // a save directory that is a file: the write fails, and with free space it is not the no-space case
    SaveStore blocked;
    std::ofstream(root / "blocker") << "x";
    blocked.SetDirectory(root / "blocker", "PT_Save_Data");
    check(!blocked.Save(file) && blocked.LastWrite() == SaveWriteStatus::Failed, "a failed write reports Failed");
    std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}
