// SPDX-License-Identifier: MIT
#include "hailstone_plugin.h"
#include "json.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using Json = nlohmann::json;
namespace fs = std::filesystem;

struct HailstoneArgs { std::vector<std::string> values; };
struct HailstoneOut { Json table; std::string key; };

extern "C" std::uint32_t hailstone_plugin_abi();
extern "C" void hailstone_plugin_set_host(const PluginHostApi*);
extern "C" void InitializePlugin() noexcept;
extern "C" void ShutdownPlugin() noexcept;
extern "C" void OnPulse() noexcept;
extern "C" void SetGameState(int) noexcept;
extern "C" void OnZoned() noexcept;
extern "C" int OnIncomingChat(const char*, std::uint32_t) noexcept;

namespace {
using namespace std::chrono_literals;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int write_path(const fs::path& dir, char* out, std::size_t capacity) {
    fs::create_directories(dir);
    const auto path = dir.u8string();
    if (path.size() >= capacity) return 0;
    std::memcpy(out, path.c_str(), path.size() + 1);
    return static_cast<int>(path.size());
}

template <typename Function>
struct Callback { Function function; void* user; };

struct Host {
    explicit Host(fs::path directory) : directory(std::move(directory)) {
        api.abi = HAILSTONE_PLUGIN_ABI;
        api.size = sizeof(api);
        api.host = this;
        api.caps = [](void*) { return HAILSTONE_CAP_READ | HAILSTONE_CAP_COMMANDS |
                                       HAILSTONE_CAP_EVENTS | HAILSTONE_CAP_UI | HAILSTONE_CAP_NET; };
        api.config_dir = [](void* h, char* out, std::size_t n) { return write_path(from(h).directory / "plugin", out, n); };
        api.character_config_dir = [](void* h, char* out, std::size_t n) {
            return write_path(from(h).directory / from(h).character, out, n);
        };
        api.get_player = [](void* h, HailstonePlayer* player) {
            auto& f = from(h);
            player->in_zone = f.game_state == GAMESTATE_INGAME;
            player->game_state = f.game_state;
            std::memcpy(player->name, f.character.c_str(), f.character.size() + 1);
            return 1;
        };
        api.server_info = [](void*, HailstoneServer* server) {
            std::memcpy(server->world, "Bastion", sizeof("Bastion"));
            return 1;
        };
        api.next_spawn = [](void*, std::uint32_t after, HailstoneSpawn* spawn) {
            if (after) return 0;
            spawn->id = 24;
            spawn->is_npc = 1;
            std::memcpy(spawn->name, "Banker", sizeof("Banker"));
            return 1;
        };
        api.add_command = [](void* h, const char* name, HailstoneCommandFn fn, void* user) {
            from(h).commands.emplace(name, Callback{fn, user});
            return 1;
        };
        api.remove_command = [](void* h, const char* name) { from(h).commands.erase(name); };
        api.add_ui_function = [](void* h, const char* name, HailstoneUiFn fn, void* user) {
            from(h).functions.emplace(name, Callback{fn, user});
            return 1;
        };
        api.remove_ui_function = [](void* h, const char* name) { from(h).functions.erase(name); };
        api.out_begin_table = [](void*, HailstoneOut* out) { out->table = Json::object(); };
        api.out_key = [](void*, HailstoneOut* out, const char* key) { out->key = key; };
        api.out_value = [](void*, HailstoneOut* out, const HailstoneValue* value) {
            if (value->kind == HAILSTONE_BOOL) out->table[out->key] = value->i != 0;
            if (value->kind == HAILSTONE_INT) out->table[out->key] = value->i;
            if (value->kind == HAILSTONE_STRING) out->table[out->key] = value->s;
        };
        api.out_end = [](void*, HailstoneOut*) {};
        api.arg = [](void*, const HailstoneArgs* args, std::size_t index, HailstoneValue* out) {
            if (index >= args->values.size()) return 0;
            *out = {HAILSTONE_STRING, 0, 0, args->values[index].c_str()};
            return 1;
        };
    }

    ~Host() { dismiss(); }

    void open() {
        hailstone_plugin_set_host(&api);
        InitializePlugin();
        loaded = true;
        const auto& callback = commands.at("/hailwire");
        callback.function(callback.user, "");
    }

    void dismiss() {
        if (loaded) ShutdownPlugin();
        loaded = false;
    }

    void act(const std::string& name, const std::string& text = {}) const {
        const auto& callback = functions.at("action");
        const HailstoneArgs args{{name, text}};
        callback.function(callback.user, &args, nullptr);
    }

    Json state() const {
        const auto& callback = functions.at("state");
        HailstoneOut out{};
        check(callback.function(callback.user, nullptr, &out) == 1, "The panel did not return state");
        return out.table;
    }

    static Host& from(void* context) { return *static_cast<Host*>(context); }

    fs::path directory;
    std::string character = "Valorith";
    int game_state = GAMESTATE_INGAME;
    PluginHostApi api{};
    std::map<std::string, Callback<HailstoneCommandFn>> commands;
    std::map<std::string, Callback<HailstoneUiFn>> functions;
    bool loaded = false;
};

template <typename Predicate>
Json until(const Host& host, Predicate ready, const char* reason) {
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    do {
        OnPulse();
        auto snapshot = host.state();
        if (ready(snapshot)) return snapshot;
        std::this_thread::sleep_for(20ms);
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error(reason);
}
} // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 3, "Provide the fixture relay URL and test profile directory");
        check(hailstone_plugin_abi() == HAILSTONE_PLUGIN_ABI, "Incompatible Hailwire plugin ABI");
        Host game(argv[2]);
        game.open();
        check(game.state().at("online").get<bool>(), "Expected a logged-in player");
        game.act("relay", argv[1]);
        check(game.state().at("relay") == argv[1], "Could not save the fixture relay URL");
        game.act("link");
        const auto linking = until(game, [](const Json& s) { return s.at("code") != ""; }, "No link code arrived");
        std::cout << linking.at("code").get<std::string>() << std::endl;
        const auto linked = until(game, [](const Json& s) { return s.at("linked").get<bool>(); }, "Link did not complete");
        check(linked.at("label") == "Val", "Wrong Discord account label");
        check(!linked.at("active").get<bool>(), "Forwarding should start off");
        OnIncomingChat("Mira tells you, 'Forwarding is off'", 0);

        game.act("on");
        check(game.state().at("active").get<bool>(), "Could not enable the current character");
        OnIncomingChat("Mira tells you, 'Ready for the raid?'", 0);
        OnIncomingChat("You told Mira, 'Outgoing echo'", 0);
        OnIncomingChat("Mira tells the guild, 'Guild chat'", 0);
        OnIncomingChat("Banker tells you, 'Welcome to my bank!'", 0);
        until(game, [](const Json& s) { return s.at("accepted") == 1; }, "Tell did not reach the relay");

        game.character = "Altair";
        OnZoned();
        check(!game.state().at("active").get<bool>(), "Another character should use the off default");
        OnIncomingChat("Mira tells you, 'Alt should stay quiet'", 0);
        game.act("test");
        until(game, [](const Json& s) { return !s.at("busy").get<bool>() && !s.at("error").get<bool>(); }, "Test DM failed");

        game.dismiss();
        game.open();
        check(game.state().at("linked").get<bool>(), "Discord link did not survive plugin reload");
        game.character = "Valorith";
        OnZoned();
        check(game.state().at("active").get<bool>(), "Per-character settings did not survive plugin reload");
        game.act("unlink");
        until(game, [](const Json& s) { return !s.at("linked").get<bool>(); }, "Unlink failed");
        OnIncomingChat("Mira tells you, 'Unlinked accounts stay quiet'", 0);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
