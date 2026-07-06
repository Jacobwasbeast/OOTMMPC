// OoTMM dedicated multiworld server: standalone console host for the relay. Rooms clients
// by seed and forwards item packets; persists each session's check/item history in
// world_state/ next to the exe and replays it to joining clients (progress lives on the
// SERVER). Config from server.properties (auto-created next to the exe), overridable per run.
// Admin commands are typed into the console; /help lists them.
//
//   Usage: ootmm_server [port] [--pvp] [--death-link]
//
// server.properties keys:
//   port=42801           TCP port the relay listens on
//   pvp=false            player-vs-player combat for every session on this server
//   death-link=false     one player's death kills every player in the session
//   timer-enabled=true   whether launchers may offer the game-speed slider
//   timer-min=0          lowest game-speed percentage the slider allows
//   timer-max=250        highest game-speed percentage the slider allows

#include "RelayServer.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {

namespace fs = std::filesystem;

struct ServerConfig {
    uint16_t port = ootmm::relay::kDefaultRelayPort;
    bool pvp = false;
    bool deathLink = false;
    bool timerEnabled = true;
    int timerMin = 0;
    int timerMax = 250;
};

std::string Trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return s.substr(b, e - b);
}

std::string ToLower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// Resolve server.properties next to the exe (falls back to CWD if argv[0] is bare).
fs::path PropertiesPath(const char* argv0) {
    std::error_code ec;
    fs::path exe = argv0 != nullptr ? fs::path(argv0) : fs::path();
    if (exe.has_parent_path()) {
        return exe.parent_path() / "server.properties";
    }
    return fs::current_path(ec) / "server.properties";
}

void WriteDefaultProperties(const fs::path& path, const ServerConfig& config) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return;
    }
    out << "# OoTMM multiworld server settings\n"
           "# Players connect by entering <this machine's address>:<port> in the\n"
           "# launcher's multiworld host box. Clients are roomed by seed, so one\n"
           "# server can host any number of concurrent multiworld sessions.\n"
           "\n"
           "# TCP port the relay listens on (1-65535).\n"
           "port="
        << config.port
        << "\n"
           "\n"
           "# Player-vs-player combat for every session on this server: attacks\n"
           "# damage and knock back other players. This is a server rule; players\n"
           "# cannot override it from their launchers.\n"
           "pvp="
        << (config.pvp ? "true" : "false")
        << "\n"
           "\n"
           "# Death link: when one player dies, every player in the session dies.\n"
           "# A server rule; players cannot override it.\n"
           "death-link="
        << (config.deathLink ? "true" : "false")
        << "\n"
           "\n"
           "# Timer (game speed) feature: when enabled, each launcher offers a game-speed\n"
           "# slider in its options sidebar, constrained to [timer-min, timer-max] percent\n"
           "# (100 = vanilla speed). Disable to lock every session at 100%.\n"
           "timer-enabled="
        << (config.timerEnabled ? "true" : "false")
        << "\n"
           "timer-min="
        << config.timerMin
        << "\n"
           "timer-max="
        << config.timerMax << "\n";
}

// Load server.properties (create with defaults if missing); unknown keys warn, malformed values keep defaults.
ServerConfig LoadProperties(const fs::path& path) {
    ServerConfig config;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        WriteDefaultProperties(path, config);
        std::cout << "[server] created default settings file: " << path.string() << std::endl;
        return config;
    }
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
            continue;
        }
        const size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            std::cerr << "[server] server.properties line " << lineNo << ": expected key=value, got '" << trimmed
                      << "'" << std::endl;
            continue;
        }
        const std::string key = ToLower(Trim(trimmed.substr(0, eq)));
        const std::string value = Trim(trimmed.substr(eq + 1));
        if (key == "port") {
            char* end = nullptr;
            const long parsed = std::strtol(value.c_str(), &end, 10);
            if (end == nullptr || *end != '\0' || parsed < 1 || parsed > 65535) {
                std::cerr << "[server] server.properties line " << lineNo << ": invalid port '" << value
                          << "' (expected 1-65535); using " << config.port << std::endl;
                continue;
            }
            config.port = static_cast<uint16_t>(parsed);
        } else if (key == "pvp" || key == "death-link" || key == "timer-enabled") {
            const std::string lowered = ToLower(value);
            bool parsed = false;
            bool valid = true;
            if (lowered == "true" || lowered == "1" || lowered == "yes" || lowered == "on") {
                parsed = true;
            } else if (lowered == "false" || lowered == "0" || lowered == "no" || lowered == "off") {
                parsed = false;
            } else {
                valid = false;
                std::cerr << "[server] server.properties line " << lineNo << ": invalid " << key << " value '"
                          << value << "' (expected true/false)" << std::endl;
            }
            if (valid) {
                if (key == "pvp") {
                    config.pvp = parsed;
                } else if (key == "death-link") {
                    config.deathLink = parsed;
                } else {
                    config.timerEnabled = parsed;
                }
            }
        } else if (key == "timer-min" || key == "timer-max") {
            char* end = nullptr;
            const long parsed = std::strtol(value.c_str(), &end, 10);
            if (end == nullptr || *end != '\0' || parsed < 0 || parsed > 250) {
                std::cerr << "[server] server.properties line " << lineNo << ": invalid " << key << " value '"
                          << value << "' (expected 0-250)" << std::endl;
            } else if (key == "timer-min") {
                config.timerMin = static_cast<int>(parsed);
            } else {
                config.timerMax = static_cast<int>(parsed);
            }
        } else {
            std::cerr << "[server] server.properties line " << lineNo << ": unknown key '" << key << "'" << std::endl;
        }
    }
    return config;
}

} // namespace

int main(int argc, char** argv) {
    const fs::path propsPath = PropertiesPath(argc > 0 ? argv[0] : nullptr);
    ServerConfig config = LoadProperties(propsPath);
    bool portSet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help" || arg == "/?") {
            std::cout << "OoTMM dedicated multiworld server\n"
                         "usage: ootmm_server [port] [--pvp] [--death-link]\n\n"
                         "Settings load from server.properties next to the executable (created\n"
                         "with defaults on first run); command-line arguments override it for\n"
                         "this run only.\n\n"
                         "  port          TCP port the relay listens on (properties default "
                      << ootmm::relay::kDefaultRelayPort
                      << ")\n"
                         "  --pvp         player-vs-player combat for every session (server rule)\n"
                         "  --death-link  one player's death kills everyone (server rule)\n\n"
                         "While running, admin commands are typed into this console; /help lists\n"
                         "them (/list-sessions, /list-checks, /unlock-check, /unlock-all,\n"
                         "/teleport, ...).\n";
            return 0;
        }
        if (arg == "--pvp") {
            config.pvp = true;
            continue;
        }
        if (arg == "--death-link") {
            config.deathLink = true;
            continue;
        }
        if (portSet) {
            std::cerr << "ootmm_server: unexpected argument '" << arg << "'" << std::endl;
            std::cerr << "usage: ootmm_server [port] [--pvp] [--death-link]" << std::endl;
            return 2;
        }
        char* end = nullptr;
        const long value = std::strtol(arg.c_str(), &end, 10);
        if (end == nullptr || *end != '\0' || value < 1 || value > 65535) {
            std::cerr << "ootmm_server: invalid port '" << arg << "' (expected 1-65535)" << std::endl;
            return 2;
        }
        config.port = static_cast<uint16_t>(value);
        portSet = true;
    }

    std::cout << "OoTMM dedicated multiworld server" << std::endl;
    std::cout << "[server] type /help for admin commands" << std::endl;

    static ootmm::relay::RelayRules rules;
    rules.pvp.store(config.pvp);
    rules.deathLink.store(config.deathLink);
    rules.timerEnabled.store(config.timerEnabled);
    rules.timerMinPercent.store(config.timerMin);
    rules.timerMaxPercent.store(config.timerMax);

    // Console reader: forwards each typed line to the relay thread (executed between selects).
    std::thread console([] {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (!line.empty()) {
                ootmm::relay::SubmitRelayCommand(line);
            }
        }
    });
    console.detach();

    // Durable per-session check/item history lives next to the executable.
    const fs::path storeDir = propsPath.parent_path() / "world_state";
    return ootmm::relay::RunRelayServer(config.port, rules, storeDir);
}
