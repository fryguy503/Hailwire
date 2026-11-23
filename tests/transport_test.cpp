#include "transport.h"

#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    using namespace hailwire;
    int failed = 0;
    const auto expect = [&](bool condition, std::string_view name) {
        if (!condition) {
            std::cerr << name << '\n';
            ++failed;
        }
    };

    for (const auto url : {"https://relay.example.com", "https://wire.example.org:8443/", "http://127.0.0.1:8787",
                           "http://localhost:8787/", "https://localhost"}) {
        expect(valid_relay_url(url), url);
    }
    for (const auto url : {"", "relay.example.com", "http://relay.example.com", "http://localhost.example.org",
                           "http://127.0.0.1.example.org", "https://-relay.example.org", "https://relay..example.org",
                           "https://relay.example.org/extra", "https://relay.example.org//", "https://relay.example.org?x=1",
                           "https://name:pass@relay.example.org", "https://relay.example.org:", "https://relay.example.org:70000",
                           "https://relay.example.org:0", "https://relay.example.org#x", "https://relay.example.org\\extra",
                           "https://relay.example.org\r\n", "https://relay%.example.org"}) {
        expect(!valid_relay_url(url), url);
    }
    const std::string base = argc == 2 ? argv[1] : "http://127.0.0.1:1";
    expect(post({base, "/v1/tells?other-host", "", "{}"}).error == "Invalid relay request path", "Reject unsafe request path");
    expect(post({base, "/v1/tells", "abc\r\nInjected", "{}"}).error == "Invalid relay credential", "Reject control characters in credentials");
    expect(post({base, "/v1/tells", "", std::string(16385, 'x')}).error == "Relay request was too large", "Bound the request body");

    if (argc == 2) {
        const auto success = post({base, "/v1/inspect", "fixture-device_token", R"({"tell":"Hello from EverQuest!"})"});
        expect(success.status == 200 && success.body == R"({"received":true})" && success.error.empty(), "JSON POST, token and response body");
        const auto redirected = post({base, "/v1/redirect", "fixture-device_token", "{}"});
        expect(redirected.status == 307, "Never follow a redirect with a private tell or token");
        const auto throttled = post({base, "/v1/throttle", "", "{}"});
        expect(throttled.status == 429 && throttled.retry_after_seconds == 17, "Read the Retry-After header");
        const auto excessive = post({base, "/v1/excessive", "", "{}"});
        expect(excessive.status == 0 && !excessive.error.empty(), "Bound the response body");
    }
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
