#include "engine/core/crash_upload.h"
#include "engine/platform/third_party.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <random>
#include <chrono>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

#include "engine/core/log.h"
#include "engine/platform/http.h"
#include "engine/platform/os.h"

namespace pt {
namespace {

constexpr const char* kMinidumpUrl =
    "https://o4512234559111168.ingest.de.sentry.io/api/4512234589192272/minidump/?sentry_key=59acba0456e2617d53ad958a3be13210";
constexpr size_t kMaxLogBytes = 4u << 20;
constexpr int kMaxPerStart = 3;

std::string ReadFile(const std::filesystem::path& path, size_t limit) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const auto length = in.tellg();
    if (length < 0) return {};
    const bool dump = path.extension() == ".dmp";
    if (dump && static_cast<uint64_t>(length) > limit) {
        LogWarn("crash report: {} exceeds the upload limit, not truncated", pt::os::PathToUtf8(path.filename()));
        return {};
    }
    const size_t bytes = std::min<uint64_t>(static_cast<uint64_t>(length), limit);
    in.seekg(length - static_cast<std::streamoff>(bytes));
    std::string data(bytes, '\0');
    in.read(data.data(), static_cast<std::streamsize>(bytes));
    if (!in) return {};
    return data;
}

bool Exists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

void Touch(const std::filesystem::path& path) {
    std::ofstream(path, std::ios::binary) << "1";
}

std::vector<std::filesystem::path> Dumps(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = pt::os::PathToUtf8(it->path().filename());
        if (name.starts_with("pt-crash-") && name.ends_with(".dmp")) out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string JsonEscape(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
    }
    return out;
}

std::string UnrecordedProxyLine(const std::string& log) {
    const size_t at = log.find("unrecorded proxy ");
    if (at == std::string::npos) return {};
    const size_t end = log.find_first_of("\r\n", at);
    return log.substr(at, std::min<size_t>(end == std::string::npos ? log.size() - at : end - at, 600));
}
std::string ProxyTags(const std::string& line) { return line.empty() ? std::string() : ",\"unrecorded_proxy\":\"yes\""; }
std::string ThirdPartyLine(const std::string& log) {
    const size_t at = log.rfind("third party: ");
    if (at == std::string::npos) return {};
    const size_t start = at + 13;
    const size_t end = log.find_first_of("\r\n", start);
    return log.substr(start, std::min<size_t>(end == std::string::npos ? log.size() - start : end - start, 500));
}
std::string ThirdPartyTags(const std::string& line) {
    return line.empty() ? std::string() : ",\"third_party\":\"" + JsonEscape(third_party::ToolNames(line).substr(0, 200)) + "\"";
}
std::string ProxyExtra(const std::string& line, const std::string& tools = {}) {
    std::string extra;
    if (!line.empty()) extra += "\"unrecorded_proxy\":\"" + JsonEscape(line) + "\"";
    if (!tools.empty()) extra += std::string(extra.empty() ? "" : ",") + "\"third_party\":\"" + JsonEscape(tools) + "\"";
    return extra.empty() ? std::string() : ",\"extra\":{" + extra + "}";
}

std::string CrashVersionFromLog(const std::filesystem::path& log, const std::string& build, const std::string& dump_name) {
    if (ReadFile(log, kMaxLogBytes).find(dump_name) == std::string::npos) return "unknown";
    std::ifstream header(log, std::ios::binary);
    std::string line, logged_build, logged_version;
    for (int i = 0; i < 32 && std::getline(header, line); ++i) {
        if (const size_t p = line.find("pt-port version "); p != std::string::npos) {
            const size_t start = p + 16;
            logged_version = line.substr(start, line.find(' ', start) - start);
        }
        if (const size_t p = line.find("crash reporting: build "); p != std::string::npos) {
            const size_t start = p + 23;
            logged_build = line.substr(start, line.find(',', start) - start);
        }
    }
    return logged_build == build && !logged_version.empty() ? logged_version : "unknown";
}

void Upload(std::vector<std::filesystem::path> dumps, std::string version) {
    for (const std::filesystem::path& dump : dumps) {
        std::vector<http::FormPart> parts;
        const std::string data = ReadFile(dump, 256u << 20);
        if (data.empty()) continue;
        parts.push_back({"upload_file_minidump", pt::os::PathToUtf8(dump.filename()), "application/octet-stream", data});
        std::filesystem::path log = dump;
        log.replace_extension(".log");
        std::string crash_version = "unknown", proxy, tools;
        const std::string filename = pt::os::PathToUtf8(dump.filename());
        const std::string build = filename.size() > 29 ? filename.substr(9, filename.size() - 29) : "unknown";
        if (Exists(log)) {
            crash_version = CrashVersionFromLog(log, build, filename);
            if (crash_version != "unknown") {
                const std::string text = ReadFile(log, kMaxLogBytes);
                proxy = UnrecordedProxyLine(text);
                tools = ThirdPartyLine(text);
                parts.push_back({"pt.log", "pt.log", "text/plain", text});
            }
        }
        parts.push_back({"sentry", "", "", "{\"release\":\"pt-port@" + JsonEscape(crash_version) + "\",\"tags\":{\"build\":\"" +
                                             JsonEscape(build) + "\",\"uploader_version\":\"" + JsonEscape(version) + "\",\"os\":\"" +
#ifdef _WIN32
                                             "windows"
#elif defined(__APPLE__)
                                             "macos"
#else
                                             "linux"
#endif
                                             "\"" + ProxyTags(proxy) + ThirdPartyTags(tools) + "}" + ProxyExtra(proxy, tools) + "}"});
        const auto response = http::PostForm(kMinidumpUrl, parts);
        if (response && response->status >= 200 && response->status < 300) {
            std::filesystem::path sent = dump;
            sent.replace_extension(".sent");
            Touch(sent);
            LogInfo("crash report: {} sent", pt::os::PathToUtf8(dump.filename()));
        } else {
            LogWarn("crash report: {} not sent ({}), tried again at the next start", pt::os::PathToUtf8(dump.filename()),
                    response ? std::to_string(response->status) : std::string("no answer"));
        }
    }
}

}

void PreserveCrashLog(const std::filesystem::path& log_path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(log_path, ec)) return;
    const auto log_time = std::filesystem::last_write_time(log_path, ec);
    if (ec) return;
    const std::string session_log = ReadFile(log_path, kMaxLogBytes);
    const std::filesystem::path dir = log_path.has_parent_path() ? log_path.parent_path() : std::filesystem::path(".");
    for (const std::filesystem::path& dump : Dumps(dir)) {
        std::filesystem::path log = dump;
        log.replace_extension(".log");
        if (Exists(log)) continue;
        if (session_log.find(pt::os::PathToUtf8(dump.filename())) == std::string::npos) continue;
        const auto dump_time = std::filesystem::last_write_time(dump, ec);
        if (ec || log_time < dump_time - std::chrono::minutes(1)) continue;
        std::filesystem::copy_file(log_path, log, ec);
    }
}

void OfferCrashUpload(const std::filesystem::path& dir, const std::string& version, bool interactive) {
    std::vector<std::filesystem::path> pending;
    for (const std::filesystem::path& dump : Dumps(dir)) {
        std::filesystem::path sent = dump, skip = dump;
        sent.replace_extension(".sent");
        skip.replace_extension(".skip");
        std::error_code ec;
        const auto age = std::filesystem::file_time_type::clock::now() - std::filesystem::last_write_time(dump, ec);
        if (!ec && age < std::chrono::hours(24 * 14) && !Exists(sent) && !Exists(skip)) pending.push_back(dump);
    }
    if (pending.empty()) return;
    if (pending.size() > kMaxPerStart) pending.erase(pending.begin(), pending.end() - kMaxPerStart);
    const std::filesystem::path consent = dir / "crash-reports.txt";
    bool always = ReadFile(consent, 64).starts_with("always");
    if (!always) {
        if (!interactive) return;
        const SDL_MessageBoxButtonData buttons[] = {
            {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Send"},
            {0, 2, "Always send"},
            {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Don't send"},
        };
        const std::string text =
            "P.T. closed unexpectedly last time.\n\nSend the crash report to the developer? It holds the crash dump (the game's "
            "memory state when it stopped) and pt.log (the game's log: settings, GPU and what happened). Nothing else is sent.";
        SDL_MessageBoxData box{SDL_MESSAGEBOX_INFORMATION, nullptr, "P.T. crash report", text.c_str(), SDL_arraysize(buttons), buttons, nullptr};
        int choice = 0;
        if (!SDL_ShowMessageBox(&box, &choice)) return;
        if (choice == 0) {
            for (const std::filesystem::path& dump : pending) {
                std::filesystem::path skip = dump;
                skip.replace_extension(".skip");
                Touch(skip);
            }
            LogInfo("crash report: {} declined", pending.size());
            return;
        }
        if (choice == 2) {
            std::ofstream(consent, std::ios::binary) << "always\n";
            always = true;
        }
    }
    LogInfo("crash report: sending {}", pending.size());
    std::thread(Upload, std::move(pending), version).detach();
}

namespace {
constexpr const char* kEnvelopeUrl =
    "https://o4512234559111168.ingest.de.sentry.io/api/4512234589192272/envelope/?sentry_key=59acba0456e2617d53ad958a3be13210";
std::atomic<int> g_log_report{0};
}

void SendLogReport(const std::filesystem::path& log, const std::filesystem::path& dir, const std::string& version) {
    if (g_log_report.exchange(static_cast<int>(LogReportState::Sending)) == static_cast<int>(LogReportState::Sending)) return;
    LogInfo("log report: sending pt.log{} to the developer", Dumps(dir).empty() ? "" : " and the newest crash dump");
    std::thread([log, dir, version] {
        std::mt19937_64 random(std::random_device{}() ^ static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::string event_id = std::format("{:016x}{:016x}", random(), random());
        const std::string proxy = UnrecordedProxyLine(ReadFile(log, kMaxLogBytes));
        const std::string tools = ThirdPartyLine(ReadFile(log, kMaxLogBytes));
        const std::string event = "{\"event_id\":\"" + event_id + "\",\"level\":\"info\",\"logger\":\"pt-port\",\"message\":{\"formatted\":\"pt.log sent by the player\"}," +
                                  "\"release\":\"pt-port@" + JsonEscape(version) + "\",\"tags\":{\"report\":\"player\",\"os\":\"" +
#ifdef _WIN32
                                  "windows"
#elif defined(__APPLE__)
                                  "macos"
#else
                                  "linux"
#endif
                                  "\"" + ProxyTags(proxy) + ThirdPartyTags(tools) + "}" + ProxyExtra(proxy, tools) + "}";
        std::string body = "{\"event_id\":\"" + event_id + "\"}\n";
        body += "{\"type\":\"event\",\"length\":" + std::to_string(event.size()) + "}\n" + event + "\n";
        const std::string text = ReadFile(log, kMaxLogBytes);
        body += "{\"type\":\"attachment\",\"length\":" + std::to_string(text.size()) + ",\"filename\":\"pt.log\",\"content_type\":\"text/plain\"}\n" + text + "\n";
        if (const std::vector<std::filesystem::path> dumps = Dumps(dir); !dumps.empty()) {
            const std::filesystem::path& dump = dumps.back();
            const std::string data = ReadFile(dump, 64u << 20);
            if (!data.empty()) {
                body += "{\"type\":\"attachment\",\"length\":" + std::to_string(data.size()) + ",\"filename\":\"" + JsonEscape(pt::os::PathToUtf8(dump.filename())) +
                        "\",\"attachment_type\":\"event.minidump\"}\n" + data + "\n";
            }
            std::filesystem::path crash_log = dump;
            crash_log.replace_extension(".log");
            if (Exists(crash_log)) {
                const std::string old = ReadFile(crash_log, kMaxLogBytes);
                body += "{\"type\":\"attachment\",\"length\":" + std::to_string(old.size()) + ",\"filename\":\"crash-pt.log\",\"content_type\":\"text/plain\"}\n" + old + "\n";
            }
        }
        const auto response = http::PostBody(kEnvelopeUrl, "application/x-sentry-envelope", body);
        const bool ok = response && response->status >= 200 && response->status < 300;
        if (ok) {
            LogInfo("log report: sent ({})", event_id);
        } else {
            LogWarn("log report: not sent ({})", response ? std::to_string(response->status) : std::string("no answer"));
        }
        g_log_report = static_cast<int>(ok ? LogReportState::Sent : LogReportState::Failed);
    }).detach();
}

LogReportState GetLogReportState() {
    return static_cast<LogReportState>(g_log_report.load());
}

}
