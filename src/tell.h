// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace hailwire {

struct Tell {
    std::string sender;
    std::string message;
};

// Fallback for published Hailstone SDKs that expose formatted chat.
std::optional<Tell> parse_tell(std::string_view line);
std::string visible_tell_text(std::string_view message);

} // namespace hailwire
