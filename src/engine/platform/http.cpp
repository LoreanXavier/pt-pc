#include "engine/platform/http.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <type_traits>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <dlfcn.h>
#endif

namespace pt::http {

std::optional<int64_t> ParseHttpDate(const std::string& text) {
    static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char month[4] = {};
    int day = 0, year = 0, hour = 0, minute = 0, second = 0;
    const auto comma = text.find(',');
    const std::string rest = comma == std::string::npos ? text : text.substr(comma + 1);
    if (std::sscanf(rest.c_str(), " %d %3s %d %d:%d:%d", &day, month, &year, &hour, &minute, &second) != 6) return std::nullopt;
    int mon = -1;
    for (int i = 0; i < 12; ++i) {
        if (std::strcmp(month, kMonths[i]) == 0) mon = i;
    }
    if (mon < 0 || year < 1970) return std::nullopt;
    // days from civil (proleptic Gregorian), no time zone involved
    const int y = year - (mon < 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int m = mon + 1;
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = int64_t(era) * 146097 + doe - 719468;
    return days * 86400 + hour * 3600 + minute * 60 + second;
}

#ifdef _WIN32

namespace {
struct Handle {
    HINTERNET h = nullptr;
    ~Handle() { if (h) WinHttpCloseHandle(h); }
};
std::wstring Widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
}

std::optional<Response> Get(const Request& request) {
    const std::wstring url = Widen(request.url);
    URL_COMPONENTS parts{sizeof(parts)};
    wchar_t host[256] = {}, path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    parts.dwExtraInfoLength = 1;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) return std::nullopt;
    std::wstring resource(path, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo && parts.dwExtraInfoLength) resource.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    Handle session{WinHttpOpen(L"pt-port", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.h) return std::nullopt;
    WinHttpSetTimeouts(session.h, request.timeout_ms, request.timeout_ms, request.timeout_ms, request.timeout_ms);
    Handle connection{WinHttpConnect(session.h, host, parts.nPort, 0)};
    if (!connection.h) return std::nullopt;
    Handle req{WinHttpOpenRequest(connection.h, L"GET", resource.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (!req.h) return std::nullopt;
    if (!request.follow_redirects) {
        DWORD off = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(req.h, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof(off));
    }
    const wchar_t* headers = request.no_cache ? L"Cache-Control: no-cache\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS;
    if (!WinHttpSendRequest(req.h, headers, request.no_cache ? DWORD(-1) : 0, nullptr, 0, 0, 0) || !WinHttpReceiveResponse(req.h, nullptr))
        return std::nullopt;
    Response response;
    DWORD status = 0, bytes = sizeof(status);
    if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &bytes, nullptr)) return std::nullopt;
    response.status = int(status);
    DWORD age = 0;
    bytes = sizeof(age);
    response.aged = WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CUSTOM | WINHTTP_QUERY_FLAG_NUMBER, L"Age", &age, &bytes, nullptr) && age > 0;
    SYSTEMTIME date{};
    bytes = sizeof(date);
    if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_DATE | WINHTTP_QUERY_FLAG_SYSTEMTIME, nullptr, &date, &bytes, nullptr)) {
        FILETIME file{};
        if (SystemTimeToFileTime(&date, &file)) {
            ULARGE_INTEGER ticks{};
            ticks.LowPart = file.dwLowDateTime;
            ticks.HighPart = file.dwHighDateTime;
            response.date = int64_t(ticks.QuadPart / 10000000) - 11644473600LL;
        }
    }
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(req.h, &available) || available == 0) break;
        if (response.body.size() + available > request.max_body) return std::nullopt;
        const size_t at = response.body.size();
        response.body.resize(at + available);
        DWORD read = 0;
        if (!WinHttpReadData(req.h, response.body.data() + at, available, &read)) return std::nullopt;
        response.body.resize(at + read);
    }
    return response;
}

#else

namespace {
// The few libcurl entry points used, with the option numbers of curl.h (stable ABI since libcurl 7)
using CURL = void;
struct curl_slist;
enum : int {
    kOptWriteData = 10001, kOptUrl = 10002, kOptUserAgent = 10018, kOptHttpHeader = 10023, kOptHeaderData = 10029,
    kOptWriteFunction = 20011, kOptHeaderFunction = 20079, kOptFollowLocation = 52, kOptMaxRedirs = 68, kOptNoSignal = 99,
    kOptTimeoutMs = 155, kOptConnectTimeoutMs = 156, kOptProtocolsStr = 10318,
};
constexpr int kInfoResponseCode = 0x200002;
struct Curl {
    void* library = nullptr;
    CURL* (*easy_init)() = nullptr;
    int (*easy_setopt)(CURL*, int, ...) = nullptr;
    int (*easy_perform)(CURL*) = nullptr;
    int (*easy_getinfo)(CURL*, int, ...) = nullptr;
    void (*easy_cleanup)(CURL*) = nullptr;
    curl_slist* (*slist_append)(curl_slist*, const char*) = nullptr;
    void (*slist_free_all)(curl_slist*) = nullptr;
    Curl() {
#ifdef __APPLE__
        /* macOS ships libcurl with the system */
        for (const char* name : {"/usr/lib/libcurl.4.dylib", "libcurl.4.dylib", "libcurl.dylib"}) {
#else
        for (const char* name : {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so"}) {
#endif
            if ((library = dlopen(name, RTLD_NOW | RTLD_LOCAL))) break;
        }
        if (!library) return;
        auto load = [&](auto& fn, const char* name) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(library, name)); };
        load(easy_init, "curl_easy_init");
        load(easy_setopt, "curl_easy_setopt");
        load(easy_perform, "curl_easy_perform");
        load(easy_getinfo, "curl_easy_getinfo");
        load(easy_cleanup, "curl_easy_cleanup");
        load(slist_append, "curl_slist_append");
        load(slist_free_all, "curl_slist_free_all");
    }
    bool Ready() const { return easy_init && easy_setopt && easy_perform && easy_getinfo && easy_cleanup && slist_append && slist_free_all; }
};
struct Sink {
    Response* response;
    size_t max;
    bool overflow = false;
};
size_t OnBody(char* data, size_t size, size_t count, void* user) {
    auto* sink = static_cast<Sink*>(user);
    if (sink->response->body.size() + size * count > sink->max) {
        sink->overflow = true;
        return 0;
    }
    sink->response->body.append(data, size * count);
    return size * count;
}
size_t OnHeader(char* data, size_t size, size_t count, void* user) {
    auto* response = static_cast<Response*>(user);
    std::string line(data, size * count);
    std::string lower = line;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (lower.starts_with("http/")) {
        // a new response (after a redirect): forget the previous one's headers
        response->date.reset();
        response->aged = false;
    } else if (lower.starts_with("date:")) {
        response->date = ParseHttpDate(line.substr(5));
    } else if (lower.starts_with("age:")) {
        response->aged = std::atol(line.c_str() + 4) > 0;
    }
    return size * count;
}
}

std::optional<Response> Get(const Request& request) {
    static Curl curl;
    if (!curl.Ready() || !request.url.starts_with("https://")) return std::nullopt;
    CURL* handle = curl.easy_init();
    if (!handle) return std::nullopt;
    Response response;
    Sink sink{&response, request.max_body};
    curl_slist* headers = request.no_cache ? curl.slist_append(nullptr, "Cache-Control: no-cache") : nullptr;
    curl.easy_setopt(handle, kOptUrl, request.url.c_str());
    curl.easy_setopt(handle, kOptProtocolsStr, "https");
    curl.easy_setopt(handle, kOptUserAgent, "pt-port");
    curl.easy_setopt(handle, kOptNoSignal, 1L);
    curl.easy_setopt(handle, kOptTimeoutMs, long(request.timeout_ms));
    curl.easy_setopt(handle, kOptConnectTimeoutMs, long(request.timeout_ms));
    curl.easy_setopt(handle, kOptFollowLocation, request.follow_redirects ? 1L : 0L);
    curl.easy_setopt(handle, kOptMaxRedirs, 5L);
    curl.easy_setopt(handle, kOptWriteFunction, &OnBody);
    curl.easy_setopt(handle, kOptWriteData, &sink);
    curl.easy_setopt(handle, kOptHeaderFunction, &OnHeader);
    curl.easy_setopt(handle, kOptHeaderData, &response);
    if (headers) curl.easy_setopt(handle, kOptHttpHeader, headers);
    const int result = curl.easy_perform(handle);
    long status = 0;
    curl.easy_getinfo(handle, kInfoResponseCode, &status);
    curl.easy_cleanup(handle);
    if (headers) curl.slist_free_all(headers);
    if (result != 0 || sink.overflow || status == 0) return std::nullopt;
    response.status = int(status);
    return response;
}

#endif

}
