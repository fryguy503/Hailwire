// SPDX-License-Identifier: MIT
#include "tell.h"

#include <algorithm>

namespace hailwire {
namespace {

bool letter(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

bool digit(char ch) {
    return ch >= '0' && ch <= '9';
}

bool hex(char ch) {
    return digit(ch) || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

bool player_name(std::string_view sender) {
    if (sender.size() > 128) return false;
    for (;;) {
        const auto dot = sender.find('.');
        const auto part = sender.substr(0, dot);
        if (part.empty() || !letter(part.front())) return false;
        if (dot == std::string_view::npos) {
            return std::all_of(part.begin(), part.end(), letter);
        }
        if (!std::all_of(part.begin(), part.end(), [](char ch) {
                return letter(ch) || digit(ch) || ch == '-';
            })) return false;
        sender.remove_prefix(dot + 1);
    }
}

void append_text(std::string& output, std::string_view text) {
    for (const unsigned char ch : text) {
        if (ch == '\t') output.push_back(' ');
        else if (ch >= 0x20 && ch != 0x7f) output.push_back(static_cast<char>(ch));
    }
}

void append_link(std::string& output, std::string_view link) {
    if (link.empty()) return;
    if (link[0] == '0') {
        for (const auto metadata : {77u, 56u}) {
            if (link.size() > metadata && std::all_of(link.begin() + 1, link.begin() + metadata, hex)) {
                append_text(output, link.substr(metadata));
                return;
            }
        }
        output += "[item]";
    } else if (link[0] == '3') {
        const auto separator = link.find("^'");
        if (separator != std::string_view::npos) append_text(output, link.substr(separator + 2));
        else output += "[achievement]";
    } else {
        output += "[link]";
    }
}

} // namespace

std::string visible_tell_text(std::string_view message) {
    constexpr char delimiter = '\x12';
    std::string output;
    output.reserve(message.size());
    for (;;) {
        const auto start = message.find(delimiter);
        append_text(output, message.substr(0, start));
        if (start == std::string_view::npos) break;
        message.remove_prefix(start + 1);
        const auto end = message.find(delimiter);
        if (end == std::string_view::npos) {
            output += "[link]";
            break;
        }
        append_link(output, message.substr(0, end));
        message.remove_prefix(end + 1);
    }
    return output;
}

std::optional<Tell> parse_tell(std::string_view line) {
    constexpr std::string_view marker = " tells you,";
    if (line.empty() || line.size() > 4096 || line.back() != '\'') return std::nullopt;
    const auto start = line.find(marker);
    if (start == std::string_view::npos) return std::nullopt;
    const auto sender = line.substr(0, start);
    if (!player_name(sender)) return std::nullopt;

    auto payload = line.substr(start + marker.size());
    if (payload.starts_with(" ")) payload.remove_prefix(1);
    if (payload.starts_with("in ")) {
        const auto separator = payload.find(", '");
        if (separator == std::string_view::npos || separator > 47) return std::nullopt;
        const auto language = payload.substr(3, separator - 3);
        if (language.empty() || !std::all_of(language.begin(), language.end(), [](char ch) {
                return letter(ch) || ch == ' ' || ch == '-' || ch == '\'';
            })) return std::nullopt;
        payload.remove_prefix(separator + 2);
    }
    if (payload.size() < 3 || payload.front() != '\'') return std::nullopt;
    auto message = visible_tell_text(payload.substr(1, payload.size() - 2));
    if (message.find_first_not_of(' ') == std::string::npos) return std::nullopt;
    return Tell{std::string(sender), std::move(message)};
}

} // namespace hailwire
