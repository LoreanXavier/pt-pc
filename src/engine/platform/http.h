#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace pt::http {

struct Response {
    int status = 0;
    std::string body;
    std::optional<int64_t> date;
    bool aged = false;
};

struct Request {
    std::string url;
    int timeout_ms = 5000;
    bool follow_redirects = false;
    size_t max_body = 256 * 1024;
    bool no_cache = false;
};

std::optional<Response> Get(const Request& request);

std::optional<int64_t> ParseHttpDate(const std::string& text);

}
