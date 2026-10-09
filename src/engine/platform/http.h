#pragma once

// One HTTPS GET for the update check: WinHTTP on Windows, the system's libcurl (loaded at run
// time, so the game starts without it) elsewhere. Blocking: call it off the game loop.

#include <cstdint>
#include <optional>
#include <string>

namespace pt::http {

struct Response {
    int status = 0;
    std::string body;
    std::optional<int64_t> date;  // the Date header as a Unix second
    bool aged = false;            // an Age header above 0: a cached answer
};

struct Request {
    std::string url;  // https only
    int timeout_ms = 5000;
    bool follow_redirects = false;
    size_t max_body = 256 * 1024;
    bool no_cache = false;
};

// nullopt when there is no answer at all (offline, DNS, TLS, timeout, no libcurl)
std::optional<Response> Get(const Request& request);

// "Tue, 06 Oct 2026 09:25:00 GMT" as a Unix second
std::optional<int64_t> ParseHttpDate(const std::string& text);

}
