#include "engine/platform/update_check.h"

#include <cctype>
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <map>
#include <thread>
#include <vector>

#include "engine/platform/http.h"

#if __has_include("pt_version.h")
#include "pt_version.h"
#endif
#ifndef PT_VERSION
#define PT_VERSION "0.0.0-dev"
#endif
#ifndef PT_UPDATE_MANIFEST_URL
#define PT_UPDATE_MANIFEST_URL ""
#endif

namespace pt::update {
namespace {
bool Placeholder(std::string_view url) {
    const auto scheme = url.find("://");
    const auto host_start = scheme == std::string_view::npos ? 0 : scheme + 3;
    const auto host_end = url.find_first_of("/:", host_start);
    const std::string_view host = url.substr(host_start, host_end == std::string_view::npos ? std::string_view::npos : host_end - host_start);
    return host.empty() || host.ends_with(".invalid") || host == "invalid";
}

struct Value {
    std::string text;
    std::map<std::string, Value> members;
    bool object = false;
};
class Reader {
public:
    explicit Reader(std::string_view s) : s_(s) {}
    bool Parse(Value& out) {
        if (!Read(out, 0)) return false;
        Space();
        return at_ == s_.size();
    }

private:
    void Space() {
        while (at_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[at_]))) ++at_;
    }
    bool String(std::string& out) {
        if (at_ >= s_.size() || s_[at_] != '"') return false;
        for (++at_; at_ < s_.size(); ++at_) {
            const char c = s_[at_];
            if (c == '"') {
                ++at_;
                return true;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (++at_ >= s_.size()) return false;
            switch (s_[at_]) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': break;
                case 'b': case 'f': break;
                case 'u': {
                    unsigned code = 0;
                    if (at_ + 4 >= s_.size() || std::from_chars(s_.data() + at_ + 1, s_.data() + at_ + 5, code, 16).ec != std::errc()) return false;
                    at_ += 4;
                    if (code < 0x80) out.push_back(char(code));
                    else if (code < 0x800) { out.push_back(char(0xC0 | (code >> 6))); out.push_back(char(0x80 | (code & 0x3F))); }
                    else { out.push_back(char(0xE0 | (code >> 12))); out.push_back(char(0x80 | ((code >> 6) & 0x3F))); out.push_back(char(0x80 | (code & 0x3F))); }
                    break;
                }
                default: out.push_back(s_[at_]); break;
            }
        }
        return false;
    }
    bool Read(Value& out, int depth) {
        if (depth > 16) return false;
        Space();
        if (at_ >= s_.size()) return false;
        const char c = s_[at_];
        if (c == '"') return String(out.text);
        if (c == '{' || c == '[') {
            const char close = c == '{' ? '}' : ']';
            out.object = c == '{';
            ++at_;
            Space();
            if (at_ < s_.size() && s_[at_] == close) { ++at_; return true; }
            for (;;) {
                Space();
                std::string key;
                if (out.object) {
                    if (!String(key)) return false;
                    Space();
                    if (at_ >= s_.size() || s_[at_++] != ':') return false;
                }
                Value item;
                if (!Read(item, depth + 1)) return false;
                if (out.object) out.members[key] = std::move(item);
                Space();
                if (at_ >= s_.size()) return false;
                if (s_[at_] == ',') { ++at_; continue; }
                if (s_[at_] == close) { ++at_; return true; }
                return false;
            }
        }
        const size_t start = at_;
        while (at_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[at_])) || s_[at_] == '.' || s_[at_] == '-' || s_[at_] == '+')) ++at_;
        return at_ > start;
    }
    std::string_view s_;
    size_t at_ = 0;
};
const std::string* Text(const Value& object, const char* key) {
    const auto it = object.members.find(key);
    return it != object.members.end() && !it->second.object && !it->second.text.empty() ? &it->second.text : nullptr;
}
}

std::string ManifestUrl() {
    if (const char* env = std::getenv("PT_UPDATE_MANIFEST_URL"); env && *env) return env;
    return PT_UPDATE_MANIFEST_URL;
}

std::string_view CurrentVersion() { return PT_VERSION; }

std::string_view Platform() {
#ifdef _WIN32
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

int CompareVersions(std::string_view a, std::string_view b) {
    auto split = [](std::string_view v, std::string_view& suffix) {
        std::vector<long> parts;
        if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.remove_prefix(1);
        const auto dash = v.find('-');
        suffix = dash == std::string_view::npos ? std::string_view() : v.substr(dash + 1);
        v = v.substr(0, dash);
        while (!v.empty()) {
            const auto dot = v.find('.');
            const std::string_view part = v.substr(0, dot);
            long n = 0;
            std::from_chars(part.data(), part.data() + part.size(), n);
            parts.push_back(n);
            if (dot == std::string_view::npos) break;
            v.remove_prefix(dot + 1);
        }
        return parts;
    };
    std::string_view sa, sb;
    const auto pa = split(a, sa), pb = split(b, sb);
    for (size_t i = 0; i < std::max(pa.size(), pb.size()); ++i) {
        const long x = i < pa.size() ? pa[i] : 0, y = i < pb.size() ? pb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    if (sa.empty() != sb.empty()) return sa.empty() ? 1 : -1;
    return sa == sb ? 0 : (sa < sb ? -1 : 1);
}

std::optional<Release> ParseManifest(std::string_view json, std::string_view platform) {
    Value root;
    if (!Reader(json).Parse(root) || !root.object) return std::nullopt;
    const std::string* version = Text(root, "version");
    if (!version) return std::nullopt;
    Release release;
    release.version = *version;
    if (const std::string* url = Text(root, "url")) release.url = *url;
    if (const std::string* notes = Text(root, "notes")) release.notes = *notes;
    if (const auto platforms = root.members.find("platforms"); platforms != root.members.end()) {
        if (const auto entry = platforms->second.members.find(std::string(platform)); entry != platforms->second.members.end()) {
            if (const std::string* url = Text(entry->second, "url")) release.url = *url;
        }
    }
    if (release.url.size() > 512) release.url.resize(512);
    if (release.notes.size() > 200) release.notes.resize(200);
    return release;
}

void Checker::Start() {
    if (state_) return;
    state_ = std::make_shared<State>();
    const std::string url = ManifestUrl();
    if (url.empty() || Placeholder(url) || !url.starts_with("https://")) {
        state_->done = true;
        return;
    }
    std::thread([state = state_, url] {
        http::Request request;
        request.url = url;
        request.timeout_ms = 5000;
        request.follow_redirects = true;
        request.max_body = 64 * 1024;
        if (const auto response = http::Get(request); response && response->status == 200) {
            if (auto release = ParseManifest(response->body, Platform()); release && CompareVersions(release->version, CurrentVersion()) > 0) {
                std::lock_guard lock(state->mutex);
                state->newer = std::move(release);
            }
        }
        state->done = true;
    }).detach();
}

void Checker::Fake(std::string_view version) {
    if (state_) return;
    state_ = std::make_shared<State>();
    if (CompareVersions(version, CurrentVersion()) > 0) {
        Release release;
        release.version = std::string(version);
        release.url = "(fake update, --fake-update)";
        std::lock_guard lock(state_->mutex);
        state_->newer = std::move(release);
    }
    state_->done = true;
}

std::optional<Release> Checker::Newer() const {
    if (!state_) return std::nullopt;
    std::lock_guard lock(state_->mutex);
    return state_->newer;
}

bool Checker::Done() const { return state_ && state_->done; }

}
