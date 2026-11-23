// SPDX-License-Identifier: MIT
#include "hailstone_plugin.h"
#include "hailwire_build_config.h"
#include "json.hpp"
#include "tell.h"
#include "transport.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <string_view>
#include <thread>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

HAILSTONE_PLUGIN

namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

enum class Action { Start, Poll, Tell, Test, Unlink, Discard };
enum class Preference { Default, On, Off };

struct Job {
    Action action;
    std::uint64_t generation;
    hailwire::Request request;
    Clock::time_point due = Clock::now();
    unsigned attempts = 0;
};

struct Result {
    Action action;
    std::uint64_t generation;
    hailwire::Response response;
};

class Worker {
public:
    Worker() : thread_([this] { run(); }) {}

    ~Worker() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            jobs_.clear();
        }
        wake_.notify_one();
        thread_.join();
    }

    bool push(Job job) {
        {
            std::lock_guard lock(mutex_);
            if (jobs_.size() + results_.size() >= 48 && job.action == Action::Tell) return false;
            if (job.action == Action::Tell) jobs_.push_back(std::move(job));
            else jobs_.push_front(std::move(job));
        }
        wake_.notify_one();
        return true;
    }

    void clear(std::uint64_t generation) {
        {
            std::lock_guard lock(mutex_);
            generation_ = generation;
            jobs_.clear();
            results_.clear();
        }
        wake_.notify_one();
    }

    void drop_tells() {
        {
            std::lock_guard lock(mutex_);
            ++tell_epoch_;
            std::erase_if(jobs_, [](const Job& job) { return job.action == Action::Tell; });
        }
        wake_.notify_one();
    }

    std::deque<Result> results() {
        std::lock_guard lock(mutex_);
        return std::exchange(results_, {});
    }

    std::size_t pending_tells() {
        std::lock_guard lock(mutex_);
        return static_cast<std::size_t>(tell_in_flight_) +
               static_cast<std::size_t>(std::count_if(jobs_.begin(), jobs_.end(), [](const Job& job) {
                   return job.action == Action::Tell;
               }));
    }

private:
    void run() {
        std::unique_lock lock(mutex_);
        while (!stopping_) {
            auto next = jobs_.end();
            auto deadline = Clock::time_point::max();
            const auto now = Clock::now();
            for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
                if (it->due <= now) {
                    next = it;
                    break;
                }
                deadline = std::min(deadline, it->due);
            }
            if (next == jobs_.end()) {
                if (deadline == Clock::time_point::max()) wake_.wait(lock);
                else wake_.wait_until(lock, deadline);
                continue;
            }

            auto job = std::move(*next);
            jobs_.erase(next);
            const auto tell_epoch = tell_epoch_;
            tell_in_flight_ = job.action == Action::Tell;
            lock.unlock();
            hailwire::Response response;
            try { response = hailwire::post(job.request); }
            catch (...) { response.error = "Unable to reach the relay."; }
            lock.lock();
            tell_in_flight_ = false;
            if (stopping_ || job.generation != generation_) continue;
            const bool tell = job.action == Action::Tell;
            if (tell && tell_epoch != tell_epoch_) continue;
            const bool retryable = response.status == 0 || response.status == 429 || response.status >= 500;
            if (tell && retryable && job.attempts < 2) {
                ++job.attempts;
                const auto delay = std::max(static_cast<int>(job.attempts * 2), response.retry_after_seconds);
                job.due = Clock::now() + std::chrono::seconds(delay);
                jobs_.push_back(std::move(job));
                continue;
            }
            results_.push_back({job.action, job.generation, std::move(response)});
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    std::deque<Result> results_;
    bool stopping_ = false;
    bool tell_in_flight_ = false;
    std::uint64_t generation_ = 0;
    std::uint64_t tell_epoch_ = 0;
    std::thread thread_;
};

struct Account {
    std::string relay = HAILWIRE_RELAY_URL;
    std::string token;
    std::string label;
    std::string revision;
    bool all_characters = false;
    bool operator==(const Account&) const = default;
};

class SettingsLock {
public:
    explicit SettingsLock(const fs::path& file) {
#ifdef _WIN32
        handle_ = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
#else
        handle_ = open(file.c_str(), O_WRONLY | O_CREAT, 0600);
        if (handle_ >= 0 && flock(handle_, LOCK_EX | LOCK_NB) != 0) {
            close(handle_);
            handle_ = -1;
        }
#endif
    }

    explicit operator bool() const {
#ifdef _WIN32
        return handle_ != INVALID_HANDLE_VALUE;
#else
        return handle_ >= 0;
#endif
    }

    ~SettingsLock() {
        if (!*this) return;
#ifdef _WIN32
        CloseHandle(handle_);
#else
        close(handle_);
#endif
    }

    SettingsLock(const SettingsLock&) = delete;
    SettingsLock& operator=(const SettingsLock&) = delete;

private:
#ifdef _WIN32
    HANDLE handle_;
#else
    int handle_;
#endif
};

struct State {
    std::unique_ptr<Worker> worker;
    Account account;
    fs::path account_file;
    fs::path character_file;
    std::string character;
    std::string server;
    Preference preference = Preference::Default;
    bool open = false;
    bool online = false;
    bool bad_status = false;
    bool starting = false;
    bool polling = false;
    bool testing = false;
    bool unlinking = false;
    bool can_network = false;
    bool can_listen = false;
    std::string status;
    std::string code;
    std::string poll_token;
    std::string guild_url;
    std::string revoked_token;
    Clock::time_point deadline{};
    Clock::time_point poll_at{};
    Clock::time_point refresh_at{};
    std::uint64_t generation = 0;
    std::uint64_t accepted = 0;
    std::uint64_t failed = 0;
    std::string nonce;
    std::uint64_t message_count = 0;
};

State state;

template <std::size_t N>
std::string field(const char (&text)[N]) {
    return {text, std::find(text, text + N, '\0')};
}

std::string status_for(std::string_view error) {
    if (error == "discord_dm_unavailable") return "Discord could not deliver the DM. Check your Discord server and DM settings, then try again.";
    if (error == "invalid_device") return "This Discord link has expired or was removed. Link Discord again.";
    return "The relay could not complete the request. Try again in a moment.";
}

void set_status(std::string text, bool bad = false) {
    state.status = std::move(text);
    state.bad_status = bad;
}

std::string random_tag() {
    std::random_device random;
    char buffer[33];
    std::snprintf(buffer, sizeof buffer, "%08x%08x%08x%08x",
                  static_cast<unsigned>(random()), static_cast<unsigned>(random()),
                  static_cast<unsigned>(random()), static_cast<unsigned>(random()));
    return buffer;
}

std::string utc_now() {
    const std::time_t now = std::time(nullptr);
    std::tm date{};
#ifdef _WIN32
    if (gmtime_s(&date, &now) != 0) return {};
#else
    if (!gmtime_r(&now, &date)) return {};
#endif
    char buffer[32]{};
    std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%SZ", &date);
    return buffer;
}

std::string json_string(const Json& data, const char* key, std::size_t max_size) {
    if (!data.is_object()) return {};
    const auto it = data.find(key);
    if (it == data.end() || !it->is_string()) return {};
    const auto& value = it->get_ref<const std::string&>();
    if (value.size() > max_size || value.find('\0') != std::string::npos) return {};
    return value;
}

bool json_true(const Json& data, const char* key) {
    if (!data.is_object()) return false;
    const auto it = data.find(key);
    return it != data.end() && it->is_boolean() && it->get<bool>();
}

bool opaque(std::string_view token, std::size_t minimum) {
    return token.size() >= minimum && std::all_of(token.begin(), token.end(), [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
    });
}

Json load(const fs::path& path) {
    if (path.empty()) return {};
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::array<char, 4097> buffer{};
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    if (file.bad() || file.gcount() == static_cast<std::streamsize>(buffer.size())) return {};
    return Json::parse(buffer.data(), buffer.data() + file.gcount(), nullptr, false);
}

bool save(const fs::path& path, const Json& contents) {
    if (path.empty()) return false;
    auto temp = path;
    temp += "." + random_tag() + ".tmp";
    const auto payload = contents.dump(2) + "\n";
#ifdef _WIN32
    std::ofstream output(temp, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    output.close();
    const bool replaced = output && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    const int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return false;
    const auto written = write(fd, payload.data(), payload.size());
    const bool closed = close(fd) == 0;
    const bool replaced = closed && written == static_cast<ssize_t>(payload.size()) &&
                          ::rename(temp.c_str(), path.c_str()) == 0;
#endif
    if (!replaced) {
        std::error_code ignored;
        fs::remove(temp, ignored);
    }
    return replaced;
}

fs::path folder(int (*get)(void*, char*, size_t)) {
    char path[4096]{};
    const int length = get(host->host, path, sizeof path);
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof path) return {};
    return fs::path(std::string(path, path + length));
}

void clear_link_request() {
    state.starting = state.polling = false;
    state.code.clear();
    state.poll_token.clear();
    state.guild_url.clear();
}

void clear_requests() {
    state.worker->clear(++state.generation);
    state.testing = state.unlinking = false;
    clear_link_request();
}

bool active() {
    if (!state.online || !state.can_network || !state.can_listen || state.character_file.empty() ||
        state.account.token.empty() || state.unlinking) return false;
    if (state.preference == Preference::Default) return state.account.all_characters;
    return state.preference == Preference::On;
}

Account account_from_disk() {
    Account account;
    const auto data = load(state.account_file);
    auto relay = json_string(data, "relay", 512);
    if (hailwire::valid_relay_url(relay)) account.relay = std::move(relay);
    if (!hailwire::valid_relay_url(account.relay)) account.relay.clear();
    auto token = json_string(data, "device_token", 256);
    auto revision = json_string(data, "revision", 64);
    if (account.relay.empty() || token == state.revoked_token || !opaque(token, 24) ||
        !opaque(revision, 16)) return account;
    account.token = std::move(token);
    account.revision = std::move(revision);
    account.label = json_string(data, "discord_label", 128);
    account.all_characters = json_true(data, "all_characters");
    return account;
}

bool store_account(const Account& account, bool revoking = false) {
    if (state.account_file.empty()) {
        set_status("The Hailwire settings folder is unavailable.", true);
        return false;
    }
    auto lock_path = state.account_file;
    lock_path += ".lock";
    const SettingsLock lock(lock_path);
    if (!lock) {
        set_status("Could not open Discord settings. Try again in a moment.", true);
        return false;
    }
    const auto latest = account_from_disk();
    const bool same = revoking ? latest.token == state.account.token && latest.relay == state.account.relay
                              : latest == state.account;
    if (!same) {
        set_status("Discord settings changed in another Hailstone window. Try again.", true);
        return false;
    }
    if (save(state.account_file, {{"relay", account.relay}, {"device_token", account.token},
                                  {"revision", account.revision}, {"discord_label", account.label},
                                  {"all_characters", account.all_characters}})) return true;
    set_status("Hailwire could not save your Discord settings.", true);
    return false;
}

void reload_preference() {
    state.preference = Preference::Default;
    const auto data = load(state.character_file);
    if (state.account.revision.empty() || state.account.revision != json_string(data, "revision", 64)) return;
    const auto preference = json_string(data, "forwarding", 10);
    if (preference == "on") state.preference = Preference::On;
    if (preference == "off") state.preference = Preference::Off;
}

void reload_account() {
    const bool was_active = active();
    auto next = account_from_disk();
    if (next.token != state.account.token || next.relay != state.account.relay) {
        clear_requests();
        set_status(next.token.empty() ? "" : "Discord linked.");
    }
    state.account = std::move(next);
    reload_preference();
    if (was_active && !active()) state.worker->drop_tells();
}

void leave_world() {
    state.online = false;
    state.character.clear();
    state.server.clear();
    state.character_file.clear();
    state.preference = Preference::Default;
    state.worker->drop_tells();
}

void refresh_permissions() {
    const auto caps = host->caps(host->host);
    const bool network = (caps & HAILSTONE_CAP_NET) != 0;
    const bool listen = (caps & (HAILSTONE_CAP_READ | HAILSTONE_CAP_EVENTS)) ==
                        (HAILSTONE_CAP_READ | HAILSTONE_CAP_EVENTS);
    if (state.can_network && !network) clear_requests();
    if (state.can_listen && !listen) state.worker->drop_tells();
    state.can_network = network;
    state.can_listen = listen;
}

void refresh_character() {
    HailstonePlayer player{};
    player.size = sizeof player;
    if (!host->get_player(host->host, &player) || player.game_state != GAMESTATE_INGAME ||
            !player.in_zone || !player.name[0]) {
        if (player.game_state == GAMESTATE_INGAME) state.online = false;
        else if (!state.character_file.empty()) leave_world();
        return;
    }

    const auto dir = folder(host->character_config_dir);
    if (dir.empty()) {
        if (state.online) leave_world();
        return;
    }
    const auto character_file = dir / "forwarding.json";
    if (character_file == state.character_file) {
        state.online = true;
        return;
    }
    if (!state.character_file.empty()) leave_world();
    HailstoneServer server{};
    server.size = sizeof server;
    if (!host->server_info(host->host, &server)) return;
    state.server = server.world[0] ? field(server.world) : field(server.name);
    if (state.server.empty()) return;
    state.character_file = character_file;
    state.character = field(player.name);
    state.online = true;
    reload_preference();
}

bool queue(Action action, std::string_view path, const Json& data, std::string_view token = {}) {
    if (!state.can_network) return false;
    const auto payload = data.dump(-1, ' ', false, Json::error_handler_t::replace);
    return state.worker->push({action, state.generation, {state.account.relay, std::string(path), std::string(token), payload}});
}

std::string explain(const hailwire::Response& response, const Json& data) {
    if (response.status == 0) return "Could not reach the relay. Check the relay address and your connection.";
    if (response.status == 429) return "The relay is busy. Try again shortly.";
    return status_for(json_string(data, "error", 64));
}

void invalidate_link() {
    Account next;
    next.relay = state.account.relay;
    const auto latest = account_from_disk();
    if (latest.token != state.account.token || latest.relay != state.account.relay) {
        reload_account();
        return;
    }
    const auto revoked = state.account.token;
    if (!store_account(next, true)) {
        // A revoked token can never forward another tell, even if its file remains.
        state.revoked_token = revoked;
        clear_requests();
        state.account = std::move(next);
        return;
    }
    state.revoked_token = revoked;
    clear_requests();
    state.account = std::move(next);
    state.preference = Preference::Default;
}

void discard_link(const Account& account) {
    if (!state.can_network) return;
    // The credential belongs to the relay that created it, even when another window changed settings.
    state.worker->push({Action::Discard, state.generation, {account.relay, "/v1/unlink", account.token, "{}"}});
}

void handle(Result&& result) {
    if (result.generation != state.generation) return;
    const auto& response = result.response;
    const auto data = Json::parse(response.body, nullptr, false);
    const bool success = response.status >= 200 && response.status <= 299;
    const auto now = Clock::now();
    switch (result.action) {
    case Action::Discard:
        break;
    case Action::Start: {
        state.starting = false;
        if (!success) {
            set_status(explain(response, data), true);
            return;
        }
        auto code = json_string(data, "code", 20);
        auto token = json_string(data, "poll_token", 256);
        const auto expiry = data.is_object() ? data.find("expires_in") : data.end();
        if (!opaque(code, 4) || !opaque(token, 24) || expiry == data.end() || !expiry->is_number_integer()) {
            set_status("The relay returned an invalid linking response.", true);
            return;
        }
        state.code = std::move(code);
        state.poll_token = std::move(token);
        auto guild_url = json_string(data, "guild_url", 256);
        if (guild_url.starts_with("https://discord.com/") || guild_url.starts_with("https://discord.gg/")) {
            state.guild_url = std::move(guild_url);
        }
        state.deadline = now + std::chrono::seconds(std::clamp(expiry->get<int64_t>(), int64_t{10}, int64_t{900}));
        state.poll_at = now + 2s;
        set_status("");
        break;
    }
    case Action::Poll: {
        state.polling = false;
        state.poll_at = now + 2s;
        if (!success) {
            set_status(explain(response, data), true);
            return;
        }
        const auto status = json_string(data, "status", 24);
        if (status == "pending") return;
        if (status == "expired") {
            clear_link_request();
            set_status("The code expired. Select Link Discord for a new code.", true);
            return;
        }
        auto token = json_string(data, "device_token", 256);
        if (status != "linked" || !opaque(token, 24)) {
            clear_link_request();
            set_status("The relay returned an invalid linking response.", true);
            return;
        }
        Account next;
        next.relay = state.account.relay;
        next.token = std::move(token);
        next.label = json_string(data, "discord_label", 128);
        next.revision = random_tag();
        clear_link_request();
        const auto latest = account_from_disk();
        if (!latest.token.empty() || latest.relay != next.relay) {
            reload_account();
            discard_link(next);
            set_status("Discord settings changed in another Hailstone window.");
            return;
        }
        if (!store_account(next)) {
            discard_link(next);
            return;
        }
        state.account = std::move(next);
        reload_preference();
        set_status("Discord linked. Turn on forwarding to send tells.");
        break;
    }
    case Action::Tell:
        if (success) {
            ++state.accepted;
            set_status("Last tell accepted by the relay.");
        } else {
            ++state.failed;
            if (response.status == 401) invalidate_link();
            set_status(explain(response, data), true);
        }
        break;
    case Action::Test:
        state.testing = false;
        if (response.status == 401) invalidate_link();
        set_status(success ? "Test DM sent. Check your Discord messages." : explain(response, data), !success);
        break;
    case Action::Unlink:
        if (!state.unlinking) return;
        state.unlinking = false;
        if (success || response.status == 401) {
            invalidate_link();
            set_status("Discord unlinked.");
        } else {
            set_status(explain(response, data), true);
        }
        break;
    }
}

bool set_preference(Preference preference) {
    if (!state.online || state.account.token.empty() || state.unlinking) return false;
    const char* value = preference == Preference::On ? "on" : preference == Preference::Off ? "off" : "default";
    if (!save(state.character_file, {{"revision", state.account.revision}, {"forwarding", value}})) {
        set_status("Hailwire could not save forwarding for this character.", true);
        return false;
    }
    state.preference = preference;
    if (!active()) state.worker->drop_tells();
    set_status(active() ? "Forwarding is on for this character." : "Forwarding is off for this character.");
    return true;
}

bool action(std::string_view name, std::string text) {
    if (name == "hide") {
        state.open = false;
        return true;
    }
    if (name == "cancel" && (state.starting || !state.poll_token.empty())) {
        clear_requests();
        set_status("Link setup canceled.");
        return true;
    }

    reload_account();
    refresh_permissions();
    refresh_character();
    if (name == "relay") {
        if (!state.account.token.empty() || state.starting || !state.poll_token.empty()) return false;
        const auto first = text.find_first_not_of(" \t\r\n");
        const auto last = text.find_last_not_of(" \t\r\n");
        text = first == std::string::npos ? "" : text.substr(first, last - first + 1);
        if (text.size() > 512 || !hailwire::valid_relay_url(text)) {
            set_status("Enter the relay's HTTPS address from your server operator.", true);
            return false;
        }
        Account next;
        next.relay = std::move(text);
        if (!store_account(next)) return false;
        state.account = std::move(next);
        set_status("Relay address saved. Select Link Discord to continue.");
        return true;
    }

    if (name == "link" && state.account.token.empty() && !state.starting && state.poll_token.empty()) {
        if (!state.online || !state.can_network || state.account.relay.empty()) return false;
        state.starting = queue(Action::Start, "/v1/link/start", Json::object());
        set_status("Requesting a link code…");
        return state.starting;
    }

    if (state.account.token.empty() || state.unlinking) return false;
    if (name == "on") return set_preference(Preference::On);
    if (name == "off") return set_preference(Preference::Off);
    if (name == "default") return set_preference(Preference::Default);
    if (name == "all_on" || name == "all_off") {
        auto next = state.account;
        next.all_characters = name == "all_on";
        if (!store_account(next)) return false;
        state.account = std::move(next);
        if (!active()) state.worker->drop_tells();
        set_status(state.account.all_characters ? "Forwarding is on for characters using the default."
                                               : "Forwarding is off for characters using the default.");
        return true;
    }
    if (name == "test" && state.can_network && !state.testing) {
        state.testing = queue(Action::Test, "/v1/test", Json::object(), state.account.token);
        set_status("Sending a test DM…");
        return state.testing;
    }
    if (name == "unlink" && state.can_network && !state.testing) {
        state.worker->drop_tells();
        state.unlinking = queue(Action::Unlink, "/v1/unlink", Json::object(), state.account.token);
        set_status("Unlinking Discord…");
        return state.unlinking;
    }
    return false;
}

void output(HailstoneOut* out, const char* key, const char* value) {
    host->out_key(host->host, out, key);
    const HailstoneValue field{HAILSTONE_STRING, 0, 0, value};
    host->out_value(host->host, out, &field);
}

void output(HailstoneOut* out, const char* key, int kind, int64_t value) {
    host->out_key(host->host, out, key);
    const HailstoneValue field{kind, value, 0, nullptr};
    host->out_value(host->host, out, &field);
}

int panel_state(void*, const HailstoneArgs*, HailstoneOut* out) noexcept {
    try {
        if (!state.worker || !state.open) return 0;
        const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(state.deadline - Clock::now()).count();
        host->out_begin_table(host->host, out);
        output(out, "linked", HAILSTONE_BOOL, !state.account.token.empty());
        output(out, "online", HAILSTONE_BOOL, state.online);
        output(out, "active", HAILSTONE_BOOL, active());
        output(out, "events", HAILSTONE_BOOL, state.can_listen);
        output(out, "network", HAILSTONE_BOOL, state.can_network);
        output(out, "all", HAILSTONE_BOOL, state.account.all_characters);
        output(out, "override", HAILSTONE_BOOL, state.preference != Preference::Default);
        output(out, "starting", HAILSTONE_BOOL, state.starting);
        output(out, "busy", HAILSTONE_BOOL, state.testing || state.unlinking);
        output(out, "pending", HAILSTONE_INT, static_cast<int64_t>(state.worker->pending_tells()));
        output(out, "accepted", HAILSTONE_INT, static_cast<int64_t>(state.accepted));
        output(out, "failed", HAILSTONE_INT, static_cast<int64_t>(state.failed));
        output(out, "remaining", HAILSTONE_INT, std::max<int64_t>(0, remaining));
        output(out, "error", HAILSTONE_BOOL, state.bad_status);
        output(out, "relay", state.account.relay.c_str());
        output(out, "label", state.account.label.c_str());
        output(out, "character", state.character.c_str());
        output(out, "server", state.server.c_str());
        output(out, "status", state.status.c_str());
        output(out, "code", state.code.c_str());
        output(out, "guild_url", state.guild_url.c_str());
        host->out_end(host->host, out);
        return 1;
    } catch (...) { return 0; }
}

std::string argument(const HailstoneArgs* args, std::size_t index, std::size_t max) {
    HailstoneValue value{};
    if (!host->arg(host->host, args, index, &value) || value.kind != HAILSTONE_STRING || !value.s) return {};
    const std::string_view text(value.s);
    return text.size() <= max ? std::string(text) : std::string{};
}

int panel_action(void*, const HailstoneArgs* args, HailstoneOut*) noexcept {
    try {
        if (state.worker) action(argument(args, 0, 20), argument(args, 1, 512));
    } catch (...) { set_status("Hailwire could not complete the action.", true); }
    return 0;
}

void command(void*, const char* args) noexcept {
    try {
        if (!state.worker) return;
        const std::string_view name = args ? args : "";
        if (name.empty() || name == "show") state.open = true;
        else if (name == "hide") state.open = false;
        else if (name == "on" || name == "off" || name == "test") {
            action(name, {});
            state.open = true;
        } else {
            host->write_chat(host->host, "Hailwire: /hailwire opens the panel. Use on, off, or test for shortcuts.", 0);
        }
    } catch (...) {}
}

bool ascii_same(std::string_view a, std::string_view b) {
    const auto lower = [](char ch) { return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch; };
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [&](char x, char y) { return lower(x) == lower(y); });
}

bool visible_npc(std::string_view sender) {
    if (!HAILSTONE_HOST_HAS(host, next_spawn) || sender.find('.') != std::string_view::npos) return false;
    HailstoneSpawn spawn{};
    spawn.size = sizeof spawn;
    std::uint32_t after = 0;
    while (host->next_spawn(host->host, after, &spawn) && spawn.id > after) {
        if (spawn.is_npc && (ascii_same(sender, field(spawn.name)) ||
                             ascii_same(sender, field(spawn.display_name)))) return true;
        after = spawn.id;
    }
    return false;
}

bool local_bank_line(std::string_view line, const std::string& sender) {
    if (!HAILSTONE_HOST_HAS(host, eqstr)) return false;
    const char* args[] = {sender.c_str()};
    char text[512];
    for (const std::uint32_t string_id : {1465u, 12143u}) {
        const int length = host->eqstr(host->host, string_id, args, 1, text, sizeof text);
        if (length > 0 && static_cast<std::size_t>(length) < sizeof text &&
            line == std::string_view(text, static_cast<std::size_t>(length))) return true;
    }
    return false;
}

void forward(hailwire::Tell tell) {
    if (!active() || tell.message.empty() || tell.message.size() > 2048) return;
    const auto id = state.nonce + "-" + std::to_string(++state.message_count);
    if (!queue(Action::Tell, "/v1/tells",
               {{"server", state.server}, {"character", state.character}, {"sender", tell.sender},
                {"message", tell.message}, {"id", id}, {"timestamp", utc_now()}}, state.account.token)) {
        ++state.failed;
        set_status("The relay queue is full. A tell was skipped.", true);
    }
}

} // namespace

HAILSTONE_API void InitializePlugin() noexcept {
    try {
        if (!HAILSTONE_HOST_HAS(host, server_info) || !HAILSTONE_HOST_HAS(host, character_config_dir) ||
            !HAILSTONE_HOST_HAS(host, add_ui_function) || !HAILSTONE_HOST_HAS(host, config_dir) ||
            !HAILSTONE_HOST_HAS(host, get_player) || !HAILSTONE_HOST_HAS(host, caps)) return;
        state = State{};
        const auto root = folder(host->config_dir);
        if (!root.empty()) state.account_file = root / "discord.json";
        state.worker = std::make_unique<Worker>();
        state.nonce = random_tag();
        refresh_permissions();
        reload_account();
        host->add_command(host->host, "/hailwire", command, nullptr);
        host->add_ui_function(host->host, "state", panel_state, nullptr);
        host->add_ui_function(host->host, "action", panel_action, nullptr);
        if (HAILSTONE_HOST_HAS(host, add_menu)) host->add_menu(host->host, "Hailwire", "/hailwire");
        refresh_character();
    } catch (...) { state.worker.reset(); }
}

HAILSTONE_API void ShutdownPlugin() noexcept {
    if (state.worker) {
        host->remove_command(host->host, "/hailwire");
        host->remove_ui_function(host->host, "state");
        host->remove_ui_function(host->host, "action");
    }
    state = State{};
}

HAILSTONE_API void OnPulse() noexcept {
    try {
        if (!state.worker) return;
        refresh_permissions();
        const auto now = Clock::now();
        if (now >= state.refresh_at) {
            reload_account();
            refresh_character();
            state.refresh_at = now + 1s;
        }
        for (auto& result : state.worker->results()) handle(std::move(result));
        if (state.poll_token.empty() || state.polling) return;
        if (now >= state.deadline) {
            clear_link_request();
            set_status("The code expired. Select Link Discord for a new code.", true);
        } else if (now >= state.poll_at) {
            state.polling = queue(Action::Poll, "/v1/link/poll", {{"poll_token", state.poll_token}});
        }
    } catch (...) { set_status("Hailwire could not refresh its connection.", true); }
}

HAILSTONE_API void SetGameState(int game_state) noexcept {
    try {
        if (!state.worker) return;
        if (game_state == GAMESTATE_INGAME) refresh_character();
        else leave_world();
    } catch (...) {}
}

HAILSTONE_API void OnBeginZone() noexcept {
    try {
        if (state.worker) state.online = false;
    } catch (...) {}
}

HAILSTONE_API void OnZoned() noexcept {
    try {
        if (state.worker) refresh_character();
    } catch (...) {}
}

HAILSTONE_API int OnIncomingChat(const char* line, uint32_t) noexcept {
    try {
        if (!state.worker || !line || state.account.token.empty()) return 0;
        refresh_permissions();
        auto tell = hailwire::parse_tell(line);
        if (!tell) return 0;
        reload_account();
        refresh_character();
        if (!active() || visible_npc(tell->sender) || local_bank_line(line, tell->sender)) return 0;
        forward(std::move(*tell));
    } catch (...) {}
    return 0;
}
