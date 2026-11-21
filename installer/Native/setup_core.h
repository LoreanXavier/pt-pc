#pragma once
#include <zlib.h>

#include "engine/core/ascii.h"
#include "engine/fs/game_folder.h"
#include "engine/fs/pt_release.h"
#include "engine/platform/json_value.h"
#include "engine/platform/update_journal.h"
#include "engine/render/upscale/fg_unlock.h"
#include "shipped_history.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace pt::setup {
namespace fs = std::filesystem;

struct Hooks {
    std::function<void(const std::string&)> report;
    std::function<bool()> cancelled;
    std::function<void(uint64_t, uint64_t)> progress;
};
inline Hooks hooks;
inline void Report(const std::string& text) { if (hooks.report) hooks.report(text); }
inline void CheckCancel() {
    if (hooks.cancelled && hooks.cancelled()) throw std::runtime_error("Installation cancelled.");
}
struct Progress {
    uint64_t done = 0, total = 0;
    void Begin(uint64_t bytes) { done = 0; total = bytes; Notify(); }
    void Add(uint64_t bytes) { At(done + bytes); }
    void At(uint64_t bytes) {
        const uint64_t next = std::min(std::max(bytes, done), total);
        if (next == done) return;
        done = next;
        Notify();
    }
    void Notify() const { if (hooks.progress) hooks.progress(done, total); }
};
inline Progress progress;
inline uint64_t FolderBytes(const fs::path& dir) {
    uint64_t sum = 0;
    std::error_code error;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error)) {
        std::error_code e;
        const auto size = fs::file_size(it->path(), e);
        if (!e) sum += size;
    }
    return sum;
}
struct ProgressTrace {
    uint64_t events = 0, last_done = 0, last_total = 0;
    bool monotonic = true;
    std::vector<bool> seen = std::vector<bool>(1001, false);
    void Note(uint64_t done, uint64_t total) {
        if (events && (done < last_done || total != last_total)) monotonic = false;
        ++events;
        last_done = done;
        last_total = total;
        if (total) seen[size_t(done * 1000 / total)] = true;
    }
    std::string Summary() const {
        size_t distinct = 0;
        for (bool s : seen) distinct += s;
        return "progress events " + std::to_string(events) + " monotonic " + (monotonic ? "yes" : "no") + " distinct " + std::to_string(distinct) + " last " +
               std::to_string(last_total ? last_done * 100 / last_total : 0) + "% (" + std::to_string(last_done) + "/" + std::to_string(last_total) + " bytes)";
    }
};
inline fs::path Utf8Path(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }
inline fs::path FolderPath(const fs::path& folder) {
    fs::path path = fs::absolute(folder).lexically_normal();
    while (!path.has_filename() && path.has_relative_path()) path = path.parent_path();
    return path;
}
inline std::string PathUtf8(const fs::path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

inline fs::path Contained(const fs::path& root, const std::string& name) {
    const fs::path relative = Utf8Path(name);
    if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) throw std::runtime_error("Unsafe payload path.");
    for (const auto& part : relative) {
        const std::string text = PathUtf8(part);
        if (text == ".." || text == "." || text.find(':') != std::string::npos) throw std::runtime_error("Unsafe payload path.");
    }
    return root / relative;
}

inline constexpr const char* kGameArchives[] = {"chunk1.psarc", "texture.qar", "pathid_list_ps4.bin"};
enum class SourceKind { Package, Folder };
struct GameFiles {
    fs::path psarc, qar, pathid;
    std::string title, content_id;
    std::vector<std::string> notes;
};
struct Source {
    SourceKind kind;
    fs::path path;
    GameFiles files;
};
inline std::string ReadHead(const fs::path& file, size_t count, std::streamoff from = 0, std::ios::seekdir dir = std::ios::beg) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return {};
    input.seekg(from, dir);
    std::string bytes(count, '\0');
    input.read(bytes.data(), std::streamsize(count));
    if (input.gcount() != std::streamsize(count)) return {};
    return bytes;
}
inline bool IsPackage(const fs::path& file) { return ReadHead(file, 4) == "\x7F" "CNT"; }
inline bool IsPsarc(const fs::path& file) { return ReadHead(file, 4) == "PSAR"; }
inline bool IsQar(const fs::path& file) {
    const std::string footer = ReadHead(file, 0x24, -0x24, std::ios::end);
    return footer.size() == 0x24 && footer.compare(0x16, 2, "aq") == 0;
}
inline bool IsPathList(const fs::path& file) {
    std::error_code error;
    const auto size = fs::file_size(file, error);
    return !error && size >= 32 && size < 64ull * 1024 * 1024;
}
inline std::string ParamSfoText(const fs::path& folder, const char* key) {
    return release::ParamSfoValue(release::ReadSmallFile(folder / "sce_sys" / "param.sfo", 1024 * 1024), key);
}
inline std::string TitleId(const fs::path& folder) { return ParamSfoText(folder, "TITLE_ID"); }
inline std::string ContentId(const fs::path& folder) { return ParamSfoText(folder, "CONTENT_ID"); }
inline const char* Region(const std::string& title) {
    const release::KnownTitle* known = release::FindKnownTitle(title);
    return known ? known->region : nullptr;
}
inline std::string RegionOf(const std::string& title, const std::string& content_id) {
    if (const char* region = release::RegionOfContentId(content_id)) return region;
    if (const char* region = Region(title)) return region;
    return "unknown";
}
inline std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(pt::AsciiLower(static_cast<char>(c))); });
    return text;
}
inline GameFiles FindArchives(const fs::path& folder) {
    GameFiles found;
    std::error_code error;
    auto pick = [&](const char* name, auto kind, auto valid) -> fs::path {
        if (fs::is_regular_file(folder / name, error) && valid(folder / name)) return folder / name;
        fs::path best;
        uintmax_t best_size = 0;
        for (fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error)) {
            if (!it->is_regular_file(error) || !kind(it->path()) || !valid(it->path())) continue;
            const auto size = it->file_size(error);
            if (!error && size >= best_size) { best = it->path(); best_size = size; }
        }
        if (!best.empty()) found.notes.push_back(std::string(name) + " is named " + PathUtf8(best.filename()) + " in this release; installed as " + name);
        return best;
    };
    auto ext = [](const char* e) { return [e](const fs::path& p) { return Lower(PathUtf8(p.extension())) == e; }; };
    found.psarc = pick("chunk1.psarc", ext(".psarc"), IsPsarc);
    found.qar = pick("texture.qar", ext(".qar"), IsQar);
    found.pathid = pick("pathid_list_ps4.bin", [](const fs::path& p) { return Lower(PathUtf8(p.filename())).find("pathid_list") != std::string::npos; }, IsPathList);
    return found;
}
inline bool ConfirmPt(GameFiles& files) {
    std::string list;
    if (!files.pathid.empty()) {
        std::ifstream input(files.pathid, std::ios::binary);
        list.assign((std::istreambuf_iterator<char>(input)), {});
    }
    const bool levels = list.find("pt14_") != std::string::npos;
    const char* region = Region(files.title);
    if (!levels && !region && !(files.pathid.empty() && files.title.empty())) return false;
    if (!levels && !region) files.notes.push_back("no path list and no title ID: could not confirm this is P.T.; installed because it has the Fox Engine archive pair");
    if (!files.title.empty() && !region) files.notes.push_back("title ID " + files.title + " is not a known P.T. release; installed because its archives are P.T.'s");
    if (region && files.title != "CUSA01127") {
        const release::KnownTitle* known = release::FindKnownTitle(files.title);
        files.notes.push_back(files.title + " is " + (known && known->confirmed ? "the " : "listed as the ") + RegionOf(files.title, files.content_id) +
                              " release; the port is tested with the US release CUSA01127 (other releases are expected to hold the same data; "
                              "the game logs at its start what differs)");
    }
    if (files.pathid.empty()) files.notes.push_back("pathid_list_ps4.bin is missing: enhanced textures cannot be generated; the game runs without it");
    return true;
}
inline constexpr const char* kNotPt =
    "No P.T. game data was found here. Select the folder that holds chunk1.psarc and texture.qar (decrypted, from your own console's dump), or your P.T. fake PKG.";
inline Source ResolveSource(const fs::path& input) {
    std::error_code error;
    if (input.empty() || !fs::exists(input, error)) throw std::runtime_error("Select your P.T. game: a fake PKG, or the game folder from your console dump.");
    auto folder = [&](const fs::path& dir) -> std::optional<Source> {
        GameFiles files = FindArchives(dir);
        if (files.psarc.empty() || files.qar.empty()) return std::nullopt;
        files.title = TitleId(dir);
        files.content_id = ContentId(dir);
        if (files.title.empty() && files.content_id.empty()) {
            files.title = TitleId(dir.parent_path());
            files.content_id = ContentId(dir.parent_path());
        }
        if (files.title.empty()) files.title = release::TitleOfContentId(files.content_id);
        if (!ConfirmPt(files))
            throw std::runtime_error("This folder holds another game (" + (files.title.empty() ? std::string("no P.T. levels in its path list") : files.title) + "), not P.T.");
        return Source{SourceKind::Folder, dir, std::move(files)};
    };
    if (fs::is_regular_file(input, error)) {
        if (IsPackage(input)) return {SourceKind::Package, input, {}};
        if (auto found = folder(input.parent_path())) return *found;
        throw std::runtime_error("This file is not a PS4 package or a P.T. archive. Select a P.T. fake PKG, or the game folder from your dump.");
    }
    std::vector<fs::path> level{input};
    for (int depth = 0; depth < 3 && !level.empty(); ++depth) {
        std::vector<fs::path> next;
        for (const auto& dir : level) {
            if (auto found = folder(dir)) return *found;
            if (depth < 2)
                for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, error), end; !error && it != end && next.size() < 256; it.increment(error))
                    if (it->is_directory(error) && it->path().filename() != "sce_sys" && it->path().filename() != "sce_module") next.push_back(it->path());
        }
        level = std::move(next);
    }
    std::vector<fs::path> packages;
    for (fs::directory_iterator it(input, fs::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error))
        if (it->is_regular_file(error) && IsPackage(it->path())) packages.push_back(it->path());
    if (packages.size() == 1) return {SourceKind::Package, packages[0], {}};
    if (packages.size() > 1) throw std::runtime_error("This folder has several PKG files. Select the P.T. package itself.");
    throw std::runtime_error(kNotPt);
}
inline constexpr uint64_t kIconMaxBytes = 8ull * 1024 * 1024;
inline bool IsPng(const std::string& bytes) { return bytes.size() >= 8 && bytes.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0; }
inline uint32_t Be32(const std::string& bytes, size_t at) {
    const auto* p = reinterpret_cast<const unsigned char*>(bytes.data() + at);
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline std::string ReadSlice(const fs::path& file, uint64_t offset, uint64_t count) {
    if (count == 0 || count > kIconMaxBytes) return {};
    std::ifstream input(file, std::ios::binary);
    if (!input) return {};
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < 0 || uint64_t(end) < offset || uint64_t(end) - offset < count) return {};
    input.seekg(std::streamoff(offset));
    std::string bytes(size_t(count), '\0');
    input.read(bytes.data(), std::streamsize(count));
    return input.gcount() == std::streamsize(count) ? bytes : std::string{};
}
inline std::optional<std::string> SystemPngFromFolder(const fs::path& dir, const char* name) {
    std::error_code error;
    const fs::path icon = dir / "sce_sys" / name;
    if (!fs::is_regular_file(icon, error)) return std::nullopt;
    const auto size = fs::file_size(icon, error);
    if (error || size < 8 || size > kIconMaxBytes) return std::nullopt;
    const std::string bytes = ReadSlice(icon, 0, size);
    if (!IsPng(bytes)) return std::nullopt;
    return bytes;
}
inline std::optional<std::string> IconFromFolder(const fs::path& dir) { return SystemPngFromFolder(dir, "icon0.png"); }
inline std::optional<std::string> SystemPngFromPackage(const fs::path& pkg, const char* wanted) {
    const std::string head = ReadSlice(pkg, 0, 0x20);
    if (head.size() != 0x20 || head.compare(0, 4, "\x7F" "CNT") != 0) return std::nullopt;
    const uint32_t count = Be32(head, 0x10), table = Be32(head, 0x18);
    if (count == 0 || count > 256) return std::nullopt;
    const std::string entries = ReadSlice(pkg, table, uint64_t(count) * 32);
    if (entries.size() != uint64_t(count) * 32) return std::nullopt;
    uint32_t names_offset = 0, names_size = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t at = size_t(i) * 32;
        if (Be32(entries, at) != 0x0200) continue;
        names_offset = Be32(entries, at + 16);
        names_size = Be32(entries, at + 20);
    }
    if (names_size == 0 || names_size > 1024 * 1024) return std::nullopt;
    const std::string names = ReadSlice(pkg, names_offset, names_size);
    if (names.size() != names_size) return std::nullopt;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t at = size_t(i) * 32;
        const uint32_t id = Be32(entries, at), flags1 = Be32(entries, at + 8), offset = Be32(entries, at + 16), size = Be32(entries, at + 20);
        if (id < 0x1000 || (flags1 & 0x80000000u) || size < 8 || size > kIconMaxBytes) continue;
        const uint32_t name_at = Be32(entries, at + 4);
        if (name_at >= names.size()) continue;
        if (std::string(names.c_str() + name_at) != wanted) continue;
        const std::string bytes = ReadSlice(pkg, offset, size);
        if (IsPng(bytes)) return bytes;
    }
    return std::nullopt;
}
inline std::optional<std::string> IconFromPackage(const fs::path& pkg) { return SystemPngFromPackage(pkg, "icon0.png"); }
inline std::optional<std::string> FindSystemPng(const fs::path& input, const char* name) {
    std::error_code error;
    if (input.empty() || !fs::exists(input, error)) return std::nullopt;
    if (fs::is_regular_file(input, error)) {
        if (IsPackage(input)) {
            if (auto png = SystemPngFromPackage(input, name)) return png;
        }
        if (auto png = SystemPngFromFolder(input.parent_path(), name)) return png;
        return std::nullopt;
    }
    if (auto png = SystemPngFromFolder(input, name)) return png;
    for (fs::directory_iterator it(input, fs::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error))
        if (it->is_directory(error))
            if (auto png = SystemPngFromFolder(it->path(), name)) return png;
    return std::nullopt;
}
inline std::optional<std::string> FindIconPng(const fs::path& input) { return FindSystemPng(input, "icon0.png"); }
inline std::string PackageTitle(const fs::path& pkg) {
    const std::string head = ReadSlice(pkg, 0, 0x40 + 0x24);
    if (head.size() != 0x64 || head.compare(0, 4, "\x7F" "CNT") != 0) return {};
    return head.substr(0x40 + 7, 9);
}
inline std::vector<fs::path> GuessGameSources(const std::vector<fs::path>& folders) {
    std::vector<fs::path> found;
    size_t seen = 0;
    std::error_code error;
    auto consider = [&](const fs::path& path) {
        std::error_code e;
        if (fs::is_regular_file(path, e)) {
            if (Lower(PathUtf8(path.extension())) == ".pkg" && Region(PackageTitle(path))) found.push_back(path);
        } else if (fs::is_directory(path, e) && !fs::is_symlink(path, e)) {
            if (Region(TitleId(path)) && fs::is_regular_file(path / "sce_sys" / "pic1.png", e)) found.push_back(path);
        }
    };
    for (const auto& folder : folders) {
        if (folder.empty() || !fs::is_directory(folder, error)) continue;
        for (fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, error), end; !error && it != end && seen < 4000; it.increment(error)) {
            ++seen;
            consider(it->path());
            std::error_code e;
            if (it->is_directory(e) && !it->is_symlink(e) && !Region(TitleId(it->path())))
                for (fs::directory_iterator inner(it->path(), fs::directory_options::skip_permission_denied, e), stop; !e && inner != stop && seen < 4000; inner.increment(e)) {
                    ++seen;
                    consider(inner->path());
                }
        }
        error.clear();
    }
    return found;
}
inline void CopyArchives(const GameFiles& files, const fs::path& assets) {
    fs::create_directories(assets);
    std::vector<char> buffer(4 * 1024 * 1024);
    const fs::path sources[] = {files.psarc, files.qar, files.pathid};
    for (int i = 0; i < 3; ++i) {
        if (sources[i].empty()) continue;
        const char* name = kGameArchives[i];
        Report(std::string("Copying ") + name + " from your game folder...");
        std::ifstream input(sources[i], std::ios::binary);
        std::ofstream output(assets / name, std::ios::binary | std::ios::trunc);
        if (!input || !output) throw std::runtime_error(std::string("Could not copy ") + name + ".");
        while (input) {
            CheckCancel();
            input.read(buffer.data(), std::streamsize(buffer.size()));
            if (input.gcount()) {
                output.write(buffer.data(), input.gcount());
                progress.Add(uint64_t(input.gcount()));
            }
        }
        if (!input.eof() || !output) throw std::runtime_error(std::string("Could not copy ") + name + " (check free space).");
        output.close();
        std::error_code error;
        if (fs::file_size(assets / name, error) != fs::file_size(sources[i], error) || error) throw std::runtime_error(std::string("Copy of ") + name + " is incomplete.");
    }
    std::ofstream source(assets / "source.txt", std::ios::binary);
    source << "content_id=" << files.content_id << "\ntitle_id=" << files.title << "\nregion=" << RegionOf(files.title, files.content_id) << "\nsource=folder\n";
    for (const auto& note : files.notes) source << "warning=" << note << "\n";
}
inline std::vector<std::string> WriteNotes(const Source& source, const fs::path& staging) {
    std::vector<std::string> notes = source.files.notes;
    if (source.kind == SourceKind::Package) {
        std::ifstream log(staging / "install-extraction.log");
        for (std::string line; std::getline(log, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.starts_with("warning: ")) notes.push_back(line.substr(9));
        }
    }
    if (!notes.empty()) {
        std::ofstream file(staging / "install-notes.txt", std::ios::binary);
        for (const auto& note : notes) file << note << "\n";
    }
    return notes;
}

struct Reader {
    const unsigned char* p;
    size_t left;
    template <typename T> T Number() {
        if (left < sizeof(T)) throw std::runtime_error("Truncated installer payload.");
        T result{};
        std::memcpy(&result, p, sizeof(T));
        p += sizeof(T);
        left -= sizeof(T);
        return result;
    }
    std::string String(size_t n) {
        if (n > left) throw std::runtime_error("Truncated installer payload.");
        std::string s(reinterpret_cast<const char*>(p), n);
        p += n;
        left -= n;
        return s;
    }
};
struct InstalledFile {
    std::string path;
    std::string sha256;
};
inline bool IsUserDataPath(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    const size_t slash = path.find('/');
    std::string first = path.substr(0, slash);
#ifdef _WIN32
    while (!first.empty() && (first.back() == '.' || first.back() == ' ')) first.pop_back();
#endif
    return Lower(std::move(first)) == "data";
}
inline void ProbeWritableDirectory(const fs::path& dir, const std::string& unique_id, const std::string& description) {
    std::error_code error;
    if (fs::is_symlink(dir, error) || !fs::is_directory(dir, error))
        throw std::runtime_error("The " + description + " must be a writable folder.");
    const fs::path probe = dir / (".pt-write-probe-" + unique_id);
    if (!fs::create_directory(probe, error) || error)
        throw std::runtime_error("The " + description + " is not writable.");
    try {
        std::ofstream output(probe / "write-test", std::ios::binary | std::ios::trunc);
        output.put('x');
        output.flush();
        if (!output) throw std::runtime_error("The " + description + " is not writable.");
        output.close();
        if (!output) throw std::runtime_error("The " + description + " is not writable.");
        fs::remove_all(probe);
    } catch (...) {
        fs::remove_all(probe, error);
        throw;
    }
}
inline void EnsureDataDirectory(const fs::path& install, const std::string& unique_id,
                                const std::function<void(const fs::path&)>& check_parents) {
    const fs::path data = install / "data";
    std::error_code error;
    if (!fs::exists(data, error)) fs::create_directory(data, error);
    if (error) throw std::runtime_error("Could not create the game's data folder.");
    if (check_parents) check_parents(data);
    ProbeWritableDirectory(data, unique_id, "game data folder");
}
inline std::string UpdateFolderProblem(const fs::path& install, const std::string& unique_id) {
    try {
        ProbeWritableDirectory(install, unique_id, "game folder " + PathUtf8(install));
        std::error_code error;
        if (fs::is_directory(install / "data", error)) ProbeWritableDirectory(install / "data", unique_id, "data folder " + PathUtf8(install / "data"));
        ProbeWritableDirectory(install.parent_path(), unique_id, "folder above the game, " + PathUtf8(install.parent_path()) + ",");
    } catch (const std::exception& e) {
        return e.what();
    }
    return {};
}
inline void ValidateProgramFiles(const std::vector<InstalledFile>& files) {
    for (const auto& file : files)
        if (IsUserDataPath(file.path)) throw std::runtime_error("The installer payload cannot contain files under data/.");
}
inline void RequireProgramFiles(const std::vector<InstalledFile>& files, const std::vector<std::string>& required) {
    ValidateProgramFiles(files);
    for (const auto& path : required)
        if (std::none_of(files.begin(), files.end(), [&](const InstalledFile& file) { return Lower(file.path) == Lower(path); }))
            throw std::runtime_error("The installer payload is missing required runtime file " + path + ".");
}
inline uint64_t PayloadBytes(const unsigned char* data, size_t size) {
    Reader reader{data, size};
    if (reader.String(8) != "PTSETUP1") throw std::runtime_error("Invalid installer payload.");
    const uint32_t count = reader.Number<uint32_t>();
    if (count > 10000) throw std::runtime_error("Invalid payload count.");
    uint64_t total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        reader.String(reader.Number<uint16_t>());
        total += reader.Number<uint64_t>();
        const auto packed = reader.Number<uint64_t>();
        reader.String(64);
        if (packed > reader.left) throw std::runtime_error("Invalid payload size.");
        reader.p += packed;
        reader.left -= packed;
    }
    return total;
}
struct PayloadEntry {
    std::string path;
    uint64_t size = 0;
    std::string sha256;
};
inline std::vector<PayloadEntry> ListPayload(const unsigned char* data, size_t size) {
    Reader reader{data, size};
    if (reader.String(8) != "PTSETUP1") throw std::runtime_error("Invalid installer payload.");
    const uint32_t count = reader.Number<uint32_t>();
    if (count > 10000) throw std::runtime_error("Invalid payload count.");
    std::vector<PayloadEntry> entries;
    for (uint32_t i = 0; i < count; ++i) {
        PayloadEntry entry;
        entry.path = reader.String(reader.Number<uint16_t>());
        entry.size = reader.Number<uint64_t>();
        const auto packed = reader.Number<uint64_t>();
        entry.sha256 = reader.String(64);
        if (packed > reader.left) throw std::runtime_error("Invalid payload size.");
        reader.p += packed;
        reader.left -= packed;
        entries.push_back(std::move(entry));
    }
    if (reader.left) throw std::runtime_error("Unexpected payload data.");
    return entries;
}
inline std::vector<InstalledFile> UnpackPayload(const unsigned char* data, size_t size, const fs::path& root,
                                           const std::function<std::string(const fs::path&)>& hash_file,
                                           const std::function<bool(const std::string&)>& want = {}) {
    Reader reader{data, size};
    if (reader.String(8) != "PTSETUP1") throw std::runtime_error("Invalid installer payload.");
    const uint32_t count = reader.Number<uint32_t>();
    if (count > 10000) throw std::runtime_error("Invalid payload count.");
    std::vector<InstalledFile> written;
    std::string group;
    for (uint32_t i = 0; i < count; ++i) {
        CheckCancel();
        auto name = reader.String(reader.Number<uint16_t>());
        auto bytes_size = reader.Number<uint64_t>();
        auto packed = reader.Number<uint64_t>();
        auto digest = reader.String(64);
        if (bytes_size > 512ull * 1024 * 1024 || packed > reader.left) throw std::runtime_error("Invalid payload size.");
        if (want && !want(PathUtf8(Utf8Path(name)))) {
            reader.p += packed;
            reader.left -= packed;
            continue;
        }
        const unsigned char* compressed = reader.p;
        reader.p += packed;
        reader.left -= packed;
        const fs::path path = Contained(root, name);
        if (IsUserDataPath(name)) throw std::runtime_error("The installer payload cannot contain files under data/.");
        const std::string folder = path.parent_path() == root ? "program files" : PathUtf8(*Utf8Path(name).begin());
        if (folder != group) {
            group = folder;
            Report("Extracting " + group + "...");
        }
        fs::create_directories(path.parent_path());
        if (fs::exists(path)) throw std::runtime_error("Duplicate payload path.");
        {
            std::ofstream file(path, std::ios::binary);
            z_stream z{};
            if (inflateInit(&z) != Z_OK) throw std::runtime_error("Installer payload decompression failed.");
            std::vector<unsigned char> piece(1024 * 1024);
            uint64_t produced = 0, consumed = 0;
            int result = Z_OK;
            try {
                while (result != Z_STREAM_END) {
                    if (z.avail_in == 0 && consumed < packed) {
                        const uint64_t take = std::min<uint64_t>(packed - consumed, 64ull * 1024 * 1024);
                        z.next_in = const_cast<Bytef*>(compressed + consumed);
                        z.avail_in = uInt(take);
                        consumed += take;
                    }
                    z.next_out = piece.data();
                    z.avail_out = uInt(piece.size());
                    result = inflate(&z, Z_NO_FLUSH);
                    if (result != Z_OK && result != Z_STREAM_END) throw std::runtime_error("Installer payload decompression failed.");
                    const size_t chunk = piece.size() - z.avail_out;
                    produced += chunk;
                    if (produced > bytes_size) throw std::runtime_error("Installer payload decompression failed.");
                    file.write(reinterpret_cast<const char*>(piece.data()), std::streamsize(chunk));
                    progress.Add(chunk);
                    CheckCancel();
                    if (result != Z_STREAM_END && chunk == 0 && z.avail_in == 0 && consumed >= packed)
                        throw std::runtime_error("Installer payload decompression failed.");
                }
            } catch (...) {
                inflateEnd(&z);
                throw;
            }
            inflateEnd(&z);
            if (produced != bytes_size || z.avail_in != 0 || consumed != packed) throw std::runtime_error("Installer payload decompression failed.");
            if (!file) throw std::runtime_error("Could not write installation files.");
        }
        if (hash_file(path) != digest) throw std::runtime_error("Installer payload integrity check failed.");
        written.push_back({PathUtf8(Utf8Path(name)), digest});
    }
    if (reader.left) throw std::runtime_error("Unexpected payload data.");
    return written;
}

inline constexpr const char* kManifestName = "pt-install-manifest.txt";
inline constexpr const char* kFileListName = "pt-files.json";
#ifdef _WIN32
inline constexpr const char* kGameExe = "pt.exe";
#else
inline constexpr const char* kGameExe = "pt";
#endif
inline std::string PathKey(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
#ifdef _WIN32
    return Lower(std::move(path));
#else
    return path;
#endif
}
inline std::vector<PayloadEntry> ReadFileList(const fs::path& file) {
    std::error_code error;
    if (!fs::is_regular_file(file, error) || fs::file_size(file, error) > 16 * 1024 * 1024) return {};
    std::ifstream in(file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    json::Value root;
    if (!json::Reader(text).Parse(root) || !root.object) return {};
    const auto list = root.members.find("files");
    if (list == root.members.end()) return {};
    std::vector<PayloadEntry> entries;
    for (const auto& item : list->second.items) {
        const auto path = item.members.find("path"), size = item.members.find("size"), sha = item.members.find("sha256");
        if (path == item.members.end() || size == item.members.end() || sha == item.members.end() || !path->second.is_string || sha->second.text.size() != 64) return {};
        PayloadEntry entry{path->second.text, std::strtoull(size->second.text.c_str(), nullptr, 10), sha->second.text};
        try {
            Contained("x", entry.path);
        } catch (...) {
            return {};
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}
inline std::set<std::string> FgUnlockFiles(const fs::path& install) {
    std::set<std::string> keys;
    const fs::path record = install / std::string(fg_unlock::kManifest);
    std::error_code error;
    if (!fs::is_regular_file(record, error)) return keys;
    keys.insert(PathKey(std::string(fg_unlock::kManifest)));
    keys.insert(PathKey(std::string(fg_unlock::kIni)));
    json::Value root;
    std::string text;
    if (fs::file_size(record, error) <= 1024 * 1024 && !error) {
        std::ifstream in(record, std::ios::binary);
        text.assign((std::istreambuf_iterator<char>(in)), {});
    }
    if (!json::Reader(text).Parse(root) || !root.object) {
        for (const char* name : fg_unlock::kProxyCandidates) keys.insert(PathKey(name));
        return keys;
    }
    if (const auto proxy = root.members.find("proxy"); proxy != root.members.end() && proxy->second.is_string && !proxy->second.text.empty())
        keys.insert(PathKey(proxy->second.text));
    if (const auto files = root.members.find("files"); files != root.members.end())
        for (const auto& item : files->second.items) {
            if (item.is_string && !item.text.empty()) keys.insert(PathKey(item.text));
            else if (item.object)
                if (const auto name = item.members.find("name"); name != item.members.end() && !name->second.text.empty()) keys.insert(PathKey(name->second.text));
        }
    return keys;
}
inline bool IsFgProxyName(const std::string& path) {
    for (const char* name : fg_unlock::kProxyCandidates)
        if (PathKey(path) == PathKey(name)) return true;
    return false;
}
inline bool HasFgProxyMarker(const fs::path& file) {
    std::error_code error;
    if (!fs::is_regular_file(file, error) || fs::file_size(file, error) > 64 * 1024 * 1024) return false;
    std::ifstream in(file, std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(in)), {});
    return data.find(std::string(fg_unlock::kProxyMarker)) != std::string::npos;
}
inline std::set<std::string> UnlockFiles(const fs::path& install) {
    std::set<std::string> keys = FgUnlockFiles(install);
    const fs::path record = install / "dlss5-autopilot.json";
    std::error_code error;
    if (!fs::is_regular_file(record, error) || fs::file_size(record, error) > 1024 * 1024) return keys;
    keys.insert(PathKey("dlss5-autopilot.json"));
    std::ifstream in(record, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    json::Value root;
    if (!json::Reader(text).Parse(root) || !root.object) return keys;
    if (const auto files = root.members.find("files"); files != root.members.end())
        for (const auto& item : files->second.items)
            if (item.is_string && !item.text.empty()) keys.insert(PathKey(item.text));
    return keys;
}
inline std::vector<std::string> HistoryPaths() {
#ifdef _WIN32
    return {std::begin(history::kWindowsPaths), std::end(history::kWindowsPaths)};
#else
    return {std::begin(history::kLinuxPaths), std::end(history::kLinuxPaths)};
#endif
}
inline std::string HistoryVersion(const std::string& game_sha256) {
#ifdef _WIN32
    for (const auto& release : history::kWindowsReleases)
#else
    for (const auto& release : history::kLinuxReleases)
#endif
        if (game_sha256 == release.game_sha256) return release.version;
    return {};
}
inline void MoveFileRetrying(const fs::path& from, const fs::path& to) {
    std::error_code error;
    for (int attempt = 0;; ++attempt) {
        fs::rename(from, to, error);
        if (!error) return;
#ifdef _WIN32
        constexpr int kAttempts = 30;
#else
        constexpr int kAttempts = 0;
#endif
        std::error_code e;
        if (attempt >= kAttempts || !fs::exists(from, e)) throw fs::filesystem_error("rename", from, to, error);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
struct ExistingInstall {
    bool found = false;
    bool has_manifest = false;
    std::string version;
    std::vector<InstalledFile> files;
    std::vector<PayloadEntry> package_files;
    std::string source;
    bool portable = false;
};
inline void WriteManifest(const fs::path& dir, const std::string& version, const std::vector<InstalledFile>& files, const std::string& source = {}) {
    const fs::path temporary = dir / (std::string(kManifestName) + ".new");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << "pt-port-install 1\nversion=" << version << "\n";
        if (!source.empty() && source.find_first_of("\r\n") == std::string::npos) out << "source=" << source << "\n";
        for (const auto& file : files) out << "file=" << file.sha256 << " " << file.path << "\n";
        if (!out) throw std::runtime_error("Could not write the install manifest.");
    }
    fs::rename(temporary, dir / kManifestName);
}
inline bool ArchivesUsable(const fs::path& assets) {
    std::error_code error;
    return fs::is_regular_file(assets / "chunk1.psarc", error) && IsPsarc(assets / "chunk1.psarc") &&
           fs::is_regular_file(assets / "texture.qar", error) && IsQar(assets / "texture.qar");
}
inline constexpr const char* kGameFolder = "CUSA01127";
inline std::string InstalledGameFolder(const fs::path& install) {
    if (ArchivesUsable(install / kGameFolder)) return kGameFolder;
    for (const fs::path& folder : game_folder::ChildFolders(install, 64))
        if (!release::TitleIdOfName(PathUtf8(folder.filename())).empty() && ArchivesUsable(folder)) return PathUtf8(folder.filename());
    return {};
}
inline bool EmptyDirectory(const fs::path& dir) {
    std::error_code error;
    return fs::is_directory(dir, error) && !fs::is_symlink(dir, error) && fs::directory_iterator(dir, error) == fs::directory_iterator() && !error;
}
inline bool ResultPathWritable(const fs::path& file) {
    std::error_code error;
    if (!fs::exists(file, error)) return true;
    return fs::is_regular_file(file, error) && fs::file_size(file, error) <= 64 * 1024 && !error;
}
inline ExistingInstall InspectInstall(const fs::path& dir) {
    ExistingInstall install;
    std::error_code error;
    if (!fs::exists(dir, error) || EmptyDirectory(dir)) return install;
    if (!fs::is_directory(dir, error) || fs::is_symlink(dir, error)) throw std::runtime_error("Choose a new installation folder. Existing folders are never overwritten.");
    std::ifstream manifest(dir / kManifestName, std::ios::binary);
    std::string line;
    if (manifest && std::getline(manifest, line) && line == "pt-port-install 1") {
        install.found = install.has_manifest = true;
        while (std::getline(manifest, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.starts_with("version=")) install.version = line.substr(8);
            if (line.starts_with("source=")) install.source = line.substr(7);
            if (line.starts_with("file=") && line.size() > 5 + 65) {
                InstalledFile file{line.substr(5 + 65), line.substr(5, 64)};
                Contained(dir, file.path);
                if (!IsUserDataPath(file.path)) install.files.push_back(std::move(file));
            }
        }
        install.package_files = ReadFileList(dir / kFileListName);
        return install;
    }
    const bool exe = fs::is_regular_file(dir / kGameExe, error) || fs::is_regular_file(dir / "pt.exe", error);
    const bool archives = !InstalledGameFolder(dir).empty() || (fs::is_regular_file(dir / kGameFolder / "chunk1.psarc", error) && fs::is_regular_file(dir / kGameFolder / "texture.qar", error));
    if (exe && archives) {
        install.found = true;
        install.package_files = ReadFileList(dir / kFileListName);
        return install;
    }
    throw std::runtime_error("This folder holds something else. Choose a new installation folder, or a folder with a P.T. install to update it.");
}
inline ExistingInstall InspectPortable(const fs::path& dir, const std::function<std::string(const fs::path&)>& hash_file = {}) {
    ExistingInstall copy;
    std::error_code error;
    if (!fs::is_directory(dir, error) || fs::is_symlink(dir, error) || fs::exists(dir / kManifestName, error)) return copy;
    if (!fs::is_regular_file(dir / kGameExe, error)) return copy;
    copy.package_files = ReadFileList(dir / kFileListName);
    const bool lists_game = std::any_of(copy.package_files.begin(), copy.package_files.end(), [](const PayloadEntry& entry) { return entry.path == kGameExe; });
    if (!lists_game) {
        copy.package_files.clear();
        return copy;
    }
    copy.found = copy.portable = true;
    if (hash_file) {
        try {
            copy.version = HistoryVersion(hash_file(dir / kGameExe));
        } catch (const std::exception&) {
        }
    }
    return copy;
}
inline std::vector<std::string> GameFilesFolders(const fs::path& dest) {
    std::vector<std::string> found;
    if (game_folder::LooksLikeGameDir(dest)) found.push_back("");
    std::vector<fs::path> level{dest};
    for (int depth = 0; depth < 2 && !level.empty(); ++depth) {
        std::vector<fs::path> next;
        for (const fs::path& dir : level)
            for (fs::path& child : game_folder::ChildFolders(dir, 64)) {
                std::error_code error;
                if (fs::is_symlink(child, error)) continue;
                if (game_folder::LooksLikeGameDir(child)) found.push_back(PathUtf8(fs::relative(child, dest, error)) + "/");
                next.push_back(std::move(child));
            }
        level = std::move(next);
    }
    return found;
}
inline bool InGameFilesFolder(const std::string& path, const std::vector<std::string>& folders) {
    const std::string key = PathKey(path);
    for (const auto& folder : folders) {
        if (folder.empty()) {
            if (key == PathKey("chunk1.psarc") || key == PathKey("texture.qar") || key == PathKey("pathid_list_ps4.bin") || key == PathKey("source.txt") ||
                key.starts_with(PathKey("sce_sys/")))
                return true;
        } else if (key.starts_with(PathKey(folder))) {
            return true;
        }
    }
    return false;
}
inline bool KnownProgramFile(const std::string& path) {
    static const char* const kNames[] = {"pt.exe", "pt", "amd_fidelityfx_vk.dll", "nvngx_dlss.dll", "libxess.dll", "README-UPDATE.txt", "manifest.json"};
    for (const char* name : kNames)
        if (path == name) return true;
    if (path == "voice/cmudict-en-us.dict" || path.starts_with("voice/en-us/") || path.starts_with("voice/vosk/")) return true;
    if (path.starts_with("lisa_date/")) return true;
    return (path.starts_with("shaders/") && path.ends_with(".spv")) || path.starts_with("extractor/");
}
inline bool IsPlayerPath(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    const std::string lower = Lower(path);
    if (IsUserDataPath(lower)) return true;
    for (const char* top : {"mods/", "cusa01127/", "enhanced-textures/", "saves/", "screenshots/", "photos/"})
        if (lower.starts_with(top)) return true;
    if (const size_t slash = path.find('/'); slash != std::string::npos && !release::TitleIdOfName(path.substr(0, slash)).empty()) return true;
    if (lower == Lower(std::string(fg_unlock::kManifest)) || lower == Lower(std::string(fg_unlock::kIni)) ||
        lower.starts_with(Lower(std::string(fg_unlock::kDir)) + "/"))
        return true;
    return lower == "pt.ini" || lower.ends_with(".log") || lower == "install-notes.txt" || lower == "icon0.png";
}
struct SwapResult {
    int replaced = 0, added = 0, removed = 0;
    std::vector<std::string> notes;
};
inline fs::path JournalFile(const fs::path& install) { return install / "data" / update_journal::kName; }
struct SwapJournal {
    fs::path file;
    std::string old_version, new_version;
    fs::path download;
    uint64_t pid = 0;
    int crash_at = -1;
    int pause_at = -1;
};
struct SimulatedCrash {};
inline void PruneEmptyParents(const fs::path& root, const std::string& path) {
    for (fs::path relative = Utf8Path(path).parent_path(); !relative.empty(); relative = relative.parent_path()) {
        if (IsPlayerPath(PathUtf8(relative) + "/")) return;
        const fs::path dir = root / relative;
        std::error_code error;
        if (fs::is_symlink(dir, error) || !EmptyDirectory(dir)) return;
        fs::remove(dir, error);
        if (error) return;
    }
}
inline std::vector<std::string> ObsoleteProgramFiles(const fs::path& dest, const ExistingInstall& old, const std::vector<std::string>& shipped_paths,
                                                     std::vector<std::string>* notes = nullptr) {
    std::set<std::string> shipped;
    for (const auto& path : shipped_paths) shipped.insert(PathKey(path));
    const std::set<std::string> unlock = UnlockFiles(dest);
    const std::vector<std::string> game_files = GameFilesFolders(dest);
    const bool fg_record = !FgUnlockFiles(dest).empty();
    std::set<std::string> seen;
    std::vector<std::string> obsolete;
    auto note = [&](const std::string& text) { if (notes) notes->push_back(text); };
    auto consider = [&](const std::string& path) {
        const std::string key = PathKey(path);
        if (shipped.count(key) || IsPlayerPath(path) || PathKey(path) == PathKey(kManifestName) || InGameFilesFolder(path, game_files) ||
            !seen.insert(key).second)
            return;
        try {
            Contained(dest, path);
        } catch (...) {
            return;
        }
        std::error_code error;
        const fs::path file = Contained(dest, path);
        if (!fs::is_regular_file(file, error)) return;
        if (unlock.count(key)) {
            if (IsFgProxyName(path)) note("kept " + path + ": the frame generation unlock's record " + std::string(fg_unlock::kManifest) + " names it");
            return;
        }
        if (IsFgProxyName(path)) {
            if (!HasFgProxyMarker(file)) {
                note("kept " + path + ": not the community frame generation proxy an earlier package shipped (another program's file)");
                return;
            }
            note("removed " + path + ": the community frame generation proxy that 1.0.6 and 1.0.7 shipped by mistake (" +
                 (fg_record ? "the unlock's record does not name it" : "no frame generation unlock record") + ")");
        }
        obsolete.push_back(path);
    };
    if (!old.found) return obsolete;
    for (const auto& file : old.files) consider(file.path);
    for (const auto& file : old.package_files) consider(file.path);
    for (const auto& path : HistoryPaths()) consider(path);
    if (!old.has_manifest) {
        std::error_code error;
        for (fs::recursive_directory_iterator it(dest, fs::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error)) {
            if (!it->is_regular_file(error)) continue;
            const std::string path = PathUtf8(fs::relative(it->path(), dest));
            if (KnownProgramFile(path)) consider(path);
        }
    }
    std::sort(obsolete.begin(), obsolete.end());
    return obsolete;
}
inline SwapResult SwapProgramFiles(const fs::path& staging, const fs::path& dest, const std::vector<InstalledFile>& files,
                                   const std::vector<std::string>& extra, const ExistingInstall& old, const fs::path& backup, int fail_at = -1,
                                   const SwapJournal* record = nullptr, const std::vector<std::string>& shipped_paths = {}) {
    ValidateProgramFiles(files);
    for (const auto& path : extra)
        if (IsUserDataPath(path)) throw std::runtime_error("The installer cannot replace files under data/.");
    struct Step {
        std::string path;
        bool placed;
        bool backed;
    };
    std::vector<Step> journal;
    SwapResult result;
    int step = 0;
    auto tick = [&] {
        if (record && step == record->crash_at) throw SimulatedCrash{};
        if (record && step == record->pause_at) {
            std::ofstream(fs::path(record->file).concat(".paused"), std::ios::binary) << step << "\n";
            std::this_thread::sleep_for(std::chrono::seconds(300));
        }
        if (step++ == fail_at) throw std::runtime_error("Test failure during the update.");
    };
    auto backup_old = [&](const std::string& path) {
        const fs::path target = Contained(dest, path), saved = Contained(backup, path);
        fs::create_directories(saved.parent_path());
        MoveFileRetrying(target, saved);
    };
    bool journal_written = false;
    try {
        std::vector<std::string> all;
        for (const auto& file : files) all.push_back(file.path);
        all.insert(all.end(), extra.begin(), extra.end());
        std::vector<bool> existed;
        for (const auto& path : all) {
            const fs::path target = Contained(dest, path);
            std::error_code error;
            existed.push_back(fs::exists(target, error));
            if (existed.back() && fs::is_directory(target, error)) throw std::runtime_error("A folder is in the way of " + path + ".");
        }
        std::vector<std::string> everything = shipped_paths;
        everything.insert(everything.end(), all.begin(), all.end());
        const std::vector<std::string> obsolete = ObsoleteProgramFiles(dest, old, everything, &result.notes);
        if (record) {
            update_journal::Journal plan;
            plan.state = update_journal::State::Swapping;
            plan.install = fs::absolute(dest);
            plan.staging = fs::absolute(staging);
            plan.backup = fs::absolute(backup);
            plan.download = record->download;
            plan.old_version = record->old_version;
            plan.new_version = record->new_version;
            plan.pid = record->pid;
            for (size_t i = 0; i < all.size(); ++i) plan.steps.push_back({existed[i] ? 'r' : 'a', all[i]});
            for (const auto& path : obsolete) {
                std::error_code error;
                if (fs::is_regular_file(Contained(dest, path), error)) plan.steps.push_back({'d', path});
            }
            if (!update_journal::Write(record->file, plan)) throw std::runtime_error("Could not write the update journal.");
            journal_written = true;
        }
        for (size_t i = 0; i < all.size(); ++i) {
            const std::string& path = all[i];
            CheckCancel();
            tick();
            const fs::path target = Contained(dest, path);
            if (existed[i]) backup_old(path);
            journal.push_back({path, false, existed[i]});
            fs::create_directories(target.parent_path());
            MoveFileRetrying(Contained(staging, path), target);
            journal.back().placed = true;
            existed[i] ? ++result.replaced : ++result.added;
        }
        for (const auto& path : obsolete) {
            tick();
            std::error_code error;
            if (!fs::is_regular_file(Contained(dest, path), error)) continue;
            backup_old(path);
            journal.push_back({path, false, true});
            ++result.removed;
        }
        for (const auto& path : obsolete) PruneEmptyParents(dest, path);
        tick();
    } catch (const SimulatedCrash&) {
        throw;
    } catch (...) {
        bool undone = true;
        for (auto it = journal.rbegin(); it != journal.rend(); ++it) {
            std::error_code error;
            if (it->placed) fs::rename(Contained(dest, it->path), Contained(staging, it->path), error);
            if (error) undone = false;
            if (it->placed && !it->backed) PruneEmptyParents(dest, it->path);
            error.clear();
            if (it->backed) {
                fs::create_directories(Contained(dest, it->path).parent_path(), error);
                fs::rename(Contained(backup, it->path), Contained(dest, it->path), error);
            }
            if (error) undone = false;
        }
        if (journal_written && undone) {
            std::error_code error;
            fs::remove(record->file, error);
        }
        throw;
    }
    return result;
}

struct InstallSteps {
    std::string version;
    std::string unique_id;
    std::function<void(const fs::path&)> check_parents;
    std::function<void()> verify_integrity;
    std::function<uint64_t()> payload_bytes;
    std::function<std::vector<InstalledFile>(const fs::path&)> unpack;
    std::function<void(const std::vector<InstalledFile>&)> validate_runtime;
    std::function<void(const fs::path&, const fs::path&)> extract;
    std::function<void(const fs::path&)> shortcut;
    bool keep_backup = false;
    fs::path download;
    uint64_t pid = 0;
    int fail_at = -1;
    std::function<bool(uint64_t)> process_alive;
    std::function<std::vector<fs::path>()> known_folders;
    std::function<std::vector<PayloadEntry>()> entries;
    std::function<std::vector<InstalledFile>(const fs::path& folder, const std::vector<PayloadEntry>& wanted)> fetch;
    std::function<std::string(const fs::path&)> hash_file;
    std::optional<std::vector<PayloadEntry>> needed;
    std::function<void()> before_swap;
    int pause_at = -1;
    std::string corrupt_after_swap;
    bool portable = false;
    std::string installed_version;
};
struct InstallOutcome {
    bool updated = false;
    bool had_manifest = false;
    std::string old_version;
    bool archives_restored = false;
    SwapResult swap;
    std::vector<std::string> notes;
    bool title_picture_missing = false;
    bool recovered = false;
    fs::path journal;
    size_t unchanged = 0, verified = 0, repaired = 0;
    uint64_t written_bytes = 0;
};
inline std::vector<PayloadEntry> DifferingFiles(const fs::path& dest, const std::vector<PayloadEntry>& entries,
                                                const std::function<std::string(const fs::path&)>& hash_file) {
    std::vector<PayloadEntry> differing;
    for (const auto& entry : entries) {
        CheckCancel();
        const fs::path target = Contained(dest, entry.path);
        std::error_code error;
        bool same = fs::is_regular_file(target, error) && !fs::is_symlink(target, error) && fs::file_size(target, error) == entry.size && !error;
        if (same) {
            try {
                same = hash_file(target) == entry.sha256;
            } catch (const std::exception&) {
                if (hooks.cancelled && hooks.cancelled()) throw;
                same = false;
            }
        }
        if (!same) differing.push_back(entry);
    }
    return differing;
}
inline void LogUpdate(const fs::path& install, const std::string& line) {
    std::error_code error;
    if (!fs::is_directory(install / "data", error)) return;
    std::ofstream log(install / "data" / "pt-update.log", std::ios::app | std::ios::binary);
    const std::time_t now = std::time(nullptr);
    char stamp[32] = {};
    std::tm local{};
#ifdef _WIN32
    const bool ok = localtime_s(&local, &now) == 0;
#else
    const bool ok = localtime_r(&now, &local) != nullptr;
#endif
    if (ok) std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S ", &local);
    log << stamp << line << "\n";
}
inline bool RecoverEarlierUpdate(const fs::path& install, const std::function<bool(uint64_t)>& process_alive) {
    const fs::path file = JournalFile(install);
    std::error_code error;
    if (!fs::exists(file, error)) return false;
    const auto journal = update_journal::Read(file);
    if (!journal) {
        fs::remove(file, error);
        return false;
    }
    if (journal->state == update_journal::State::Swapping) {
        if (journal->pid && process_alive && process_alive(journal->pid)) throw std::runtime_error("Another update of this install is running.");
        std::string why;
        if (!update_journal::Rollback(*journal, &why)) throw std::runtime_error("An earlier update could not be undone (" + why + "). Close P.T. and try again.");
        update_journal::RemoveFolders(*journal);
        fs::remove(file, error);
        return true;
    }
    update_journal::RemoveFolders(*journal);
    fs::remove(file, error);
    return false;
}
inline InstallOutcome RunInstall(const fs::path& input, fs::path destination, bool shortcut, const InstallSteps& steps) {
    if (destination.empty()) throw std::runtime_error("Choose an installation folder.");
    destination = FolderPath(destination);
    steps.check_parents(destination);
    std::error_code journal_error;
    const bool recovered = fs::exists(JournalFile(fs::absolute(destination)), journal_error) &&
                           RecoverEarlierUpdate(fs::absolute(destination), steps.process_alive);
    const ExistingInstall old = steps.portable ? InspectPortable(destination, steps.hash_file) : InspectInstall(destination);
    if (steps.portable && !old.found) throw std::runtime_error("This folder is not a P.T. PC Port portable copy (no pt-files.json naming the game).");
    InstallOutcome outcome;
    outcome.recovered = recovered;
    destination = fs::absolute(destination);
    const fs::path parent = destination.parent_path();
    const bool update = old.found;
    const std::string existing_game = update && !steps.portable ? InstalledGameFolder(destination) : std::string();
    const bool need_archives = existing_game.empty() && !steps.portable;
    const std::string game_folder_name = existing_game.empty() ? std::string(kGameFolder) : existing_game;
    std::optional<Source> source;
    if (need_archives) {
        if (update && input.empty())
            throw std::runtime_error("The game archives of this install are missing or damaged. Select your P.T. PKG or dumped game folder to restore them.");
        source = ResolveSource(input);
    }
    Report("Verifying the setup file...");
    steps.verify_integrity();
    fs::create_directories(parent);
    ProbeWritableDirectory(parent, steps.unique_id, "installation parent folder");
    if (update) {
        ProbeWritableDirectory(destination, steps.unique_id, "existing installation folder");
        EnsureDataDirectory(destination, steps.unique_id, steps.check_parents);
    }
    if (fs::space(parent).available < (need_archives ? 3ull : 1ull) * 1024 * 1024 * 1024)
        throw std::runtime_error(need_archives ? "At least 3 GB free space is required." : "At least 1 GB free space is required.");
    const fs::path staging = parent / ((update ? ".pt-update-" : ".pt-install-") + steps.unique_id);
    const fs::path backup = parent / (".pt-update-old-" + steps.unique_id);
    if (!fs::create_directory(staging)) throw std::runtime_error("Could not create staging directory.");
    auto remove_ours = [&](const fs::path& dir) {
        std::error_code error;
        if (fs::exists(dir, error) && dir.parent_path() == parent && !fs::is_symlink(dir, error) &&
            (PathUtf8(dir.filename()).starts_with(".pt-install-") || PathUtf8(dir.filename()).starts_with(".pt-update-")))
            fs::remove_all(dir, error);
    };
    try {
        const bool compare = update && steps.entries && steps.fetch && steps.hash_file;
        std::vector<PayloadEntry> new_entries, needed;
        if (compare) {
            const std::set<std::string> unlock = UnlockFiles(destination);
            for (auto& entry : steps.entries()) {
                if (unlock.count(PathKey(entry.path))) continue;
                new_entries.push_back(std::move(entry));
            }
            if (steps.needed) {
                for (const auto& entry : *steps.needed)
                    if (!unlock.count(PathKey(entry.path))) needed.push_back(entry);
            } else {
                Report("Comparing the installed files...");
                needed = DifferingFiles(destination, new_entries, steps.hash_file);
            }
            outcome.unchanged = new_entries.size() - needed.size();
        }
        uint64_t total = 0, archives = 0;
        if (compare) {
            for (const auto& entry : needed) total += entry.size;
        } else if (steps.payload_bytes) {
            total = steps.payload_bytes();
        }
        if (source) {
            auto size = [](const fs::path& file) -> uint64_t {
                std::error_code error;
                const auto bytes = file.empty() ? 0 : fs::file_size(file, error);
                return error ? 0 : bytes;
            };
            archives = source->kind == SourceKind::Package ? size(source->path) : size(source->files.psarc) + size(source->files.qar) + size(source->files.pathid);
            total += archives;
        }
        progress.Begin(std::max<uint64_t>(total, 1));
        Report("Preparing native PC runtime...");
        std::vector<InstalledFile> files, all_files;
        if (compare) {
            files = needed.empty() ? std::vector<InstalledFile>{} : steps.fetch(staging, needed);
            for (const auto& entry : new_entries) all_files.push_back({PathUtf8(Utf8Path(entry.path)), entry.sha256});
            for (const auto& entry : needed) outcome.written_bytes += entry.size;
        } else {
            files = steps.unpack(staging);
            all_files = files;
        }
        ValidateProgramFiles(all_files);
        if (steps.validate_runtime) steps.validate_runtime(all_files);
        if (!update) EnsureDataDirectory(staging, steps.unique_id, steps.check_parents);
        std::vector<std::string> extra;
        if (source) {
            if (source->kind == SourceKind::Package) {
                Report("Extracting your game package...");
                const uint64_t before = progress.done;
                steps.extract(staging, source->path);
                progress.At(before + archives);
            } else {
                CopyArchives(source->files, staging / kGameFolder);
            }
            outcome.notes = WriteNotes(*source, staging);
            outcome.archives_restored = update;
            std::error_code error;
            for (const char* name : {"CUSA01127/chunk1.psarc", "CUSA01127/texture.qar", "CUSA01127/pathid_list_ps4.bin", "CUSA01127/source.txt",
                                     "install-notes.txt", "install-extraction.log"})
                if (fs::is_regular_file(staging / name, error)) extra.push_back(name);
        }
        std::error_code error;
        const fs::path install_root = fs::weakly_canonical(destination, error);
        auto outside_install = [&](const fs::path& path) {
            std::error_code e;
            if (path.empty() || !fs::exists(path, e)) return false;
            const fs::path full = fs::weakly_canonical(fs::absolute(path), e);
            if (e) return false;
            const auto mismatch = std::mismatch(install_root.begin(), install_root.end(), full.begin(), full.end());
            return mismatch.first != install_root.end();
        };
        std::vector<fs::path> game_sources;
        if (source) game_sources.push_back(source->path);
        if (!input.empty() && outside_install(input)) game_sources.push_back(input);
        if (update && !old.source.empty() && outside_install(Utf8Path(old.source))) game_sources.push_back(Utf8Path(old.source));
        std::string recorded = game_sources.empty() ? (update ? old.source : std::string()) : PathUtf8(fs::absolute(game_sources.front()));
        if (!steps.portable) {
            const fs::path picture_path = destination / game_folder_name / "sce_sys" / "pic1.png";
            if (!fs::is_regular_file(picture_path, error)) {
                std::optional<std::string> picture;
                for (const auto& look : game_sources)
                    if ((picture = FindSystemPng(look, "pic1.png"))) break;
                if (!picture) {
                    std::vector<fs::path> folders{destination.parent_path()};
                    if (steps.known_folders)
                        for (auto& folder : steps.known_folders()) folders.push_back(std::move(folder));
                    for (const auto& look : GuessGameSources(folders))
                        if (outside_install(look) && (picture = FindSystemPng(look, "pic1.png"))) {
                            if (recorded.empty()) recorded = PathUtf8(fs::absolute(look));
                            break;
                        }
                }
                if (picture) {
                    fs::create_directories(staging / game_folder_name / "sce_sys", error);
                    std::ofstream picture_file(staging / game_folder_name / "sce_sys" / "pic1.png", std::ios::binary);
                    picture_file.write(picture->data(), std::streamsize(picture->size()));
                    picture_file.close();
                    if (picture_file) extra.push_back(game_folder_name + "/sce_sys/pic1.png");
                }
                outcome.title_picture_missing = !picture;
            }
        }
        if (shortcut) {
            const fs::path look = game_sources.empty() ? fs::path() : game_sources.front();
            if (!look.empty()) {
                if (const auto icon = FindIconPng(look)) {
                    std::ofstream icon_file(staging / "icon0.png", std::ios::binary);
                    icon_file.write(icon->data(), std::streamsize(icon->size()));
                    if (icon_file) extra.push_back("icon0.png");
                }
            }
        }
        if (!steps.portable) WriteManifest(staging, steps.version, all_files, recorded);
        CheckCancel();
        if (!update) {
            if (EmptyDirectory(destination)) fs::remove(destination);
            if (fs::exists(destination)) throw std::runtime_error("Choose a new installation folder. Existing folders are never overwritten.");
            fs::rename(staging, destination);
            if (shortcut) steps.shortcut(destination);
            return outcome;
        }
        if (!steps.portable) extra.push_back(kManifestName);
        if (steps.before_swap) steps.before_swap();
        Report("Updating the program files...");
        outcome.updated = true;
        outcome.had_manifest = old.has_manifest;
        outcome.old_version = old.version;
        if (outcome.old_version.empty() && steps.hash_file && fs::is_regular_file(destination / kGameExe, error)) {
            try {
                outcome.old_version = HistoryVersion(steps.hash_file(destination / kGameExe));
            } catch (const std::exception&) {
            }
        }
        if (outcome.old_version.empty()) outcome.old_version = steps.installed_version;
        std::vector<std::string> shipped_paths;
        for (const auto& file : all_files) shipped_paths.push_back(file.path);
        SwapJournal journal{JournalFile(destination), outcome.old_version, steps.version, steps.download, steps.pid};
        journal.pause_at = steps.pause_at;
        try {
            outcome.swap = SwapProgramFiles(staging, destination, files, extra, old, backup, steps.fail_at, &journal, shipped_paths);
        } catch (const fs::filesystem_error& e) {
            throw std::runtime_error("Could not replace " + PathUtf8(e.path1().filename()) +
                                     ": it is in use (is P.T. still running?). Close it and try again. Nothing was changed.");
        }
        for (const auto& note : outcome.swap.notes) outcome.notes.push_back("frame generation unlock: " + note);
        if (compare) {
            Report("Checking the installed files...");
            std::error_code e;
            if (!steps.corrupt_after_swap.empty()) std::ofstream(Contained(destination, steps.corrupt_after_swap), std::ios::binary | std::ios::trunc) << "damaged";
            auto bad = DifferingFiles(destination, new_entries, steps.hash_file);
            if (!bad.empty()) {
                LogUpdate(destination, std::to_string(bad.size()) + " files differ after the update (first " + bad.front().path + "); fetching them again");
                const fs::path fix = parent / (".pt-update-fix-" + steps.unique_id);
                fs::remove_all(fix, e);
                fs::create_directories(fix);
                try {
                    steps.fetch(fix, bad);
                    for (const auto& entry : bad) {
                        const fs::path target = Contained(destination, entry.path);
                        fs::create_directories(target.parent_path());
                        fs::remove(target, e);
                        MoveFileRetrying(Contained(fix, entry.path), target);
                    }
                } catch (const std::exception& failure) {
                    LogUpdate(destination, std::string("the repair failed: ") + failure.what());
                }
                remove_ours(fix);
                outcome.repaired = bad.size();
                bad = DifferingFiles(destination, new_entries, steps.hash_file);
            }
            if (!bad.empty()) {
                std::string why;
                const auto plan = update_journal::Read(journal.file);
                const bool undone = plan && update_journal::Rollback(*plan, &why);
                if (undone) {
                    update_journal::RemoveFolders(*plan);
                    fs::remove(journal.file, e);
                }
                LogUpdate(destination, "update to " + steps.version + " failed its check: " + std::to_string(bad.size()) + " files differ (first " +
                                           bad.front().path + ")" + (undone ? "; the old version was restored" : "; it could not be undone: " + why));
                throw std::runtime_error(std::to_string(bad.size()) + " files of the new version did not install correctly (first: " + bad.front().path + "). " +
                                         (undone ? "The previous version was restored." : "The next start of the game or the setup restores the previous version."));
            }
            outcome.verified = new_entries.size();
        }
        progress.At(progress.total);
        LogUpdate(destination, "update " + (outcome.old_version.empty() ? std::string(steps.portable ? "of a portable copy" : "of an older install") : outcome.old_version) +
                                   (steps.portable && !outcome.old_version.empty() ? " (a portable copy)" : "") + " to " + steps.version +
                                   ": " + std::to_string(outcome.swap.replaced) + " replaced, " + std::to_string(outcome.swap.added) + " added, " +
                                   std::to_string(outcome.unchanged) + " unchanged, " + std::to_string(outcome.swap.removed) + " removed" +
                                   (compare ? ", all " + std::to_string(outcome.verified) + " files checked" : std::string()) +
                                   (outcome.repaired ? ", " + std::to_string(outcome.repaired) + " fetched again" : std::string()));
        if (steps.keep_backup) {
            update_journal::SetState(journal.file, update_journal::State::Swapped);
            outcome.journal = journal.file;
            return outcome;
        }
        fs::remove(journal.file, error);
        remove_ours(backup);
        remove_ours(staging);
        if (shortcut) steps.shortcut(destination);
        return outcome;
    } catch (...) {
        bool kept = false;
        std::error_code error;
        for (fs::recursive_directory_iterator it(backup, error), end; !error && it != end; it.increment(error))
            if (it->is_regular_file(error)) kept = true;
        if (!kept) {
            remove_ours(staging);
            remove_ours(backup);
        }
        throw;
    }
}
inline std::string UpdateQuestion(const ExistingInstall& old, const std::string& version) {
    if (!old.found) return {};
    if (old.has_manifest && old.version == version)
        return "P.T. version " + version + " is already installed here and up to date. Repair it (check the game files and the program files, and rewrite those that differ)?";
    return "P.T. is already installed here (" + (old.version.empty() ? std::string("an older version") : "version " + old.version) + "). Update it to version " +
           version + "? The game files, settings, saves and anything else you added stay.";
}

inline std::string SelfTestUpdate(const fs::path& root) {
    std::string failures;
    auto write = [](const fs::path& file, const std::string& bytes) {
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << bytes;
    };
    auto read = [](const fs::path& file) {
        std::ifstream in(file, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), {});
    };
    std::string qar(0x40, '\0');
    qar[0x40 - 0x24 + 0x16] = 'a';
    qar[0x40 - 0x24 + 0x17] = 'q';
    auto old_install = [&](const fs::path& dest, bool manifest) {
        write(dest / kGameExe, "old exe");
        write(dest / "shaders" / "a.spv", "old a");
        write(dest / "shaders" / "gone.spv", "old gone");
        write(dest / "nvngx_dlss.dll", "old dll");
        write(dest / "voice" / "cmudict-en-us.dict", "old dictionary");
        write(dest / "voice" / "en-us" / "mdef", "old acoustic model");
        write(dest / "CUSA01127" / "chunk1.psarc", "PSAR old archive");
        write(dest / "CUSA01127" / "texture.qar", qar);
        write(dest / "pt.ini", "player settings");
        write(dest / "data" / "pt.ini", "user folder settings");
        write(dest / "data" / "pt.log", "user folder log");
        write(dest / "data" / "saves" / "progress.dat", "user save");
        write(dest / "mods" / "mine" / "init.lua", "player mod");
        write(dest / "reshade.dll", "player dll");
        if (manifest)
            WriteManifest(dest, "0.0.9", {{kGameExe, std::string(64, '0')}, {"shaders/a.spv", std::string(64, '0')},
                                          {"shaders/gone.spv", std::string(64, '0')}, {"nvngx_dlss.dll", std::string(64, '0')},
                                          {"voice/cmudict-en-us.dict", std::string(64, '0')}, {"voice/en-us/mdef", std::string(64, '0')},
                                          {"data/pt.ini", std::string(64, '0')}
#ifdef _WIN32
                                          , {"data./pt.log", std::string(64, '0')}, {"data /saves/progress.dat", std::string(64, '0')},
                                          {"DATA\\pt.ini", std::string(64, '0')}
#endif
            });
    };
    auto new_staging = [&](const fs::path& staging) {
        write(staging / kGameExe, "new exe");
        write(staging / "shaders" / "a.spv", "new a");
        write(staging / "shaders" / "b.spv", "new b");
        write(staging / kManifestName, "pt-port-install 1\nversion=0.2.0\n");
        return std::vector<InstalledFile>{{kGameExe, ""}, {"shaders/a.spv", ""}, {"shaders/b.spv", ""}};
    };
    for (bool manifest : {true, false}) {
        const std::string label = manifest ? " (manifest)" : " (older install)";
        const fs::path dest = root / (manifest ? "with" : "without") / "PT", staging = dest.parent_path() / ".pt-update-t", backup = dest.parent_path() / ".pt-update-old-t";
        old_install(dest, manifest);
        const ExistingInstall old = InspectInstall(dest);
        if (!old.found || old.has_manifest != manifest || (manifest && old.version != "0.0.9")) failures += " inspect" + label;
        const auto files = new_staging(staging);
        try {
            const auto swap = SwapProgramFiles(staging, dest, files, {kManifestName}, old, backup);
            std::error_code error;
            if (read(dest / kGameExe) != "new exe" || read(dest / "shaders" / "b.spv") != "new b") failures += " new files" + label;
            if (fs::exists(dest / "shaders" / "gone.spv", error) || fs::exists(dest / "nvngx_dlss.dll", error) ||
                fs::exists(dest / "voice" / "cmudict-en-us.dict", error) || fs::exists(dest / "voice" / "en-us" / "mdef", error))
                failures += " obsolete kept" + label;
            if (read(dest / "pt.ini") != "player settings" || read(dest / "mods" / "mine" / "init.lua") != "player mod" ||
                read(dest / "reshade.dll") != "player dll" || read(dest / "CUSA01127" / "chunk1.psarc") != "PSAR old archive" ||
                read(dest / "data" / "pt.ini") != "user folder settings" || read(dest / "data" / "pt.log") != "user folder log" ||
                read(dest / "data" / "saves" / "progress.dat") != "user save")
                failures += " player files" + label;
            if (std::any_of(old.files.begin(), old.files.end(), [](const InstalledFile& file) { return IsUserDataPath(file.path); }))
                failures += " data-classified-as-program" + label;
            if (swap.replaced != (manifest ? 3 : 2) || swap.added != (manifest ? 1 : 2) || swap.removed != 4) failures += " counts" + label;
            if (InspectInstall(dest).version != "0.2.0") failures += " manifest" + label;
        } catch (const std::exception& e) {
            failures += std::string(" swap") + label + ": " + e.what();
        }
    }
    for (int fail_at : {0, 2, 5}) {
        const fs::path dest = root / ("rollback" + std::to_string(fail_at)) / "PT", staging = dest.parent_path() / ".pt-update-t",
                       backup = dest.parent_path() / ".pt-update-old-t";
        old_install(dest, true);
        std::vector<std::pair<std::string, std::string>> before;
        for (const auto& entry : fs::recursive_directory_iterator(dest))
            if (entry.is_regular_file()) before.push_back({PathUtf8(fs::relative(entry.path(), dest)), read(entry.path())});
        const auto files = new_staging(staging);
        bool thrown = false;
        try {
            SwapProgramFiles(staging, dest, files, {kManifestName}, InspectInstall(dest), backup, fail_at);
        } catch (...) {
            thrown = true;
        }
        std::vector<std::pair<std::string, std::string>> after;
        for (const auto& entry : fs::recursive_directory_iterator(dest))
            if (entry.is_regular_file()) after.push_back({PathUtf8(fs::relative(entry.path(), dest)), read(entry.path())});
        std::sort(before.begin(), before.end());
        std::sort(after.begin(), after.end());
        if (!thrown || before != after) failures += " rollback at step " + std::to_string(fail_at);
        if (read(staging / kGameExe) != "new exe") failures += " staging restored at step " + std::to_string(fail_at);
    }
    write(root / "other" / "notes.txt", "x");
    bool refused = false;
    try { InspectInstall(root / "other"); } catch (...) { refused = true; }
    if (!refused) failures += " other-folder-accepted";
    if (InspectInstall(root / "nothing").found) failures += " new-folder";
    {
        std::error_code error;
        fs::create_directories(root / "empty", error);
        try {
            if (InspectInstall(root / "empty").found) failures += " empty-folder-as-install";
        } catch (...) { failures += " empty-folder-refused"; }
        write(root / "result.txt", "PASS old");
        if (!ResultPathWritable(root / "result.txt") || !ResultPathWritable(root / "new-result.txt")) failures += " result-refused";
        write(root / "big.bin", std::string(128 * 1024, 'x'));
        if (ResultPathWritable(root / "big.bin") || ResultPathWritable(root / "empty")) failures += " result-overwrites-user-file";
    }
    {
        const fs::path dest = root / "reserved-data" / "PT", staging = root / "reserved-data" / ".pt-update-t",
                       backup = root / "reserved-data" / ".pt-update-old-t";
        old_install(dest, true);
        write(staging / "data" / "pt.log", "payload overwrite");
        bool rejected_data = false;
        try {
            SwapProgramFiles(staging, dest, {{"data/pt.log", ""}}, {}, InspectInstall(dest), backup);
        } catch (...) {
            rejected_data = true;
        }
        if (!rejected_data || read(dest / "data" / "pt.log") != "user folder log") failures += " payload-data-overwrite-accepted";
#ifdef _WIN32
        for (const std::string& alias : {"data./pt.log", "data /pt.log", "DATA\\pt.log"}) {
            bool rejected_alias = false;
            try {
                SwapProgramFiles(staging, dest, {{alias, ""}}, {}, InspectInstall(dest), backup);
            } catch (...) {
                rejected_alias = true;
            }
            if (!rejected_alias || read(dest / "data" / "pt.log") != "user folder log")
                failures += " payload-data-alias-overwrite-accepted";
        }
        for (const std::string& alias : {"DATA\\pt.log", "data./pt.log", "data /pt.log"})
            if (!IsUserDataPath(alias)) failures += " data-alias-not-reserved";
#endif
    }
    {
        const fs::path writable = root / "write-probe";
        fs::create_directories(writable);
        try {
            ProbeWritableDirectory(writable, "selftest", "test folder");
            if (fs::exists(writable / ".pt-write-probe-selftest")) failures += " write-probe-left-files";
        } catch (const std::exception& e) {
            failures += std::string(" write-probe-writable-folder: ") + e.what();
        }
        write(root / "not-a-folder", "x");
        bool refused_probe = false;
        try { ProbeWritableDirectory(root / "not-a-folder", "selftest", "test folder"); } catch (...) { refused_probe = true; }
        if (!refused_probe) failures += " write-probe-accepted-file";
        const fs::path custom_install = root / "custom install location" / "PT";
        fs::create_directories(custom_install);
        auto no_links = [](const fs::path&) {};
        try {
            EnsureDataDirectory(custom_install, "custom-selftest", no_links);
            write(custom_install / "data" / "pt.log", "player log");
            EnsureDataDirectory(custom_install, "custom-selftest-2", no_links);
            if (read(custom_install / "data" / "pt.log") != "player log") failures += " custom-data-not-preserved";
        } catch (const std::exception& e) {
            failures += std::string(" custom-data-directory: ") + e.what();
        }
    }
    {
        const std::vector<InstalledFile> complete{{"amd_fidelityfx_vk.dll", ""}, {"nvngx_dlss.dll", ""}, {"libxess.dll", ""}};
        const std::vector<std::string> required{"amd_fidelityfx_vk.dll", "nvngx_dlss.dll", "libxess.dll"};
        try { RequireProgramFiles(complete, required); } catch (...) { failures += " complete-upscalers-rejected"; }
        bool rejected_missing = false;
        try { RequireProgramFiles({{"amd_fidelityfx_vk.dll", ""}, {"libxess.dll", ""}}, required); } catch (...) { rejected_missing = true; }
        if (!rejected_missing) failures += " missing-upscaler-accepted";
    }
    if (UpdateQuestion(InspectInstall(root / "with" / "PT"), "0.2.0").find("up to date") == std::string::npos) failures += " same-version-question";
    return failures;
}
inline std::string MakeTestPayload(const fs::path& scratch, const std::vector<std::pair<std::string, std::string>>& files,
                                   const std::function<std::string(const fs::path&)>& hash_file) {
    std::string out = "PTSETUP1";
    auto u16 = [&](uint16_t v) { out.append(reinterpret_cast<const char*>(&v), 2); };
    auto u32 = [&](uint32_t v) { out.append(reinterpret_cast<const char*>(&v), 4); };
    auto u64 = [&](uint64_t v) { out.append(reinterpret_cast<const char*>(&v), 8); };
    u32(uint32_t(files.size()));
    fs::create_directories(scratch);
    for (const auto& [name, bytes] : files) {
        {
            std::ofstream(scratch / "hash.bin", std::ios::binary | std::ios::trunc) << bytes;
        }
        const std::string digest = hash_file(scratch / "hash.bin");
        uLongf packed_size = compressBound(uLong(bytes.size()));
        std::string packed(packed_size, '\0');
        compress(reinterpret_cast<Bytef*>(packed.data()), &packed_size, reinterpret_cast<const Bytef*>(bytes.data()), uLong(bytes.size()));
        packed.resize(packed_size);
        u16(uint16_t(name.size()));
        out += name;
        u64(bytes.size());
        u64(packed.size());
        out += digest;
        out += packed;
    }
    return out;
}
inline std::string TestPng() {
    return std::string("\x89PNG\r\n\x1a\n\x00\x00\x00\x0dIHDR\x00\x00\x00\x01\x00\x00\x00\x01\x08\x02\x00\x00\x00\x90wS\xde\x00\x00\x00\x0cIDAT\x78\x9c\x63\xf8\xcf\xc0\x00\x00\x00\x03\x01\x01\x00\xc9\xfe\x92\xef\x00\x00\x00\x00IEND\xae\x42\x60\x82",
                       69);
}
inline std::string TestParamSfo(const std::string& title, const std::string& content_id = {}) {
    std::vector<std::pair<std::string, std::string>> items;
    if (!content_id.empty()) items.push_back({"CONTENT_ID", content_id});
    items.push_back({"TITLE_ID", title});
    std::string keys, values, entries;
    auto u16 = [&](std::string& out, uint16_t v) { out.append(reinterpret_cast<const char*>(&v), 2); };
    auto u32 = [&](std::string& out, uint32_t v) { out.append(reinterpret_cast<const char*>(&v), 4); };
    for (const auto& [key, value] : items) {
        u16(entries, uint16_t(keys.size()));
        u16(entries, 0x0204);
        u32(entries, uint32_t(value.size() + 1));
        u32(entries, 64);
        u32(entries, uint32_t(values.size()));
        keys += key;
        keys += '\0';
        values += value;
        values.resize(values.size() + 64 - value.size(), '\0');
    }
    std::string sfo("\0PSF\x01\x01\x00\x00", 8);
    const uint32_t key_table = uint32_t(0x14 + entries.size());
    u32(sfo, key_table);
    u32(sfo, key_table + uint32_t((keys.size() + 3) / 4 * 4));
    u32(sfo, uint32_t(items.size()));
    sfo += entries;
    sfo += keys;
    sfo.resize(key_table + (keys.size() + 3) / 4 * 4, '\0');
    sfo += values;
    return sfo;
}
inline std::string SelfTestTitlePictureUpdate(const fs::path& root, const std::function<std::string(const fs::path&)>& hash_file) {
    std::string failures;
    auto write = [](const fs::path& file, const std::string& bytes) {
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << bytes;
    };
    auto read = [](const fs::path& file) {
        std::ifstream in(file, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), {});
    };
    std::string qar(0x40, '\0');
    qar[0x40 - 0x24 + 0x16] = 'a';
    qar[0x40 - 0x24 + 0x17] = 'q';
    auto old_install = [&](const fs::path& dest) {
        write(dest / kGameExe, "old exe");
        write(dest / "shaders" / "gone.spv", "old shader");
        write(dest / "CUSA01127" / "chunk1.psarc", "PSAR old archive");
        write(dest / "CUSA01127" / "texture.qar", qar);
        write(dest / "data" / "pt.ini", "settings");
        write(dest / "data" / "saves" / "progress.dat", "save");
        write(dest / "mods" / "mine" / "init.lua", "player mod");
        WriteManifest(dest, "1.0.3", {{kGameExe, std::string(64, '0')}, {"shaders/gone.spv", std::string(64, '0')},
                                      {"mods/mine/init.lua", std::string(64, '0')}});
    };
    const std::string payload = MakeTestPayload(root / "payload-scratch", {{kGameExe, "new exe"}, {"shaders/new.spv", "new shader"}}, hash_file);
    auto steps = [&](const std::string& id) {
        InstallSteps steps;
        steps.version = "9.9.9";
        steps.unique_id = id;
        steps.check_parents = [](const fs::path&) {};
        steps.verify_integrity = [] {};
        steps.payload_bytes = [&] { return PayloadBytes(reinterpret_cast<const unsigned char*>(payload.data()), payload.size()); };
        steps.unpack = [&](const fs::path& staging) {
            return UnpackPayload(reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), staging, hash_file);
        };
        steps.extract = [](const fs::path&, const fs::path&) { throw std::runtime_error("no package in this test"); };
        steps.shortcut = [](const fs::path&) {};
        steps.known_folders = [] { return std::vector<fs::path>{}; };
        return steps;
    };
    const fs::path dump = root / "elsewhere" / "deep" / "dump" / "CUSA01127-app";
    write(dump / "sce_sys" / "pic1.png", TestPng());
    write(dump / "sce_sys" / "param.sfo", TestParamSfo("CUSA01127"));
    const fs::path dest = root / "games" / "P.T. PC Port", picture = dest / "CUSA01127" / "sce_sys" / "pic1.png";
    old_install(dest);
    try {
        auto outcome = RunInstall({}, dest, false, steps("a"));
        if (!outcome.updated) failures += " title-update-not-update";
        if (fs::exists(picture) || !outcome.title_picture_missing) failures += " title-empty-field";
        if (read(dest / kGameExe) != "new exe" || read(dest / "shaders" / "new.spv") != "new shader") failures += " title-new-files";
        if (fs::exists(dest / "shaders" / "gone.spv")) failures += " title-obsolete-kept";
        if (read(dest / "mods" / "mine" / "init.lua") != "player mod" || read(dest / "data" / "pt.ini") != "settings" ||
            read(dest / "data" / "saves" / "progress.dat") != "save")
            failures += " title-player-files";
        if (fs::exists(JournalFile(dest))) failures += " title-journal-left";
        outcome = RunInstall(dest / "CUSA01127", dest, false, steps("b"));
        if (fs::exists(picture) || !InspectInstall(dest).source.empty()) failures += " title-own-folder";
        outcome = RunInstall(dump, dest, false, steps("c"));
        if (read(picture) != TestPng() || outcome.title_picture_missing) failures += " title-dump";
        if (fs::weakly_canonical(Utf8Path(InspectInstall(dest).source)) != fs::weakly_canonical(dump)) failures += " title-source-not-recorded";
        fs::remove(picture);
        outcome = RunInstall({}, fs::path(dest.native() + fs::path::preferred_separator), false, steps("d"));
        if (read(picture) != TestPng()) failures += " title-recorded-source";
        for (const auto& entry : fs::directory_iterator(dest))
            if (PathUtf8(entry.path().filename()).starts_with(".pt-")) failures += " title-work-folder-inside-install";
        if (InspectInstall(dest).version != "9.9.9") failures += " title-version";
    } catch (const std::exception& e) {
        failures += std::string(" title-update: ") + e.what();
    }
    try {
        const fs::path side = root / "side" / "P.T. PC Port";
        old_install(side);
        write(root / "side" / "my-dump" / "sce_sys" / "pic1.png", TestPng());
        write(root / "side" / "my-dump" / "sce_sys" / "param.sfo", TestParamSfo("CUSA01127"));
        write(root / "side" / "other-game" / "sce_sys" / "pic1.png", "not ours");
        write(root / "side" / "other-game" / "sce_sys" / "param.sfo", TestParamSfo("CUSA99999"));
        const auto outcome = RunInstall({}, side, false, steps("e"));
        if (read(side / "CUSA01127" / "sce_sys" / "pic1.png") != TestPng() || outcome.title_picture_missing) failures += " title-guess";
    } catch (const std::exception& e) {
        failures += std::string(" title-guess: ") + e.what();
    }
    try {
        const fs::path eu = root / "eu" / "P.T. PC Port";
        old_install(eu);
        fs::rename(eu / "CUSA01127", eu / "CUSA01114");
        const fs::path eu_dump = root / "eu" / "dump" / "my-eu-copy";
        write(eu_dump / "chunk1.psarc", "PSAR dump archive");
        write(eu_dump / "texture.qar", qar);
        write(eu_dump / "sce_sys" / "pic1.png", TestPng());
        write(eu_dump / "sce_sys" / "param.sfo", TestParamSfo("CUSA01114", "EP4511-CUSA01114_00-PPPPPPPPTTTTTTTT"));
        const auto outcome = RunInstall(eu_dump, eu, false, steps("f"));
        if (!outcome.updated || outcome.archives_restored) failures += " region-eu-archives-restored";
        if (read(eu / "CUSA01114" / "chunk1.psarc") != "PSAR old archive") failures += " region-eu-archives-changed";
        if (fs::exists(eu / "CUSA01127")) failures += " region-eu-stray-folder";
        if (read(eu / "CUSA01114" / "sce_sys" / "pic1.png") != TestPng()) failures += " region-eu-picture";
        if (read(eu / "data" / "pt.ini") != "settings" || read(eu / "mods" / "mine" / "init.lua") != "player mod") failures += " region-eu-player-files";
        const auto again = RunInstall({}, eu, false, steps("g"));
        if (!again.updated || again.archives_restored || fs::exists(eu / "CUSA01127") || read(eu / "CUSA01114" / "chunk1.psarc") != "PSAR old archive")
            failures += " region-eu-update-without-source";
        const Source found = ResolveSource(eu_dump);
        if (found.files.title != "CUSA01114" || found.files.content_id != "EP4511-CUSA01114_00-PPPPPPPPTTTTTTTT" ||
            RegionOf(found.files.title, found.files.content_id) != "Europe")
            failures += " region-eu-identify";
        write(eu_dump / "pathid_list_ps4.bin", "/Assets/sh/level/pt14_hallway/" + std::string(40, 'x'));
        write(eu_dump / "sce_sys" / "param.sfo", TestParamSfo("CUSA09999", "HP4511-CUSA09999_00-PPPPPPPPTTTTTTTT"));
        const Source other = ResolveSource(eu_dump);
        if (RegionOf(other.files.title, other.files.content_id) != "Asia") failures += " region-other-identify";
        CopyArchives(other.files, root / "eu" / "copy");
        if (read(root / "eu" / "copy" / "source.txt").find("content_id=HP4511-CUSA09999_00-PPPPPPPPTTTTTTTT\ntitle_id=CUSA09999\nregion=Asia") == std::string::npos)
            failures += " region-other-source";
        if (!IsPlayerPath("CUSA01114/chunk1.psarc") || !IsPlayerPath("CUSA01114-app/") || !IsPlayerPath("cusa01098/texture.qar") || IsPlayerPath("CUSA01114.txt") ||
            IsPlayerPath("shaders/a.spv"))
            failures += " region-player-path";
    } catch (const std::exception& e) {
        failures += std::string(" region: ") + e.what();
    }
    std::error_code error;
    fs::remove_all(root, error);
    return failures;
}

inline std::string SelfTestJournalRecovery(const fs::path& root) {
    std::string failures;
    auto write = [](const fs::path& file, const std::string& bytes) {
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << bytes;
    };
    auto snapshot = [](const fs::path& dir) {
        std::vector<std::pair<std::string, std::string>> files;
        for (const auto& entry : fs::recursive_directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            std::ifstream in(entry.path(), std::ios::binary);
            files.push_back({PathUtf8(fs::relative(entry.path(), dir)), std::string((std::istreambuf_iterator<char>(in)), {})});
        }
        std::sort(files.begin(), files.end());
        return files;
    };
    auto setup = [&](const fs::path& dest, const fs::path& staging) {
        write(dest / kGameExe, "old exe");
        write(dest / "shaders" / "a.spv", "old a");
        write(dest / "shaders" / "gone.spv", "old gone");
        write(dest / "data" / "pt.ini", "settings");
        WriteManifest(dest, "1.0.3", {{kGameExe, std::string(64, '0')}, {"shaders/a.spv", std::string(64, '0')}, {"shaders/gone.spv", std::string(64, '0')}});
        write(staging / kGameExe, "new exe");
        write(staging / "shaders" / "a.spv", "new a");
        write(staging / "shaders" / "b.spv", "new b");
        WriteManifest(staging, "9.9.9", {});
        return std::vector<InstalledFile>{{kGameExe, ""}, {"shaders/a.spv", ""}, {"shaders/b.spv", ""}};
    };
    for (int crash_at : {0, 1, 3, 4, 5}) {
        const fs::path parent = root / ("crash" + std::to_string(crash_at)), dest = parent / "PT", staging = parent / ".pt-update-t",
                       backup = parent / ".pt-update-old-t";
        const auto files = setup(dest, staging);
        const auto before = snapshot(dest);
        SwapJournal journal{JournalFile(dest), "1.0.3", "9.9.9", {}, 0, crash_at};
        bool crashed = false;
        try {
            SwapProgramFiles(staging, dest, files, {kManifestName}, InspectInstall(dest), backup, -1, &journal);
        } catch (const SimulatedCrash&) {
            crashed = true;
        } catch (const std::exception& e) {
            failures += std::string(" journal-swap: ") + e.what();
        }
        if (!crashed || !fs::exists(JournalFile(dest))) {
            failures += " journal-not-left-at-" + std::to_string(crash_at);
            continue;
        }
        try {
            if (!RecoverEarlierUpdate(dest, [](uint64_t) { return false; })) failures += " journal-not-recovered-at-" + std::to_string(crash_at);
        } catch (const std::exception& e) {
            failures += " journal-recover-at-" + std::to_string(crash_at) + ": " + e.what();
        }
        auto after = snapshot(dest);
        if (after != before) failures += " journal-restore-at-" + std::to_string(crash_at);
        if (fs::exists(JournalFile(dest)) || fs::exists(backup) || fs::exists(staging)) failures += " journal-leftovers-at-" + std::to_string(crash_at);
    }
    {
        const fs::path parent = root / "kept", dest = parent / "PT", staging = parent / ".pt-update-t", backup = parent / ".pt-update-old-t";
        const auto files = setup(dest, staging);
        const auto before = snapshot(dest);
        SwapJournal journal{JournalFile(dest), "1.0.3", "9.9.9", {}, 0, -1};
        try {
            SwapProgramFiles(staging, dest, files, {kManifestName}, InspectInstall(dest), backup, -1, &journal);
            update_journal::SetState(journal.file, update_journal::State::Swapped);
            const auto read = update_journal::Read(journal.file);
            if (!read || read->state != update_journal::State::Swapped || read->steps.size() != 5) failures += " journal-swapped";
            std::string why;
            if (!read || !update_journal::Rollback(*read, &why)) failures += " journal-rollback: " + why;
            if (read) update_journal::RemoveFolders(*read);
            fs::remove(journal.file);
            auto restored = snapshot(dest);
            if (restored != before) failures += " journal-rollback-content";
        } catch (const std::exception& e) {
            failures += std::string(" journal-kept: ") + e.what();
        }
        fs::remove_all(parent);
        const auto files2 = setup(dest, staging);
        SwapProgramFiles(staging, dest, files2, {kManifestName}, InspectInstall(dest), backup, -1, &journal);
        update_journal::SetState(journal.file, update_journal::State::Started);
        RecoverEarlierUpdate(dest, {});
        std::ifstream exe(dest / kGameExe, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(exe)), {});
        if (text != "new exe" || fs::exists(backup) || fs::exists(JournalFile(dest))) failures += " journal-started-finish";
    }
    {
        const fs::path dest = root / "evil" / "PT";
        write(dest / kGameExe, "exe");
        auto attempt = [&](const std::string& text) {
            write(JournalFile(dest), text);
            return update_journal::Read(JournalFile(dest)).has_value();
        };
        const std::string install = update_journal::ToUtf8(fs::absolute(dest));
        const std::string parent = update_journal::ToUtf8(fs::absolute(dest.parent_path()));
        const std::string good = "pt-update-journal 1\nstate=swapping\ninstall=" + install + "\nstaging=" + parent + "/.pt-update-x\nbackup=" + parent +
                                 "/.pt-update-old-x\nstep=r pt.exe\n";
        if (!attempt(good)) failures += " journal-good-refused";
        if (attempt(good + "step=r ../outside.txt\n")) failures += " journal-dotdot-accepted";
        if (attempt("pt-update-journal 1\nstate=swapping\ninstall=" + install + "\nstaging=" + parent + "/elsewhere\nbackup=" + parent +
                    "/.pt-update-old-x\nstep=r pt.exe\n"))
            failures += " journal-foreign-staging-accepted";
        if (attempt("pt-update-journal 1\nstate=swapping\ninstall=" + parent + "\nstaging=" + parent + "/.pt-update-x\nbackup=" + parent +
                    "/.pt-update-old-x\n"))
            failures += " journal-foreign-install-accepted";
    }
    std::error_code error;
    fs::remove_all(root, error);
    return failures;
}
inline std::string SelfTestFileUpdate(const fs::path& root, const std::function<std::string(const fs::path&)>& hash_file) {
    std::string failures;
    auto write = [](const fs::path& file, const std::string& bytes) {
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary | std::ios::trunc) << bytes;
    };
    auto read = [](const fs::path& file) {
        std::ifstream in(file, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), {});
    };
    auto tree = [&](const fs::path& dir, const std::function<bool(const std::string&)>& skip) {
        std::vector<std::pair<std::string, std::string>> out;
        std::error_code error;
        for (fs::recursive_directory_iterator it(dir, error), end; !error && it != end; it.increment(error)) {
            std::string path = PathUtf8(fs::relative(it->path(), dir));
            if (it->is_directory(error)) path += "/";
            if (skip && skip(path)) continue;
            out.push_back({path, it->is_regular_file(error) ? read(it->path()) : std::string()});
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    std::string qar(0x40, '\0');
    qar[0x40 - 0x24 + 0x16] = 'a';
    qar[0x40 - 0x24 + 0x17] = 'q';
    const std::vector<std::pair<std::string, std::string>> new_files{
        {kGameExe, "new exe"},
        {"shaders/same.spv", "same shader"},
        {"shaders/changed.spv", "changed shader, new"},
        {"voice/models/new.bin", "a new voice model"},
#ifdef _WIN32
        {"Fonts/Case.ttf", "a font"},
#endif
        {"reshade-shaders/x.fx", "the package's copy of a file the unlock owns"},
        {kFileListName, "{\"format\": 1, \"files\": []}\n"},
    };
    const size_t kShipped = new_files.size() - 1;
#ifdef _WIN32
    const size_t kUnchanged = 2;
#else
    const size_t kUnchanged = 1;
#endif
    const std::string payload = MakeTestPayload(root / "payload-scratch", new_files, hash_file);
    auto steps = [&](const std::string& id) {
        InstallSteps steps;
        steps.version = "9.9.9";
        steps.unique_id = id;
        steps.check_parents = [](const fs::path&) {};
        steps.verify_integrity = [] {};
        const auto* data = reinterpret_cast<const unsigned char*>(payload.data());
        const size_t size = payload.size();
        steps.payload_bytes = [=] { return PayloadBytes(data, size); };
        steps.unpack = [=](const fs::path& staging) { return UnpackPayload(data, size, staging, hash_file); };
        steps.entries = [=] { return ListPayload(data, size); };
        steps.hash_file = hash_file;
        steps.fetch = [=](const fs::path& folder, const std::vector<PayloadEntry>& wanted) {
            std::set<std::string> names;
            for (const auto& entry : wanted) names.insert(entry.path);
            return UnpackPayload(data, size, folder, hash_file, [&names](const std::string& name) { return names.count(name) > 0; });
        };
        steps.extract = [](const fs::path&, const fs::path&) { throw std::runtime_error("no package in this test"); };
        steps.shortcut = [](const fs::path&) {};
        steps.known_folders = [] { return std::vector<fs::path>{}; };
        return steps;
    };
    const fs::path dump = root / "dump" / "CUSA01127";
    write(dump / "chunk1.psarc", "PSAR" + std::string(60, '\0'));
    write(dump / "texture.qar", qar);
    write(dump / "pathid_list_ps4.bin", "/Assets/sh/level/pt14_hallway/" + std::string(40, 'x'));
    auto old_install = [&](const fs::path& dest, bool manifest) {
        write(dest / kGameExe, "old exe");
        write(dest / "shaders" / "same.spv", "same shader");
        write(dest / "shaders" / "changed.spv", "changed shader, old");
#ifdef _WIN32
        write(dest / "fonts" / "Case.ttf", "a font");
        write(dest / "fonts" / "NotoSansArabic.ttf", "an old font");
#else
        write(dest / "loop-previews" / "pipeline-cache" / "vulkan-pipelines-v1-10de-2204-9a438000-93577e3653291eb7b40aab325793207e.bin", "1.0.2's stray cache");
#endif
        if (manifest) write(dest / "loop-previews" / "pipeline-cache" / "old.bin", "a cache an old package shipped");
        write(dest / "README.md", "an old release's readme, in no manifest");
        write(dest / "CUSA01127" / "chunk1.psarc", "PSAR" + std::string(60, '\0'));
        write(dest / "CUSA01127" / "texture.qar", qar);
        write(dest / "CUSA01127" / "pathid_list_ps4.bin", "/Assets/sh/level/pt14_hallway/" + std::string(40, 'x'));
        write(dest / "data" / "pt.ini", "settings");
        write(dest / "data" / "saves" / "progress.dat", "save");
        write(dest / "data" / "logs" / "pt.log", "log");
        write(dest / "enhanced-textures" / "cache" / "a.dds", "generated texture");
        write(dest / "mods" / "mine" / "init.lua", "player mod");
        write(dest / "pt.ini", "old layout settings");
        write(dest / "notes-of-the-player.txt", "the player's own file");
        write(dest / "dxgi.dll", "reshade");
        write(dest / "reshade-shaders" / "x.fx", "the unlock's shader");
        write(dest / "dlss5-autopilot.json", "{\"complete\": true, \"route\": \"native\", \"files\": [\"dxgi.dll\", \"reshade-shaders/x.fx\"]}");
        if (manifest)
            WriteManifest(dest, "1.0.3", {{kGameExe, std::string(64, '0')}, {"shaders/same.spv", std::string(64, '0')}, {"shaders/changed.spv", std::string(64, '0')},
                                          {"fonts/Case.ttf", std::string(64, '0')}, {"loop-previews/pipeline-cache/old.bin", std::string(64, '0')},
                                          {"dxgi.dll", std::string(64, '0')}});
    };
    auto player = [](const std::string& path) {
        return IsPlayerPath(path) || path == "data/" || path == "notes-of-the-player.txt" || path == "dxgi.dll" || path == "dlss5-autopilot.json" ||
               path == "reshade-shaders/" || path == "reshade-shaders/x.fx" || path == "cusa01127/" || path == kManifestName ||
               Lower(path).starts_with("cusa01127") || path == "install-notes.txt" || path == "enhanced-textures/" || path == "mods/";
    };
    try {
        const fs::path fresh = root / "fresh" / "PT";
        RunInstall(dump, fresh, false, steps("fresh"));
        for (bool manifest : {true, false}) {
            const std::string label = manifest ? "" : "-no-manifest";
            const fs::path dest = root / ("old" + label) / "PT";
            old_install(dest, manifest);
            const auto same_time = fs::last_write_time(dest / "shaders" / "same.spv");
            const auto outcome = RunInstall({}, dest, false, steps("u" + label));
            if (!outcome.updated || outcome.unchanged != kUnchanged || outcome.verified != kShipped || outcome.repaired != 0)
                failures += " files-counts" + label + "(" + std::to_string(outcome.unchanged) + "/" + std::to_string(outcome.verified) + ")";
            if (fs::last_write_time(dest / "shaders" / "same.spv") != same_time) failures += " files-identical-rewritten" + label;
            if (fs::exists(dest / "loop-previews") || fs::exists(dest / "README.md") || fs::exists(dest / "fonts" / "NotoSansArabic.ttf"))
                failures += " files-stale-kept" + label;
            if (read(dest / "dxgi.dll") != "reshade" || read(dest / "reshade-shaders" / "x.fx") != "the unlock's shader") failures += " files-unlock" + label;
            if (read(dest / "data" / "pt.ini") != "settings" || read(dest / "data" / "saves" / "progress.dat") != "save" || read(dest / "pt.ini") != "old layout settings" ||
                read(dest / "enhanced-textures" / "cache" / "a.dds") != "generated texture" || read(dest / "mods" / "mine" / "init.lua") != "player mod" ||
                read(dest / "notes-of-the-player.txt") != "the player's own file" || read(dest / "data" / "logs" / "pt.log") != "log")
                failures += " files-player" + label;
#ifdef _WIN32
            if (read(dest / "Fonts" / "Case.ttf") != "a font") failures += " files-letter-case" + label;
#endif
            auto lower_skip = [&](const std::string& path) { return player(Lower(path)) || player(path); };
            auto a = tree(fresh, lower_skip), b = tree(dest, lower_skip);
#ifdef _WIN32
            for (auto* t : {&a, &b})
                for (auto& [path, bytes] : *t) path = Lower(path);
            std::sort(a.begin(), a.end());
            std::sort(b.begin(), b.end());
#endif
            if (a != b) {
                failures += " files-tree" + label + ":";
                for (const auto& [path, bytes] : b)
                    if (std::find(a.begin(), a.end(), std::pair{path, bytes}) == a.end()) failures += " +" + path;
                for (const auto& [path, bytes] : a)
                    if (std::find(b.begin(), b.end(), std::pair{path, bytes}) == b.end()) failures += " -" + path;
            }
            if (InspectInstall(dest).files.size() != kShipped) failures += " files-manifest" + label;
        }
        const fs::path dest = root / "old" / "PT";
        const auto exe_time = fs::last_write_time(dest / kGameExe);
        auto again = RunInstall({}, dest, false, steps("repair"));
        if (again.unchanged != kShipped || again.swap.replaced != 1 || again.swap.added != 0 || fs::last_write_time(dest / kGameExe) != exe_time) failures += " files-repair";
        write(dest / "shaders" / "changed.spv", "changed shader, old");
        auto damaged = steps("damaged");
        damaged.corrupt_after_swap = "voice/models/new.bin";
        const auto fixed = RunInstall({}, dest, false, damaged);
        if (fixed.repaired != 1 || read(dest / "voice" / "models" / "new.bin") != "a new voice model") failures += " files-check-repair";
        if (fs::exists(root / "old" / ".pt-update-fix-damaged")) failures += " files-fix-folder-left";
        for (int crash : {0, 1}) {
            const fs::path undo = root / ("undo" + std::to_string(crash)) / "PT";
            old_install(undo, true);
            const auto before = tree(undo, {});
            if (crash) {
                const fs::path staging = undo.parent_path() / ".pt-update-c", backup = undo.parent_path() / ".pt-update-old-c";
                for (const auto& [name, bytes] : new_files) write(staging / Utf8Path(name), bytes);
                WriteManifest(staging, "9.9.9", {});
                std::vector<InstalledFile> files;
                for (const auto& [name, bytes] : new_files) files.push_back({name, ""});
                SwapJournal journal{JournalFile(undo), "1.0.3", "9.9.9", {}, 0, 4};
                try {
                    SwapProgramFiles(staging, undo, files, {kManifestName}, InspectInstall(undo), backup, -1, &journal);
                } catch (const SimulatedCrash&) {
                }
                RecoverEarlierUpdate(undo, [](uint64_t) { return false; });
            } else {
                auto failing = steps("f");
                failing.fail_at = 4;
                try {
                    RunInstall({}, undo, false, failing);
                    failures += " files-fail-not-thrown";
                } catch (const std::exception&) {
                }
            }
            if (tree(undo, {}) != before) failures += crash ? " files-crash-undo" : " files-fail-undo";
            for (const auto& entry : fs::directory_iterator(undo.parent_path()))
                if (entry.path() != undo) failures += " files-left-" + PathUtf8(entry.path().filename());
        }
    } catch (const std::exception& e) {
        failures += std::string(" files-update: ") + e.what();
    }
    std::error_code error;
    fs::remove_all(root, error);
    return failures;
}
inline std::string SelfTestPortableUpdate(const fs::path& root, const std::function<std::string(const fs::path&)>& hash_file) {
    std::string failures;
    auto write = [](const fs::path& file, const std::string& bytes) {
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary | std::ios::trunc) << bytes;
    };
    auto read = [](const fs::path& file) {
        std::ifstream in(file, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), {});
    };
    auto tree = [&](const fs::path& dir, const std::function<bool(const std::string&)>& skip) {
        std::vector<std::pair<std::string, std::string>> out;
        std::error_code error;
        for (fs::recursive_directory_iterator it(dir, error), end; !error && it != end; it.increment(error)) {
            std::string path = PathUtf8(fs::relative(it->path(), dir));
            if (it->is_directory(error)) path += "/";
            if (skip && skip(path)) continue;
            out.push_back({path, it->is_regular_file(error) ? read(it->path()) : std::string()});
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    auto list = [](const std::vector<std::pair<std::string, std::string>>& files) {
        std::string text = "{\"format\": 1, \"files\": [";
        for (size_t i = 0; i < files.size(); ++i)
            text += (i ? ", " : "") + std::string("{\"path\": \"") + files[i].first + "\", \"size\": " + std::to_string(files[i].second.size()) +
                    ", \"sha256\": \"" + std::string(64, '0') + "\"}";
        return text + "]}\n";
    };
    std::string qar(0x40, '\0');
    qar[0x40 - 0x24 + 0x16] = 'a';
    qar[0x40 - 0x24 + 0x17] = 'q';
    std::vector<std::pair<std::string, std::string>> new_files{
        {kGameExe, "new exe"}, {"shaders/same.spv", "same shader"}, {"shaders/changed.spv", "changed shader, new"}, {"voice/models/new.bin", "new model"}};
    new_files.push_back({kFileListName, list(new_files)});
    const std::vector<std::pair<std::string, std::string>> old_files{
        {kGameExe, "old exe"}, {"shaders/same.spv", "same shader"}, {"shaders/changed.spv", "changed shader, old"}, {"licenses/OLD.txt", "an old licence"}};
    const std::string payload = MakeTestPayload(root / "payload-scratch", new_files, hash_file);
    const auto* data = reinterpret_cast<const unsigned char*>(payload.data());
    const size_t size = payload.size();
    auto steps = [&](const std::string& id) {
        InstallSteps steps;
        steps.version = "9.9.9";
        steps.unique_id = id;
        steps.check_parents = [](const fs::path&) {};
        steps.verify_integrity = [] {};
        steps.payload_bytes = [=] { return PayloadBytes(data, size); };
        steps.unpack = [=](const fs::path& staging) { return UnpackPayload(data, size, staging, hash_file); };
        steps.entries = [=] { return ListPayload(data, size); };
        steps.hash_file = hash_file;
        steps.fetch = [=](const fs::path& folder, const std::vector<PayloadEntry>& wanted) {
            std::set<std::string> names;
            for (const auto& entry : wanted) names.insert(entry.path);
            return UnpackPayload(data, size, folder, hash_file, [&names](const std::string& name) { return names.count(name) > 0; });
        };
        steps.extract = [](const fs::path&, const fs::path&) { throw std::runtime_error("no package in this test"); };
        steps.shortcut = [](const fs::path&) {};
        steps.known_folders = [] { return std::vector<fs::path>{}; };
        steps.portable = true;
        steps.installed_version = "9.9.8";
        return steps;
    };
    auto old_copy = [&](const fs::path& dest) {
        for (const auto& [name, bytes] : old_files) write(dest / Utf8Path(name), bytes);
        write(dest / kFileListName, list(old_files));
        write(dest / "README.md", "a readme every published setup installed");
        write(dest / "extractor" / "chunk1.psarc", "PSAR" + std::string(60, '\0'));
        write(dest / "extractor" / "texture.qar", qar);
        write(dest / "extractor" / "sce_sys" / "pic1.png", "the player's title picture");
        write(dest / "data" / "pt.ini", "settings");
        write(dest / "data" / "saves" / "progress.dat", "save");
        write(dest / "mods" / "mine" / "init.lua", "player mod");
        write(dest / "enhanced-textures" / "cache" / "a.dds", "generated texture");
        write(dest / "notes-of-the-player.txt", "the player's own file");
        write(dest / "dxgi.dll", "reshade");
        write(dest / "dlss5-autopilot.json", "{\"complete\": true, \"route\": \"native\", \"files\": [\"dxgi.dll\"]}");
    };
    auto player = [](const std::string& path) {
        const std::string lower = Lower(path);
        return IsPlayerPath(path) || lower == "data/" || lower == "mods/" || lower == "enhanced-textures/" || lower.starts_with("extractor/") ||
               lower == "notes-of-the-player.txt" || lower == "dxgi.dll" || lower == "dlss5-autopilot.json";
    };
    try {
        const fs::path fresh = root / "fresh" / "PT";
        UnpackPayload(data, size, fresh, hash_file);
        const fs::path dest = root / "copy" / "PT";
        old_copy(dest);
        try {
            if (InspectInstall(dest).found) failures += " portable-taken-as-install";
        } catch (const std::exception&) {
        }
        const ExistingInstall seen = InspectPortable(dest, hash_file);
        if (!seen.found || !seen.portable || seen.has_manifest || seen.package_files.size() != old_files.size()) failures += " portable-inspect";
        if (GameFilesFolders(dest) != std::vector<std::string>{"extractor/"}) failures += " portable-game-folder";
        const auto outcome = RunInstall({}, dest, false, steps("p"));
        if (!outcome.updated || outcome.old_version != "9.9.8" || outcome.unchanged != 1 || outcome.verified != new_files.size())
            failures += " portable-counts(" + std::to_string(outcome.unchanged) + "/" + std::to_string(outcome.verified) + ")";
        if (fs::exists(dest / kManifestName)) failures += " portable-manifest-written";
        if (fs::exists(dest / "README.md") || fs::exists(dest / "licenses")) failures += " portable-stale-kept";
        if (read(dest / "extractor" / "chunk1.psarc").size() != 64 || read(dest / "extractor" / "texture.qar") != qar ||
            read(dest / "extractor" / "sce_sys" / "pic1.png") != "the player's title picture")
            failures += " portable-game-files";
        if (read(dest / "data" / "pt.ini") != "settings" || read(dest / "data" / "saves" / "progress.dat") != "save" || read(dest / "mods" / "mine" / "init.lua") != "player mod" ||
            read(dest / "enhanced-textures" / "cache" / "a.dds") != "generated texture" || read(dest / "notes-of-the-player.txt") != "the player's own file" ||
            read(dest / "dxgi.dll") != "reshade")
            failures += " portable-player";
        if (tree(dest, player) != tree(fresh, player)) failures += " portable-tree";
        if (!InspectPortable(dest, hash_file).found) failures += " portable-after";
        const fs::path bare = root / "bare" / "PT";
        write(bare / kGameExe, "exe");
        if (InspectPortable(bare).found) failures += " portable-no-list";
        write(bare / kFileListName, list({{"README.md", "x"}}));
        if (InspectPortable(bare).found) failures += " portable-list-without-game";
        write(bare / kFileListName, list({{kGameExe, "exe"}}));
        WriteManifest(bare, "1.0.7", {{kGameExe, std::string(64, '0')}});
        if (InspectPortable(bare).found) failures += " portable-with-manifest";
        const fs::path undo = root / "undo" / "PT";
        old_copy(undo);
        const auto before = tree(undo, {});
        auto failing = steps("f");
        failing.fail_at = 2;
        try {
            RunInstall({}, undo, false, failing);
            failures += " portable-fail-not-thrown";
        } catch (const std::exception&) {
        }
        if (tree(undo, {}) != before) failures += " portable-fail-undo";
        for (const auto& entry : fs::directory_iterator(undo.parent_path()))
            if (entry.path() != undo) failures += " portable-left-" + PathUtf8(entry.path().filename());
    } catch (const std::exception& e) {
        failures += std::string(" portable-update: ") + e.what();
    }
    std::error_code error;
    fs::remove_all(root, error);
    return failures;
}
inline std::string SelfTestFgUnlockFiles(const fs::path& root) {
    std::string failures;
    auto write = [](const fs::path& file, const std::string& bytes) {
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary | std::ios::trunc) << bytes;
    };
    auto read = [](const fs::path& file) {
        std::ifstream in(file, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), {});
    };
    const std::string proxy = "MZ the community proxy " + std::string(fg_unlock::kProxyMarker) + " loader";
    struct Case {
        const char* name;
        std::string record;
        std::string version_dll = "";
        bool kept;
        bool winmm = false;
    };
    const std::vector<Case> cases{
        {"shipped", "", "", false},
        {"recorded", R"({"format": 1, "complete": true, "proxy": "version.dll", "files": [{"name": "version.dll", "size": 1, "sha256": "x"}, {"name": "dlssg_sm86.ini"}, {"name": "dlssg_sm86/runtime/x.dll"}]})", "", true},
        {"recorded-live", R"({"complete":true,"proxy":"version.dll","files":[{"name":"dlssg_sm86.ini","sha256":"a"},{"name":"version.dll","sha256":"b"}]})", "", true},
        {"recorded-strings", R"({"complete": true, "files": ["version.dll", "dlssg_sm86/x.dll"]})", "", true},
        {"record-unreadable", "{ not json", "", true},
        {"record-other-proxy", R"({"complete": true, "proxy": "winmm.dll", "files": [{"name": "winmm.dll"}, {"name": "dlssg_sm86/x.dll"}]})", "", false, true},
        {"foreign", "", "MZ another program's version.dll (an ASI loader)", true},
    };
    for (const Case& c : cases) {
        const fs::path dest = root / c.name / "PT", staging = dest.parent_path() / ".pt-update-t", backup = dest.parent_path() / ".pt-update-old-t";
        const std::string label = std::string("-") + c.name;
        const std::string old_dll = c.version_dll.empty() ? proxy : c.version_dll;
        write(dest / kGameExe, "old exe");
        write(dest / "version.dll", old_dll);
        write(dest / "CUSA01127" / "chunk1.psarc", "PSAR");
        if (!c.record.empty()) {
            write(dest / std::string(fg_unlock::kManifest), c.record);
            write(dest / std::string(fg_unlock::kIni), "[ini]");
            write(dest / std::string(fg_unlock::kDir) / "runtime" / "x.dll", "runtime");
        }
        if (c.winmm) write(dest / "winmm.dll", proxy);
        WriteManifest(dest, "1.0.7", {{kGameExe, std::string(64, '0')}, {"version.dll", std::string(64, '0')}});
        write(dest / kFileListName, "{\"format\": 1, \"files\": [{\"path\": \"version.dll\", \"size\": 1, \"sha256\": \"" + std::string(64, '0') + "\"}]}");
        write(staging / kGameExe, "new exe");
        write(staging / kManifestName, "pt-port-install 1\nversion=1.0.8\n");
        try {
            const auto swap = SwapProgramFiles(staging, dest, {{kGameExe, ""}}, {kManifestName}, InspectInstall(dest), backup);
            std::error_code error;
            const bool present = fs::exists(dest / "version.dll", error);
            if (present != c.kept) failures += (c.kept ? " fg-proxy-removed" : " fg-proxy-kept") + label;
            if (present && read(dest / "version.dll") != old_dll) failures += " fg-proxy-changed" + label;
            if (!c.kept && read(backup / "version.dll") != old_dll) failures += " fg-proxy-not-in-backup" + label;
            if (!c.record.empty() && (read(dest / std::string(fg_unlock::kManifest)) != c.record || read(dest / std::string(fg_unlock::kIni)) != "[ini]" ||
                                      read(dest / std::string(fg_unlock::kDir) / "runtime" / "x.dll") != "runtime"))
                failures += " fg-record-files" + label;
            if (c.winmm && read(dest / "winmm.dll") != proxy) failures += " fg-recorded-winmm" + label;
            if (read(dest / kGameExe) != "new exe") failures += " fg-new-files" + label;
            const bool said = std::any_of(swap.notes.begin(), swap.notes.end(), [&](const std::string& n) {
                return n.starts_with(c.kept ? "kept version.dll" : "removed version.dll");
            });
            if (!said) failures += " fg-note" + label;
            if (swap.removed != (c.kept ? 0 : 1)) failures += " fg-counts" + label;
        } catch (const std::exception& e) {
            failures += std::string(" fg-swap") + label + ": " + e.what();
        }
    }
    if (!IsPlayerPath("pt-dlssg-route.json") || !IsPlayerPath("dlssg_sm86.ini") || !IsPlayerPath("DLSSG_SM86/runtime/x.dll") || IsPlayerPath("version.dll"))
        failures += " fg-player-paths";
    {
        const fs::path dest = root / "unlockfiles";
        write(dest / std::string(fg_unlock::kManifest), R"({"proxy": "dbghelp.dll", "files": [{"name": "dlssg_sm86/a.dll"}]})");
        const auto keys = UnlockFiles(dest);
        if (!keys.count(PathKey("dbghelp.dll")) || !keys.count(PathKey("dlssg_sm86/a.dll")) || keys.count(PathKey("version.dll")))
            failures += " fg-unlock-files";
    }
    std::error_code error;
    fs::remove_all(root, error);
    return failures;
}
inline std::string SelfTestIcon(const fs::path& root) {
    std::string failures;
    const std::string png =
        "\x89PNG\r\n\x1a\n\x00\x00\x00\x0dIHDR\x00\x00\x00\x01\x00\x00\x00\x01\x08\x02\x00\x00\x00\x90wS\xde\x00\x00\x00\x0cIDAT\x78\x9c\x63\xf8\xcf\xc0\x00\x00\x00\x03\x01\x01\x00\xc9\xfe\x92\xef\x00\x00\x00\x00IEND\xae\x42\x60\x82";
    auto write = [](const fs::path& file, const std::string& bytes) {
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << bytes;
    };
    auto be = [](uint32_t value) {
        std::string out(4, '\0');
        out[0] = char(value >> 24);
        out[1] = char(value >> 16);
        out[2] = char(value >> 8);
        out[3] = char(value);
        return out;
    };
    auto package = [&](bool encrypt) {
        const std::string names("icon0.png", 10);
        const uint32_t table = 0x80, names_at = table + 64, png_at = names_at + uint32_t(names.size());
        std::string bytes(table, '\0');
        bytes[0] = '\x7f';
        bytes[1] = 'C';
        bytes[2] = 'N';
        bytes[3] = 'T';
        bytes.replace(0x13, 1, 1, '\x02');
        bytes[0x1b] = char(table);
        auto entry = [&](uint32_t id, uint32_t flags, uint32_t offset, uint32_t size) {
            return be(id) + be(0) + be(flags) + be(0) + be(offset) + be(size) + std::string(8, '\0');
        };
        bytes += entry(0x0200, 0x40000000u, names_at, uint32_t(names.size()));
        bytes += entry(0x1200, encrypt ? 0x80000000u : 0, png_at, uint32_t(png.size()));
        bytes += names;
        bytes += png;
        return bytes;
    };
    const fs::path dir = root / "dump";
    write(dir / "sce_sys" / "icon0.png", png);
    const auto from_folder = FindIconPng(dir);
    if (!from_folder || *from_folder != png) failures += " folder-icon";
    write(dir / "sce_sys" / "icon0.png", "not a png file");
    if (FindIconPng(dir)) failures += " folder-nonpng";
    write(dir / "sce_sys" / "pic1.png", png);
    const auto picture = FindSystemPng(dir, "pic1.png");
    if (!picture || *picture != png) failures += " folder-title-picture";
    if (FindSystemPng(root / "missing", "pic1.png")) failures += " missing-title-picture";
    const fs::path pkg = root / "game.pkg";
    write(pkg, package(false));
    const auto from_pkg = FindIconPng(pkg);
    if (!from_pkg || *from_pkg != png) failures += " pkg-icon";
    write(pkg, package(true));
    if (FindIconPng(pkg)) failures += " encrypted-icon";
    if (FindIconPng(root / "missing")) failures += " missing-icon";
    return failures;
}

inline std::string SelfTestUnicodePaths(const fs::path& root) {
    auto utf8 = [](const char8_t* value) {
        const auto* bytes = reinterpret_cast<const char*>(value);
        return std::string(bytes, bytes + std::char_traits<char8_t>::length(value));
    };
    const fs::path base = root / Utf8Path(utf8(u8"Š İ 日本語 😀"));
    const fs::path game = base / Utf8Path(utf8(u8"dump-日本語"));
    const fs::path dest = base / Utf8Path(utf8(u8"install-Š-İ-日本語-😀"));
    auto write = [](const fs::path& file, const std::string& bytes) {
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << bytes;
    };
    std::string qar(0x40, '\0');
    qar[0x40 - 0x24 + 0x16] = 'a';
    qar[0x40 - 0x24 + 0x17] = 'q';
    const fs::path psarc = game / Utf8Path(utf8(u8"chunk-日本語-😀.PSARC"));
    const fs::path qar_file = game / Utf8Path(utf8(u8"texture-Š.qar"));
    const fs::path pathid = game / Utf8Path(utf8(u8"pathid_list-İ-日本語.bin"));
    write(psarc, "PSAR" + std::string(60, '\0'));
    write(qar_file, qar);
    write(pathid, "/Assets/sh/level/pt14_hallway/" + std::string(40, 'x'));

    std::string failures;
    try {
        const Source source = ResolveSource(game);
        if (source.kind != SourceKind::Folder || source.path != game || source.files.psarc != psarc || source.files.qar != qar_file || source.files.pathid != pathid)
            failures += " source";
    } catch (const std::exception& e) {
        failures += std::string(" source:") + e.what();
    }

    try {
        const fs::path live = dest / "PT";
        const fs::path stage = dest / ".pt-update-test";
        const fs::path backup = dest / ".pt-update-old-test";
        write(live / kGameExe, "old executable");
        const std::string unicode_payload_path = utf8(u8"voice/日本語😀/dictionary.bin");
        WriteManifest(live, "0.1.0", {{kGameExe, std::string(64, '0')}, {unicode_payload_path, std::string(64, '2')}});
        const auto old = InspectInstall(live);
        if (!old.found || old.version != "0.1.0" || old.files.size() != 2 || old.files.back().path != unicode_payload_path) failures += " inspect";
        write(stage / kGameExe, "new executable");
        write(stage / kManifestName, "pt-port-install 1\nversion=0.2.0\n");
        const std::vector<InstalledFile> files{{kGameExe, std::string(64, '1')}};
        SwapProgramFiles(stage, live, files, {kManifestName}, old, backup);
        std::ifstream installed(live / kGameExe, std::ios::binary);
        const std::string installed_text((std::istreambuf_iterator<char>(installed)), {});
        if (installed_text != "new executable") failures += " install";

        fs::remove_all(stage);
        write(stage / kGameExe, "rollback executable");
        write(stage / kManifestName, "pt-port-install 1\nversion=0.3.0\n");
        bool failed = false;
        try { SwapProgramFiles(stage, live, files, {kManifestName}, InspectInstall(live), backup, 1); }
        catch (...) { failed = true; }
        std::ifstream restored(live / kGameExe, std::ios::binary);
        const std::string restored_text((std::istreambuf_iterator<char>(restored)), {});
        if (!failed || restored_text != "new executable") failures += " rollback";
    } catch (const std::exception& e) {
        failures += std::string(" install-update:") + e.what();
    }
    return failures;
}

struct UninstallResult {
    int removed_files = 0;
    int failed_files = 0;
    bool removed_root = false;
};

inline UninstallResult UninstallInstallFiles(const fs::path& destination, bool remove_data) {
    const fs::path root = FolderPath(destination);
    const ExistingInstall existing = InspectInstall(root);
    if (!existing.found) {
        throw std::runtime_error("This folder does not contain an installation of P.T. PC Port.");
    }
    std::set<std::string> targets;
    for (const auto& f : existing.files) targets.insert(f.path);
    for (const auto& p : existing.package_files) targets.insert(p.path);
    for (const auto& u : UnlockFiles(root)) targets.insert(u);
    targets.insert(kManifestName);
    targets.insert(kGameExe);
    targets.insert("pt-updater.exe");
    targets.insert("install-notes.txt");
    targets.insert("install-extraction.log");

    UninstallResult out;
    std::error_code ec;
    for (const std::string& rel : targets) {
        fs::path file;
        try {
            file = Contained(root, rel);
        } catch (...) {
            continue;
        }
        if (!fs::exists(file, ec)) continue;
        if (fs::remove(file, ec)) {
            ++out.removed_files;
        } else {
            ++out.failed_files;
        }
    }
    if (remove_data) {
        fs::remove_all(root / "data", ec);
        fs::remove_all(root / "mods", ec);
    }
    std::vector<fs::path> dirs;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec)) dirs.push_back(it->path());
    }
    std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) {
        return a.native().size() > b.native().size();
    });
    for (const fs::path& dir : dirs) {
        if (fs::is_empty(dir, ec)) fs::remove(dir, ec);
    }
    if (fs::is_empty(root, ec) && fs::remove(root, ec)) {
        out.removed_root = true;
    }
    return out;
}

inline std::string SelfTestUninstall(const fs::path& root) {
    std::string failures;
    auto write = [](const fs::path& file, const std::string& bytes) {
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << bytes;
    };
    const fs::path keep_dest = root / "keep-data";
    write(keep_dest / kGameExe, "game");
    write(keep_dest / "shaders" / "a.spv", "spv");
    write(keep_dest / "data" / "pt.ini", "settings");
    write(keep_dest / "data" / "save.bin", "save");
    WriteManifest(keep_dest, "1.1.0", {{kGameExe, std::string(64, 'a')}, {"shaders/a.spv", std::string(64, 'b')}});
    try {
        const UninstallResult r = UninstallInstallFiles(keep_dest, false);
        if (r.removed_files < 3 || r.failed_files != 0 || r.removed_root) failures += " uninstall-keep-counts";
        if (fs::exists(keep_dest / kGameExe) || fs::exists(keep_dest / "shaders") || fs::exists(keep_dest / kManifestName))
            failures += " uninstall-keep-leftovers";
        if (!fs::exists(keep_dest / "data" / "pt.ini") || !fs::exists(keep_dest / "data" / "save.bin"))
            failures += " uninstall-data-lost";
    } catch (const std::exception& e) {
        failures += std::string(" uninstall-keep:") + e.what();
    }

    const fs::path full_dest = root / "remove-all";
    write(full_dest / kGameExe, "game");
    write(full_dest / "data" / "pt.ini", "settings");
    WriteManifest(full_dest, "1.1.0", {{kGameExe, std::string(64, 'a')}});
    try {
        const UninstallResult r = UninstallInstallFiles(full_dest, true);
        if (!r.removed_root || fs::exists(full_dest)) failures += " uninstall-full-root";
    } catch (const std::exception& e) {
        failures += std::string(" uninstall-full:") + e.what();
    }

    const fs::path empty_dir = root / "not-an-install";
    fs::create_directories(empty_dir);
    bool refused = false;
    try {
        UninstallInstallFiles(empty_dir, false);
    } catch (...) {
        refused = true;
    }
    if (!refused) failures += " uninstall-non-install-accepted";
    std::error_code ec;
    fs::remove_all(root, ec);
    return failures;
}
}
