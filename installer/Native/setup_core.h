#pragma once
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
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
        const auto size = it->file_size(error);
        if (!error) sum += size;
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
    std::string title;
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
inline std::string TitleId(const fs::path& folder) {
    std::error_code error;
    const fs::path sfo = folder / "sce_sys" / "param.sfo";
    if (!fs::is_regular_file(sfo, error) || fs::file_size(sfo, error) > 1024 * 1024) return {};
    std::ifstream input(sfo, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(input)), {});
    auto u32 = [&](size_t at) -> uint32_t { if (at + 4 > data.size()) return 0; uint32_t v; std::memcpy(&v, data.data() + at, 4); return v; };
    auto u16 = [&](size_t at) -> uint16_t { if (at + 2 > data.size()) return 0; uint16_t v; std::memcpy(&v, data.data() + at, 2); return v; };
    if (data.size() < 0x14 || data.compare(0, 4, std::string("\0PSF", 4)) != 0) return {};
    const uint32_t keys = u32(8), values = u32(12), count = u32(16);
    for (uint32_t i = 0; i < count && i < 512; ++i) {
        const size_t entry = 0x14 + size_t(i) * 16;
        if (entry + 16 > data.size()) break;
        const size_t key = keys + u16(entry), value = values + u32(entry + 12), length = u32(entry + 4);
        if (key >= data.size() || value > data.size() || length > data.size() - value) continue;
        if (std::string(data.c_str() + key) == "TITLE_ID") {
            std::string id = data.substr(value, length);
            id.erase(std::find(id.begin(), id.end(), '\0'), id.end());
            return id;
        }
    }
    return {};
}
inline const char* Region(const std::string& title) {
    return title == "CUSA01127" ? "US" : title == "CUSA01114" ? "Europe" : title == "CUSA01098" ? "Japan" : nullptr;
}
inline std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
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
    if (region && files.title != "CUSA01127")
        files.notes.push_back(files.title + " is the " + region + " release; the port is tested with the US release CUSA01127 (other releases are expected to hold the same data, not yet verified)");
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
        if (files.title.empty()) files.title = TitleId(dir.parent_path());
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
inline std::optional<std::string> IconFromFolder(const fs::path& dir) {
    std::error_code error;
    const fs::path icon = dir / "sce_sys" / "icon0.png";
    if (!fs::is_regular_file(icon, error)) return std::nullopt;
    const auto size = fs::file_size(icon, error);
    if (error || size < 8 || size > kIconMaxBytes) return std::nullopt;
    const std::string bytes = ReadSlice(icon, 0, size);
    if (!IsPng(bytes)) return std::nullopt;
    return bytes;
}
inline std::optional<std::string> IconFromPackage(const fs::path& pkg) {
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
        if (std::string(names.c_str() + name_at) != "icon0.png") continue;
        const std::string bytes = ReadSlice(pkg, offset, size);
        if (IsPng(bytes)) return bytes;
    }
    return std::nullopt;
}
inline std::optional<std::string> FindIconPng(const fs::path& input) {
    std::error_code error;
    if (input.empty() || !fs::exists(input, error)) return std::nullopt;
    if (fs::is_regular_file(input, error)) {
        if (IsPackage(input)) {
            if (auto icon = IconFromPackage(input)) return icon;
        }
        if (auto icon = IconFromFolder(input.parent_path())) return icon;
        return std::nullopt;
    }
    if (auto icon = IconFromFolder(input)) return icon;
    for (fs::directory_iterator it(input, fs::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error))
        if (it->is_directory(error))
            if (auto icon = IconFromFolder(it->path())) return icon;
    return std::nullopt;
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
    const char* region = Region(files.title);
    source << "title_id=" << files.title << "\nregion=" << (region ? region : "unknown") << "\nsource=folder\n";
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
inline std::vector<InstalledFile> UnpackPayload(const unsigned char* data, size_t size, const fs::path& root,
                                           const std::function<std::string(const fs::path&)>& hash_file) {
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
        std::vector<unsigned char> bytes(std::max<uint64_t>(bytes_size, 1));
        uLongf output = static_cast<uLongf>(bytes.size());
        if (uncompress(bytes.data(), &output, reader.p, static_cast<uLong>(packed)) != Z_OK || output != bytes_size)
            throw std::runtime_error("Installer payload decompression failed.");
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
            for (uint64_t at = 0; at < bytes_size;) {
                const uint64_t chunk = std::min<uint64_t>(4 * 1024 * 1024, bytes_size - at);
                file.write(reinterpret_cast<const char*>(bytes.data()) + at, std::streamsize(chunk));
                at += chunk;
                progress.Add(chunk);
                CheckCancel();
            }
            if (!file) throw std::runtime_error("Could not write installation files.");
        }
        if (hash_file(path) != digest) throw std::runtime_error("Installer payload integrity check failed.");
        written.push_back({PathUtf8(Utf8Path(name)), digest});
    }
    if (reader.left) throw std::runtime_error("Unexpected payload data.");
    return written;
}

inline constexpr const char* kManifestName = "pt-install-manifest.txt";
#ifdef _WIN32
inline constexpr const char* kGameExe = "pt.exe";
#else
inline constexpr const char* kGameExe = "pt";
#endif
struct ExistingInstall {
    bool found = false;
    bool has_manifest = false;
    std::string version;
    std::vector<InstalledFile> files;
};
inline void WriteManifest(const fs::path& dir, const std::string& version, const std::vector<InstalledFile>& files) {
    const fs::path temporary = dir / (std::string(kManifestName) + ".new");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << "pt-port-install 1\nversion=" << version << "\n";
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
inline ExistingInstall InspectInstall(const fs::path& dir) {
    ExistingInstall install;
    std::error_code error;
    if (!fs::exists(dir, error)) return install;
    if (!fs::is_directory(dir, error) || fs::is_symlink(dir, error)) throw std::runtime_error("Choose a new installation folder. Existing folders are never overwritten.");
    std::ifstream manifest(dir / kManifestName, std::ios::binary);
    std::string line;
    if (manifest && std::getline(manifest, line) && line == "pt-port-install 1") {
        install.found = install.has_manifest = true;
        while (std::getline(manifest, line)) {
            if (line.starts_with("version=")) install.version = line.substr(8);
            if (line.starts_with("file=") && line.size() > 5 + 65) {
                InstalledFile file{line.substr(5 + 65), line.substr(5, 64)};
                Contained(dir, file.path);
                if (!IsUserDataPath(file.path)) install.files.push_back(std::move(file));
            }
        }
        return install;
    }
    const bool exe = fs::is_regular_file(dir / kGameExe, error) || fs::is_regular_file(dir / "pt.exe", error);
    const bool archives = fs::is_regular_file(dir / "CUSA01127" / "chunk1.psarc", error) && fs::is_regular_file(dir / "CUSA01127" / "texture.qar", error);
    if (exe && archives) {
        install.found = true;
        return install;
    }
    throw std::runtime_error("This folder holds something else. Choose a new installation folder, or a folder with a P.T. install to update it.");
}
inline bool KnownProgramFile(const std::string& path) {
    static const char* const kNames[] = {"pt.exe", "pt", "amd_fidelityfx_vk.dll", "nvngx_dlss.dll", "libxess.dll", "README-UPDATE.txt", "manifest.json"};
    for (const char* name : kNames)
        if (path == name) return true;
    if (path == "voice/cmudict-en-us.dict" || path.starts_with("voice/en-us/") || path.starts_with("voice/vosk/")) return true;
    return (path.starts_with("shaders/") && path.ends_with(".spv")) || path.starts_with("extractor/");
}
struct SwapResult {
    int replaced = 0, added = 0, removed = 0;
};
inline SwapResult SwapProgramFiles(const fs::path& staging, const fs::path& dest, const std::vector<InstalledFile>& files,
                                   const std::vector<std::string>& extra, const ExistingInstall& old, const fs::path& backup, int fail_at = -1) {
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
    auto tick = [&] { if (step++ == fail_at) throw std::runtime_error("Test failure during the update."); };
    auto backup_old = [&](const std::string& path) {
        const fs::path target = Contained(dest, path), saved = Contained(backup, path);
        fs::create_directories(saved.parent_path());
        fs::rename(target, saved);
    };
    try {
        std::vector<std::string> all;
        for (const auto& file : files) all.push_back(file.path);
        all.insert(all.end(), extra.begin(), extra.end());
        for (const auto& path : all) {
            CheckCancel();
            tick();
            const fs::path target = Contained(dest, path);
            std::error_code error;
            const bool existed = fs::exists(target, error);
            if (existed) {
                if (fs::is_directory(target, error)) throw std::runtime_error("A folder is in the way of " + path + ".");
                backup_old(path);
            }
            journal.push_back({path, false, existed});
            fs::create_directories(target.parent_path());
            fs::rename(Contained(staging, path), target);
            journal.back().placed = true;
            existed ? ++result.replaced : ++result.added;
        }
        std::vector<std::string> obsolete;
        auto shipped = [&](const std::string& path) { return std::find(all.begin(), all.end(), path) != all.end(); };
        if (old.has_manifest) {
            for (const auto& file : old.files)
                if (!shipped(file.path)) obsolete.push_back(file.path);
        } else if (old.found) {
            std::error_code error;
            for (fs::recursive_directory_iterator it(dest, fs::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error)) {
                if (!it->is_regular_file(error)) continue;
                const std::string path = PathUtf8(fs::relative(it->path(), dest));
                if (KnownProgramFile(path) && !shipped(path)) obsolete.push_back(path);
            }
        }
        for (const auto& path : obsolete) {
            tick();
            std::error_code error;
            if (!fs::is_regular_file(Contained(dest, path), error)) continue;
            backup_old(path);
            journal.push_back({path, false, true});
            ++result.removed;
        }
        tick();
    } catch (...) {
        for (auto it = journal.rbegin(); it != journal.rend(); ++it) {
            std::error_code error;
            if (it->placed) fs::rename(Contained(dest, it->path), Contained(staging, it->path), error);
            if (it->backed) fs::rename(Contained(backup, it->path), Contained(dest, it->path), error);
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
};
struct InstallOutcome {
    bool updated = false;
    bool had_manifest = false;
    std::string old_version;
    bool archives_restored = false;
    SwapResult swap;
    std::vector<std::string> notes;
};
inline InstallOutcome RunInstall(const fs::path& input, fs::path destination, bool shortcut, const InstallSteps& steps) {
    if (destination.empty()) throw std::runtime_error("Choose an installation folder.");
    steps.check_parents(destination);
    const ExistingInstall old = InspectInstall(destination);
    InstallOutcome outcome;
    destination = fs::absolute(destination);
    const fs::path parent = destination.parent_path();
    const bool update = old.found;
    const bool need_archives = !update || !ArchivesUsable(destination / "CUSA01127");
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
        uint64_t total = steps.payload_bytes ? steps.payload_bytes() : 0, archives = 0;
        if (source) {
            auto size = [](const fs::path& file) -> uint64_t {
                std::error_code error;
                const auto bytes = file.empty() ? 0 : fs::file_size(file, error);
                return error ? 0 : bytes;
            };
            archives = source->kind == SourceKind::Package ? size(source->path) : size(source->files.psarc) + size(source->files.qar) + size(source->files.pathid);
            total += archives;
        }
        progress.Begin(total);
        Report("Preparing native PC runtime...");
        const std::vector<InstalledFile> files = steps.unpack(staging);
        ValidateProgramFiles(files);
        if (steps.validate_runtime) steps.validate_runtime(files);
        if (!update) EnsureDataDirectory(staging, steps.unique_id, steps.check_parents);
        std::vector<std::string> extra;
        if (source) {
            if (source->kind == SourceKind::Package) {
                Report("Extracting your game package...");
                const uint64_t before = progress.done;
                steps.extract(staging, source->path);
                progress.At(before + archives);
            } else {
                CopyArchives(source->files, staging / "CUSA01127");
            }
            outcome.notes = WriteNotes(*source, staging);
            outcome.archives_restored = update;
            std::error_code error;
            for (const char* name : {"CUSA01127/chunk1.psarc", "CUSA01127/texture.qar", "CUSA01127/pathid_list_ps4.bin", "CUSA01127/source.txt",
                                     "install-notes.txt", "install-extraction.log"})
                if (fs::is_regular_file(staging / name, error)) extra.push_back(name);
        }
        if (shortcut) {
            const fs::path look = source ? source->path : input;
            if (!look.empty()) {
                if (const auto icon = FindIconPng(look)) {
                    std::ofstream icon_file(staging / "icon0.png", std::ios::binary);
                    icon_file.write(icon->data(), std::streamsize(icon->size()));
                    if (icon_file) extra.push_back("icon0.png");
                }
            }
        }
        WriteManifest(staging, steps.version, files);
        CheckCancel();
        if (!update) {
            if (fs::exists(destination)) throw std::runtime_error("Choose a new installation folder. Existing folders are never overwritten.");
            fs::rename(staging, destination);
            if (shortcut) steps.shortcut(destination);
            return outcome;
        }
        extra.push_back(kManifestName);
        Report("Updating the program files...");
        outcome.updated = true;
        outcome.had_manifest = old.has_manifest;
        outcome.old_version = old.version;
        try {
            outcome.swap = SwapProgramFiles(staging, destination, files, extra, old, backup);
        } catch (const fs::filesystem_error& e) {
            throw std::runtime_error("Could not replace " + PathUtf8(e.path1().filename()) +
                                     ": it is in use (is P.T. still running?). Close it and try again. Nothing was changed.");
        }
        remove_ours(backup);
        remove_ours(staging);
        if (shortcut) steps.shortcut(destination);
        return outcome;
    } catch (...) {
        remove_ours(staging);
        bool kept = false;
        std::error_code error;
        for (fs::recursive_directory_iterator it(backup, error), end; !error && it != end; it.increment(error))
            if (it->is_regular_file(error)) kept = true;
        if (!kept) remove_ours(backup);
        throw;
    }
}
inline std::string UpdateQuestion(const ExistingInstall& old, const std::string& version) {
    if (!old.found) return {};
    if (old.has_manifest && old.version == version)
        return "P.T. version " + version + " is already installed here and up to date. Repair it (check the game files and rewrite the program files)?";
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
}
