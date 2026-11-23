#pragma once

#include <string>
#include <string_view>

namespace hailwire {

struct Request {
    std::string base_url;
    std::string path;
    std::string bearer;
    std::string json;
};

struct Response {
    int status = 0;
    std::string body;
    std::string error;
    int retry_after_seconds = 0;
};

bool valid_relay_url(std::string_view url);
// Synchronous. Call this from the request worker, never from a game callback.
Response post(const Request& request);

} // namespace hailwire
