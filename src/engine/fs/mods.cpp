#include "engine/platform/os.h"
#include "engine/fs/mods.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <system_error>

#include "engine/core/log.h"

namespace pt::mods {
namespace {

std::atomic<const ModSet*> g_active{nullptr};

char Lower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// UTF-8 whatever the system code page (a path's string() throws on Windows for names outside it)
std::string Utf8(const std::u8string& text) {
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string LowerCopy(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), Lower);
    return out;
}

// A JSON reader for mod.json: one object of plain values; comments, trailing commas and unknown keys are let through
class JsonReader {
public:
    explicit JsonReader(std::string_view text) : text_(text) {
        if (text_.starts_with("\xEF\xBB\xBF")) {
            text_.remove_prefix(3);
        }
    }

    bool ReadObject(Manifest& out, std::string* error) {
        Skip();
        if (!Eat('{')) {
            return Fail(error, "mod.json is not a JSON object");
        }
        while (true) {
            Skip();
            if (Eat('}')) {
                return true;
            }
            std::string key;
            if (!ReadString(key)) {
                return Fail(error, "expected a key in quotes");
            }
            Skip();
            if (!Eat(':')) {
                return Fail(error, "expected ':' after \"" + key + "\"");
            }
            Skip();
            Value value;
            if (!ReadValue(value)) {
                return Fail(error, "bad value for \"" + key + "\"");
            }
            Assign(out, LowerCopy(key), value);
            Skip();
            if (Eat(',')) {
                continue;
            }
            Skip();
            if (Eat('}')) {
                return true;
            }
            return Fail(error, "expected ',' or '}' after \"" + key + "\"");
        }
    }

private:
    struct Value {
        enum Kind { String, Number, Bool, Other } kind = Other;
        std::string text;
        double number = 0.0;
        bool flag = false;
    };

    static bool Fail(std::string* error, std::string message) {
        if (error) {
            *error = std::move(message);
        }
        return false;
    }

    static void Assign(Manifest& out, const std::string& key, const Value& v) {
        if (v.kind == Value::String) {
            if (key == "name") out.name = v.text;
            else if (key == "version") out.version = v.text;
            else if (key == "author") out.author = v.text;
            else if (key == "description") out.description = v.text;
            else if (key == "priority") out.priority = std::atoi(v.text.c_str());
            else if (key == "enabled") out.enabled = LowerCopy(v.text) != "false" && v.text != "0";
        } else if (v.kind == Value::Number) {
            if (key == "priority") out.priority = static_cast<int>(std::clamp(v.number, -1e9, 1e9));
            else if (key == "version") out.version = v.text;
            else if (key == "enabled") out.enabled = v.number != 0.0;
        } else if (v.kind == Value::Bool && key == "enabled") {
            out.enabled = v.flag;
        }
    }

    bool AtEnd() const { return pos_ >= text_.size(); }
    char Peek() const { return AtEnd() ? '\0' : text_[pos_]; }
    bool Eat(char c) {
        if (Peek() == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    void Skip() {
        while (!AtEnd()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else if (text_.substr(pos_).starts_with("//")) {
                while (!AtEnd() && text_[pos_] != '\n') ++pos_;
            } else if (text_.substr(pos_).starts_with("/*")) {
                const size_t end = text_.find("*/", pos_ + 2);
                pos_ = end == std::string_view::npos ? text_.size() : end + 2;
            } else {
                return;
            }
        }
    }

    static void AppendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool ReadHex4(uint32_t& out) {
        if (pos_ + 4 > text_.size()) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool ReadString(std::string& out) {
        if (!Eat('"')) return false;
        while (!AtEnd()) {
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (AtEnd()) return false;
            const char e = text_[pos_++];
            switch (e) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'u': {
                uint32_t cp = 0;
                if (!ReadHex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && text_.substr(pos_).starts_with("\\u")) {
                    pos_ += 2;
                    uint32_t low = 0;
                    if (!ReadHex4(low)) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                AppendUtf8(out, cp);
                break;
            }
            default: out.push_back(e); break;
            }
        }
        return false;
    }

    // nested objects and arrays are skipped (no key of mod.json takes one)
    bool SkipNested(char open, char close) {
        int depth = 0;
        while (!AtEnd()) {
            Skip();
            const char c = Peek();
            if (c == '"') {
                std::string ignored;
                if (!ReadString(ignored)) return false;
                continue;
            }
            ++pos_;
            if (c == open) ++depth;
            else if (c == close && --depth == 0) return true;
        }
        return false;
    }

    bool ReadValue(Value& v) {
        const char c = Peek();
        if (c == '"') {
            v.kind = Value::String;
            return ReadString(v.text);
        }
        if (c == '{') return SkipNested('{', '}');
        if (c == '[') return SkipNested('[', ']');
        const size_t start = pos_;
        while (!AtEnd() && (std::isalnum(static_cast<unsigned char>(text_[pos_])) || text_[pos_] == '-' || text_[pos_] == '+' ||
                            text_[pos_] == '.')) {
            ++pos_;
        }
        const std::string word(text_.substr(start, pos_ - start));
        if (word.empty()) return false;
        if (word == "true" || word == "false") {
            v.kind = Value::Bool;
            v.flag = word == "true";
            return true;
        }
        if (word == "null") return true;
        char* end = nullptr;
        v.number = std::strtod(word.c_str(), &end);
        if (end != word.c_str() + word.size()) return false;
        v.kind = Value::Number;
        v.text = word;
        return true;
    }

    std::string_view text_;
    size_t pos_ = 0;
};

std::filesystem::path FindChild(const std::filesystem::path& dir, std::string_view lower_name, bool directory) {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const bool kind = directory ? it->is_directory(ec) : it->is_regular_file(ec);
        if (kind && LowerCopy(Utf8(it->path().filename().u8string())) == lower_name) {
            return it->path();
        }
    }
    return {};
}

}

bool ParseManifest(std::string_view json, Manifest& out, std::string* error) {
    Manifest parsed;
    if (!JsonReader(json).ReadObject(parsed, error)) {
        return false;
    }
    out = std::move(parsed);
    return true;
}

std::string AssetKey(std::string_view path) {
    std::string key = LowerCopy(path);
    std::replace(key.begin(), key.end(), '\\', '/');
    size_t start = key.find_first_not_of('/');
    if (start == std::string::npos) {
        return {};
    }
    std::string_view rest = std::string_view(key).substr(start);
    if (rest.starts_with("assets/")) {
        rest.remove_prefix(7);
    } else if (rest.starts_with("as/")) {
        rest.remove_prefix(3);
    } else {
        return {};
    }
    // "a//b" and "./" spellings of the same file
    std::string out;
    out.reserve(rest.size());
    size_t i = 0;
    while (i < rest.size()) {
        const size_t slash = rest.find('/', i);
        const std::string_view part = rest.substr(i, slash == std::string_view::npos ? std::string_view::npos : slash - i);
        if (!part.empty() && part != ".") {
            if (!out.empty()) out.push_back('/');
            out.append(part);
        }
        if (slash == std::string_view::npos) break;
        i = slash + 1;
    }
    return out;
}

bool Wins(const Mod& a, const Mod& b) {
    if (a.manifest.priority != b.manifest.priority) {
        return a.manifest.priority > b.manifest.priority;
    }
    const std::string fa = LowerCopy(a.folder), fb = LowerCopy(b.folder);
    if (fa != fb) {
        return fa > fb;
    }
    return a.folder > b.folder;
}

void SortByPriority(std::vector<Mod>& mods) {
    std::stable_sort(mods.begin(), mods.end(), Wins);
}

std::vector<Mod> Discover(const std::filesystem::path& dir, std::vector<std::string>* warnings) {
    std::vector<Mod> mods;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return mods;
    }
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entry_ec;
        if (!it->is_directory(entry_ec)) {
            continue;
        }
        Mod mod;
        mod.root = it->path();
        mod.folder = Utf8(it->path().filename().u8string());
        if (mod.folder.empty() || mod.folder[0] == '.') {
            continue;
        }
        if (const auto manifest = FindChild(mod.root, "mod.json", false); !manifest.empty()) {
            mod.has_manifest = true;
            std::string error;
            const auto bytes = ReadDiskFile(manifest);
            const std::string_view text = bytes ? std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()) : "";
            if (!bytes || !ParseManifest(text, mod.manifest, &error)) {
                if (warnings) {
                    warnings->push_back(mod.folder + "/mod.json: " + (bytes ? error : std::string("cannot be read")) +
                                        "; the folder name and defaults are used");
                }
                mod.manifest = Manifest{};
            }
        }
        mod.enabled = mod.manifest.enabled;
        mod.has_script = !FindChild(mod.root, "init.lua", false).empty();
        if (const auto assets = FindChild(mod.root, "assets", true); !assets.empty()) {
            std::error_code walk_ec;
            for (std::filesystem::recursive_directory_iterator file(assets, std::filesystem::directory_options::skip_permission_denied,
                                                                    walk_ec),
                 last;
                 !walk_ec && file != last; file.increment(walk_ec)) {
                std::error_code file_ec;
                if (file->is_regular_file(file_ec)) {
                    mod.files.push_back(Utf8(file->path().lexically_relative(assets).generic_u8string()));
                }
            }
            std::sort(mod.files.begin(), mod.files.end());
        }
        mods.push_back(std::move(mod));
    }
    SortByPriority(mods);
    return mods;
}

void OverrideIndex::Build(const std::vector<Mod>& mods) {
    files_.clear();
    for (uint32_t i = 0; i < mods.size(); ++i) {
        const Mod& mod = mods[i];
        if (!mod.enabled) {
            continue;
        }
        std::filesystem::path assets = FindChild(mod.root, "assets", true);
        if (assets.empty()) {
            assets = mod.root / "Assets";
        }
        for (const std::string& file : mod.files) {
            const std::string key = AssetKey("/Assets/" + file);
            if (key.empty()) {
                continue;
            }
            auto it = files_.find(key);
            if (it == files_.end()) {
                files_.emplace(key, Hit{assets / std::filesystem::path(std::u8string(file.begin(), file.end())), i});
            } else if (Wins(mod, mods[it->second.mod])) {
                it->second = Hit{assets / std::filesystem::path(std::u8string(file.begin(), file.end())), i};
            }
        }
    }
}

const OverrideIndex::Hit* OverrideIndex::Find(std::string_view asset_path) const {
    if (files_.empty()) {
        return nullptr;
    }
    const std::string key = AssetKey(asset_path);
    if (key.empty()) {
        return nullptr;
    }
    auto it = files_.find(key);
    return it == files_.end() ? nullptr : &it->second;
}

std::unique_ptr<ModSet> Load(const std::filesystem::path& dir, const std::map<std::string, bool>& overrides,
                             std::vector<std::string>* warnings) {
    auto set = std::make_unique<ModSet>();
    set->mods = Discover(dir, warnings);
    for (Mod& mod : set->mods) {
        if (auto it = overrides.find(mod.folder); it != overrides.end()) {
            mod.enabled = it->second;
        }
    }
    set->index.Build(set->mods);
    return set;
}

void SetActive(const ModSet* set) {
    g_active.store(set && !set->index.Empty() ? set : nullptr, std::memory_order_release);
}

const ModSet* Active() {
    return g_active.load(std::memory_order_acquire);
}

std::optional<std::vector<uint8_t>> ReadOverride(std::string_view asset_path) {
    const ModSet* set = Active();
    if (!set) {
        return std::nullopt;
    }
    const OverrideIndex::Hit* hit = set->index.Find(asset_path);
    if (!hit) {
        return std::nullopt;
    }
    return ReadDiskFile(hit->file);
}

std::optional<std::vector<uint8_t>> ReadDiskFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return std::nullopt;
    }
    const std::streamoff size = file.tellg();
    if (size < 0) {
        return std::nullopt;
    }
    // no game file comes near this; a larger override is refused with a reason instead of an allocation that fails
    constexpr std::streamoff kMaxFile = std::streamoff(1) << 30;
    if (size > kMaxFile) {
        LogError("mods: {} is {} bytes, more than the {} an override may have; the game's own file is used", pt::os::PathToUtf8(path), size, kMaxFile);
        return std::nullopt;
    }
    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.seekg(0);
    if (!data.empty() && !file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()))) {
        return std::nullopt;
    }
    return data;
}

}
