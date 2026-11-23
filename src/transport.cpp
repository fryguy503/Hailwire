#include "transport.h"

#include <algorithm>
#include <charconv>
#include <memory>
#include <optional>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif

namespace hailwire {
namespace {

constexpr auto kRequestLimit = 16u * 1024;
constexpr auto kResponseLimit = 64u * 1024;

struct Origin {
    std::string_view host;
    unsigned short port;
    bool tls;
};

bool alphanumeric(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool valid_host(std::string_view host) {
    if (host.empty() || host.size() > 253 || host.back() == '.') return false;
    while (!host.empty()) {
        const auto separator = host.find('.');
        const auto label = host.substr(0, separator);
        if (label.empty() || label.size() > 63 || !alphanumeric(label.front()) || !alphanumeric(label.back())) {
            return false;
        }
        if (label.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") != label.npos) {
            return false;
        }
        if (separator == host.npos) break;
        host.remove_prefix(separator + 1);
    }
    return true;
}

std::optional<Origin> parse_origin(std::string_view url) {
    const bool tls = url.starts_with("https://");
    if (!tls && !url.starts_with("http://")) return std::nullopt;
    url.remove_prefix(tls ? 8 : 7);
    if (url.ends_with('/')) url.remove_suffix(1);

    const auto colon = url.find(':');
    const auto host = url.substr(0, colon);
    if (!valid_host(host)) return std::nullopt;
    if (!tls && host != "127.0.0.1" && host != "localhost") return std::nullopt;

    unsigned port = tls ? 443 : 80;
    if (colon != url.npos) {
        const auto field = url.substr(colon + 1);
        const auto parsed = std::from_chars(field.data(), field.data() + field.size(), port);
        if (parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size() || port == 0 || port > 65535) {
            return std::nullopt;
        }
    }
    return Origin{host, static_cast<unsigned short>(port), tls};
}

Response failure(std::string error) {
    return {0, {}, std::move(error)};
}

int retry_after(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == value.npos) return 0;
    value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    unsigned seconds = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return 0;
    return static_cast<int>(std::min(seconds, 60u));
}

#ifdef _WIN32
struct InternetCloser {
    void operator()(void* handle) const { WinHttpCloseHandle(handle); }
};
using InternetHandle = std::unique_ptr<void, InternetCloser>;

Response windows_error(const char* action) {
    const auto error = GetLastError();
    return failure(std::string(action) + " (WinHTTP " + std::to_string(error) + ")");
}

std::wstring wide_ascii(std::string_view value) {
    return std::wstring(value.begin(), value.end());
}

Response send_request(const Origin& origin, const Request& request) {
    InternetHandle session(WinHttpOpen(L"Hailwire/1", origin.tls ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY : WINHTTP_ACCESS_TYPE_NO_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return windows_error("Could not initialize HTTPS");
    if (!WinHttpSetTimeouts(session.get(), 2500, 2500, 3000, 3500)) return windows_error("Could not set request timeouts");

    InternetHandle connection(WinHttpConnect(session.get(), wide_ascii(origin.host).c_str(), origin.port, 0));
    if (!connection) return windows_error("Could not open the relay connection");
    InternetHandle call(WinHttpOpenRequest(connection.get(), L"POST", wide_ascii(request.path).c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          origin.tls ? WINHTTP_FLAG_SECURE : 0));
    if (!call) return windows_error("Could not initialize the relay request");
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(call.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect))) {
        return windows_error("Could not set the redirect policy");
    }

    std::wstring headers = L"Content-Type: application/json\r\nAccept: application/json\r\n";
    if (!request.bearer.empty()) headers += L"Authorization: Bearer " + wide_ascii(request.bearer) + L"\r\n";
    const auto size = static_cast<DWORD>(request.json.size());
    if (!WinHttpSendRequest(call.get(), headers.c_str(), static_cast<DWORD>(headers.size()),
                            const_cast<char*>(request.json.data()), size, size, 0) ||
        !WinHttpReceiveResponse(call.get(), nullptr)) {
        return windows_error("Could not reach the relay");
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(call.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX)) {
        return windows_error("Relay returned an invalid response");
    }
    Response result{static_cast<int>(status), {}, {}};
    wchar_t retry[32]{};
    DWORD retry_size = sizeof(retry);
    if (WinHttpQueryHeaders(call.get(), WINHTTP_QUERY_RETRY_AFTER, WINHTTP_HEADER_NAME_BY_INDEX,
                            retry, &retry_size, WINHTTP_NO_HEADER_INDEX)) {
        const auto value = std::wstring_view(retry, retry_size / sizeof(wchar_t));
        if (value.find_first_not_of(L"0123456789 \t") == value.npos) {
            std::string ascii;
            for (const auto character : value) ascii.push_back(static_cast<char>(character));
            result.retry_after_seconds = retry_after(ascii);
        }
    }
    char buffer[4096];
    for (;;) {
        DWORD count = 0;
        if (!WinHttpReadData(call.get(), buffer, sizeof(buffer), &count)) return windows_error("Could not read the relay response");
        if (count == 0) break;
        if (result.body.size() + count > kResponseLimit) return failure("Relay response was too large");
        result.body.append(buffer, count);
    }
    return result;
}
#else
class CurlRuntime {
public:
    CurlRuntime() : ready(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK) {}
    ~CurlRuntime() { if (ready) curl_global_cleanup(); }

    const bool ready;
};

size_t receive(char* bytes, size_t size, size_t count, void* output) noexcept {
    auto& body = *static_cast<std::string*>(output);
    if (size != 0 && count > (kResponseLimit - body.size()) / size) return 0;
    try {
        body.append(bytes, size * count);
        return size * count;
    } catch (...) {
        return 0;
    }
}

size_t receive_header(char* bytes, size_t size, size_t count, void* output) noexcept {
    const auto length = size * count;
    auto line = std::string_view(bytes, length);
    auto& retry = *static_cast<int*>(output);
    if (line.starts_with("HTTP/")) retry = 0;
    constexpr auto name = std::string_view("retry-after:");
    if (line.size() < name.size() || !std::equal(name.begin(), name.end(), line.begin(), [](char lower, char raw) {
            return lower == (raw >= 'A' && raw <= 'Z' ? raw + ('a' - 'A') : raw);
        })) return length;
    retry = retry_after(line.substr(name.size()));
    return length;
}

Response send_request(const Origin& origin, const Request& request) {
    static const CurlRuntime runtime;
    if (!runtime.ready) return failure("Could not initialize HTTPS");
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> call(curl_easy_init(), curl_easy_cleanup);
    if (!call) return failure("Could not initialize the relay request");

    auto base = std::string_view(request.base_url);
    if (base.ends_with('/')) base.remove_suffix(1);
    const std::string url = std::string(base) + request.path;
    using Headers = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;
    Headers headers(nullptr, curl_slist_free_all);
    const auto add_header = [&](const char* value) {
        auto* list = curl_slist_append(headers.get(), value);
        if (!list) return false;
        headers.release();
        headers.reset(list);
        return true;
    };
    if (!add_header("Content-Type: application/json") || !add_header("Accept: application/json") ||
        (!request.bearer.empty() && !add_header(("Authorization: Bearer " + request.bearer).c_str()))) {
        return failure("Could not allocate request headers");
    }

    Response response;
    curl_easy_setopt(call.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(call.get(), CURLOPT_USERAGENT, "Hailwire/1");
    curl_easy_setopt(call.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(call.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(call.get(), CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    curl_easy_setopt(call.get(), CURLOPT_TIMEOUT_MS, 8000L);
    curl_easy_setopt(call.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(call.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(call.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    if (!origin.tls) curl_easy_setopt(call.get(), CURLOPT_NOPROXY, "*");
    curl_easy_setopt(call.get(), CURLOPT_POSTFIELDS, request.json.c_str());
    curl_easy_setopt(call.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(request.json.size()));
    curl_easy_setopt(call.get(), CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(call.get(), CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(call.get(), CURLOPT_HEADERFUNCTION, receive_header);
    curl_easy_setopt(call.get(), CURLOPT_HEADERDATA, &response.retry_after_seconds);
    const auto result = curl_easy_perform(call.get());
    if (result == CURLE_WRITE_ERROR) return failure("Could not buffer the relay response");
    if (result != CURLE_OK) return failure(std::string("Could not reach the relay (") + curl_easy_strerror(result) + ")");

    long status = 0;
    curl_easy_getinfo(call.get(), CURLINFO_RESPONSE_CODE, &status);
    response.status = static_cast<int>(status);
    return response;
}
#endif

} // namespace

bool valid_relay_url(std::string_view url) { return parse_origin(url).has_value(); }

Response post(const Request& request) {
    const auto origin = parse_origin(request.base_url);
    if (!origin) return failure("Enter an HTTPS relay address with no path");
    if (!request.path.starts_with("/v1/") ||
        request.path.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789/-_") != request.path.npos) {
        return failure("Invalid relay request path");
    }
    if (request.bearer.size() > 256 || request.bearer.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != request.bearer.npos) {
        return failure("Invalid relay credential");
    }
    if (request.json.size() > kRequestLimit) return failure("Relay request was too large");
    return send_request(*origin, request);
}

} // namespace hailwire
