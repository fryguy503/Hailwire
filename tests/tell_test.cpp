// SPDX-License-Identifier: MIT
#include "tell.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {
void check(bool condition, const char* reason) {
    if (!condition) {
        std::cerr << reason << '\n';
        std::exit(1);
    }
}
} // namespace

int main() {
    using hailwire::parse_tell;
    const auto basic = parse_tell("Mira tells you, 'I'm ready'");
    check(basic && basic->sender == "Mira" && basic->message == "I'm ready", "Incoming tell");
    const auto translated = parse_tell("World.Ari tells you, in Elvish, 'Hello'");
    check(translated && translated->sender == "World.Ari" && translated->message == "Hello",
          "Qualified and language-annotated tell");
    check(parse_tell("Mira tells you,in an unknown tongue, 'Hello'").has_value(), "Language without leading space");
    check(!parse_tell("You told Mira, 'Hello'"), "Outgoing echo");
    check(!parse_tell("Mira tells the group, 'Ari tells you, 'Hello''"), "Embedded group message");
    check(!parse_tell("The Banker tells you, 'Hello'"), "NPC display name");
    check(!parse_tell("Banker01 tells you, 'Hello'"), "NPC numbered name");
    check(!parse_tell("Mira tells you, '  '"), "Empty tell");
    check(!parse_tell("Mira tells you, 'Missing quote"), "Truncated tell");
    const auto item = parse_tell("Mira tells you, 'Try \x12" + std::string(56, '0') + "Rusty Sword\x12'");
    check(item && item->message == "Try Rusty Sword", "Visible item name without tag metadata");
    const auto unknown = parse_tell("Mira tells you, 'Try \x12" "fmetadata\x12'");
    check(unknown && unknown->message == "Try [link]", "Omit unknown link metadata");
}
