#include "RelayServer.hpp"

#include "ootmm/AnchorBridge.hpp"

#include <cctype>
#include <chrono>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ootmm::relay {

#ifdef _WIN32
static_assert(sizeof(SocketHandle) == sizeof(SOCKET), "SocketHandle must alias winsock's SOCKET");
static_assert(kInvalidSocket == static_cast<SocketHandle>(INVALID_SOCKET),
              "kInvalidSocket must equal INVALID_SOCKET");

void CloseSocket(SocketHandle s) {
    closesocket(static_cast<SOCKET>(s));
}

bool SocketWouldBlock() {
    return WSAGetLastError() == WSAEWOULDBLOCK;
}

void SetNonBlocking(SocketHandle s) {
    u_long mode = 1;
    ioctlsocket(static_cast<SOCKET>(s), FIONBIO, &mode);
}

bool InitSockets() {
    static bool initialized = false;
    if (!initialized) {
        WSADATA data{};
        initialized = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    return initialized;
}
#else
void CloseSocket(SocketHandle s) {
    close(s);
}

bool SocketWouldBlock() {
    return errno == EWOULDBLOCK || errno == EAGAIN;
}

void SetNonBlocking(SocketHandle s) {
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
}

bool InitSockets() {
    return true;
}
#endif

std::string ExtractJsonStringField(const std::string& line, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    const size_t start = line.find(needle);
    if (start == std::string::npos) {
        return {};
    }
    size_t cursor = start + needle.size();
    while (cursor < line.size() && (line[cursor] == ' ' || line[cursor] == '\t')) {
        ++cursor;
    }
    if (cursor >= line.size() || line[cursor] != ':') {
        return {};
    }
    ++cursor;
    while (cursor < line.size() && (line[cursor] == ' ' || line[cursor] == '\t')) {
        ++cursor;
    }
    if (cursor >= line.size() || line[cursor] != '"') {
        return {};
    }
    const size_t valueStart = cursor + 1;
    const size_t end = line.find('"', valueStart);
    if (end == std::string::npos) {
        return {};
    }
    return line.substr(valueStart, end - valueStart);
}

// Reads an unquoted numeric field ("playerId":3). Returns fallback when absent/non-numeric.
int ExtractJsonNumberField(const std::string& line, const std::string& key, int fallback) {
    const std::string needle = "\"" + key + "\"";
    const size_t start = line.find(needle);
    if (start == std::string::npos) {
        return fallback;
    }
    size_t cursor = start + needle.size();
    while (cursor < line.size() && (line[cursor] == ' ' || line[cursor] == '\t' || line[cursor] == ':')) {
        ++cursor;
    }
    const size_t valueStart = cursor;
    while (cursor < line.size() && (std::isdigit(static_cast<unsigned char>(line[cursor])) != 0 || line[cursor] == '-')) {
        ++cursor;
    }
    if (cursor == valueStart) {
        return fallback;
    }
    try {
        return std::stoi(line.substr(valueStart, cursor - valueStart));
    } catch (...) {
        return fallback;
    }
}

int RunRelayServer(uint16_t port, bool pvp) {
    // Convenience wrapper for callers with a fixed rule (dedicated server, --serve).
    static std::atomic<bool> fixedRule{ false };
    fixedRule.store(pvp);
    return RunRelayServer(port, fixedRule);
}

int RunRelayServerImpl(uint16_t port, const RelayRules& rules, const std::filesystem::path& storeDir,
                       const std::atomic<bool>* followedPvp);

int RunRelayServer(uint16_t port, const std::atomic<bool>& pvp) {
    // Legacy single-rule entry (the launcher's host checkbox): the pvp atomic stays LIVE (the
    // loop re-reads it via followedPvp each iteration); other rules keep their defaults.
    static RelayRules rules;
    return RunRelayServerImpl(port, rules, {}, &pvp);
}

int RunRelayServer(uint16_t port, const RelayRules& rules, const std::filesystem::path& storeDir) {
    return RunRelayServerImpl(port, rules, storeDir, nullptr);
}

// --- Console command queue ----------------------------------------------------------------------
namespace {
std::mutex gCommandMutex;
std::deque<std::string> gCommandQueue;
} // namespace

void SubmitRelayCommand(const std::string& line) {
    std::lock_guard<std::mutex> lock(gCommandMutex);
    gCommandQueue.push_back(line);
}

// --- Persistent per-room packet store -------------------------------------------------------------
// Check/item packets are the durable multiworld state: the relay keeps every unique
// OOTMM_CHECK_COMPLETE / OOTMM_GIVE_ITEM line per room, mirrors to disk, and replays the room's
// history to every joiner — so the SERVER owns check state and a fresh native save gets it all back.
namespace {

struct RoomStore {
    std::vector<std::string> lines;       // insertion order (replay order matters for grants)
    std::set<std::string> seen;           // dedup
    bool loaded = false;
};

bool IsPersistedPacketType(const std::string& type) {
    return type == ootmm::anchor::PacketCheckComplete || type == ootmm::anchor::PacketGiveItem;
}

std::filesystem::path RoomStorePath(const std::filesystem::path& dir, const std::string& room) {
    std::string safe;
    for (const char c : room) {
        safe += (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_') ? c : '_';
    }
    return dir / ("room_" + safe + ".jsonl");
}

void LoadRoomStore(RoomStore& store, const std::filesystem::path& dir, const std::string& room) {
    if (store.loaded) {
        return;
    }
    store.loaded = true;
    if (dir.empty()) {
        return;
    }
    std::ifstream in(RoomStorePath(dir, room), std::ios::binary);
    std::string line;
    while (in && std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty() && store.seen.insert(line).second) {
            store.lines.push_back(line);
        }
    }
}

// Appends a line to the store (memory + disk). Returns true when the line was new.
bool AppendRoomStore(RoomStore& store, const std::filesystem::path& dir, const std::string& room,
                     const std::string& line) {
    if (!store.seen.insert(line).second) {
        return false;
    }
    store.lines.push_back(line);
    if (!dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::ofstream out(RoomStorePath(dir, room), std::ios::binary | std::ios::app);
        if (out) {
            out << line << "\n";
        }
    }
    return true;
}

// Removes every stored line containing this checkId and rewrites the disk file (/lock-check).
size_t EraseRoomStoreCheck(RoomStore& store, const std::filesystem::path& dir, const std::string& room,
                           const std::string& checkId) {
    const std::string needle = "\"" + checkId + "\"";
    size_t removed = 0;
    std::vector<std::string> kept;
    kept.reserve(store.lines.size());
    for (const std::string& line : store.lines) {
        if (line.find(needle) != std::string::npos) {
            store.seen.erase(line);
            ++removed;
        } else {
            kept.push_back(line);
        }
    }
    store.lines.swap(kept);
    if (removed > 0 && !dir.empty()) {
        std::ofstream out(RoomStorePath(dir, room), std::ios::binary | std::ios::trunc);
        for (const std::string& line : store.lines) {
            out << line << "\n";
        }
    }
    return removed;
}

} // namespace

int RunRelayServerImpl(uint16_t port, const RelayRules& rules, const std::filesystem::path& storeDir,
                       const std::atomic<bool>* followedPvp) {
    if (!InitSockets()) {
        std::cerr << "[relay] socket init failed" << std::endl;
        return 1;
    }
    SocketHandle listener = static_cast<SocketHandle>(socket(AF_INET, SOCK_STREAM, 0));
    if (listener == kInvalidSocket) {
        std::cerr << "[relay] cannot create listen socket" << std::endl;
        return 1;
    }
    const int enable = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enable), sizeof(enable));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listener, 8) != 0) {
        std::cerr << "[relay] cannot bind/listen on port " << port << std::endl;
        CloseSocket(listener);
        return 1;
    }
    const auto effPvp = [&]() { return followedPvp != nullptr ? followedPvp->load() : rules.pvp.load(); };
    std::cout << "[relay] multiworld relay listening on port " << port << (effPvp() ? " (PvP enabled)" : "")
              << (rules.deathLink.load() ? " (death link enabled)" : "") << std::endl;
    if (!storeDir.empty()) {
        std::cout << "[relay] check state persists in " << storeDir.string() << std::endl;
    }
    // Session rules announced to every joiner and re-broadcast periodically; launchers mirror
    // them into their games live. The refresh makes the rules converge even when the one-shot
    // join line is lost (reconnect race) or the host re-toggles a rule while the relay runs.
    const auto sessionMetaLine = [&]() {
        std::ostringstream out;
        out << "{\"type\":\"OOTMM_SESSION_META\",\"pvp\":" << (effPvp() ? "true" : "false")
            << ",\"deathLink\":" << (rules.deathLink.load() ? "true" : "false")
            << ",\"timerEnabled\":" << (rules.timerEnabled.load() ? "true" : "false")
            << ",\"timerMin\":" << rules.timerMinPercent.load() << ",\"timerMax\":" << rules.timerMaxPercent.load()
            << "}\n";
        return out.str();
    };
    std::string lastMeta = sessionMetaLine();
    auto nextMetaBroadcast = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    struct RelayClient {
        SocketHandle sock;
        std::string room;   // seedId from the handshake; empty until handshaken
        std::string buffer; // partial-line receive buffer
        std::string name;
        int playerId = 0;   // multiworld player id from the handshake (0 = unknown/solo)
    };
    std::vector<RelayClient> clients;
    std::map<std::string, RoomStore> rooms; // durable check/item history per seed room

    // Per-player full check catalog (every checkId in that player's world), uploaded on join so
    // /list-checks can show the COMPLETE set, not just collected checks. In-memory only (not
    // replayed to joiners, not persisted) — players re-upload on reconnect. Keyed room -> playerId.
    struct PlayerCatalog {
        std::string name;
        std::vector<std::string> checkIds;
    };
    std::map<std::string, std::map<int, PlayerCatalog>> catalogs;

    // Broadcasts a fabricated line to a room (console commands) and, when it is a persisted
    // type, records it so future joiners replay it too.
    const auto injectRoomLine = [&](const std::string& room, const std::string& line) {
        RoomStore& store = rooms[room];
        LoadRoomStore(store, storeDir, room);
        if (IsPersistedPacketType(ExtractJsonStringField(line, "type"))) {
            AppendRoomStore(store, storeDir, room, line);
        }
        const std::string wire = line + "\n";
        int sentTo = 0;
        for (const RelayClient& c : clients) {
            if (c.room == room) {
                send(c.sock, wire.data(), static_cast<int>(wire.size()), 0);
                ++sentTo;
            }
        }
        std::cout << "[relay] injected into room " << room << " (" << sentTo << " clients): " << line << std::endl;
    };

    const auto runCommand = [&](const std::string& raw) {
        std::istringstream in(raw);
        std::string cmd;
        in >> cmd;
        if (cmd.empty()) {
            return;
        }
        if (cmd == "/help") {
            std::cout << "[relay] commands:\n"
                         "  /help                                     this list\n"
                         "  /list-sessions                            active rooms (session ids)\n"
                         "  /list-players [session-id]                connected players\n"
                         "  /list-checks <session-id> [player]        all checks + status; player =\n"
                         "                                            username, player-id=N, or N\n"
                         "  /unlock-check <session-id> <check-id>     mark a check completed for everyone\n"
                         "  /lock-check <session-id> <check-id>       forget a stored check (fresh saves\n"
                         "                                            no longer receive it)\n"
                         "  /unlock-all <session-id> <who> [game]     unlock every check for a player;\n"
                         "                                            who = username, player-id=N, or all;\n"
                         "                                            game = oot, mm, or all (default all)\n"
                         "  /teleport <session-id> <who> <dest>       teleport a player; <who> is a client\n"
                         "                                            name or player-id=N; <dest> is a client\n"
                         "                                            name, player-id=N, or entrance=0xNNNN\n"
                         "  /say <session-id> <message...>            broadcast a chat line to a room\n"
                      << std::endl;
            return;
        }
        if (cmd == "/list-sessions") {
            std::set<std::string> active;
            for (const RelayClient& c : clients) {
                if (!c.room.empty()) {
                    active.insert(c.room);
                }
            }
            for (const auto& [room, store] : rooms) {
                active.insert(room);
            }
            std::cout << "[relay] " << active.size() << " session(s):" << std::endl;
            for (const std::string& room : active) {
                size_t online = 0;
                for (const RelayClient& c : clients) {
                    if (c.room == room) {
                        ++online;
                    }
                }
                const auto it = rooms.find(room);
                const size_t stored = it != rooms.end() ? it->second.lines.size() : 0;
                std::cout << "  " << room << "  (" << online << " online, " << stored << " stored packets)"
                          << std::endl;
            }
            return;
        }
        if (cmd == "/list-players") {
            std::string room;
            in >> room;
            for (const RelayClient& c : clients) {
                if (!c.room.empty() && (room.empty() || c.room == room)) {
                    std::cout << "  '" << c.name << "' in " << c.room << std::endl;
                }
            }
            return;
        }
        if (cmd == "/list-checks") {
            std::string room;
            std::string playerArg;
            in >> room >> playerArg;
            if (room.empty()) {
                std::cout << "[relay] usage: /list-checks <session-id> [player]  "
                             "(player = username, player-id=N, or N)"
                          << std::endl;
                return;
            }
            RoomStore& store = rooms[room];
            LoadRoomStore(store, storeDir, room);
            // Completed set per collecting player (sourcePlayer): checkId -> the player who finished it.
            std::map<int, std::set<std::string>> completedByPlayer;
            for (const std::string& line : store.lines) {
                if (ExtractJsonStringField(line, "type") == ootmm::anchor::PacketCheckComplete) {
                    const int src = ExtractJsonNumberField(line, "sourcePlayer", 0);
                    completedByPlayer[src].insert(ExtractJsonStringField(line, "checkId"));
                }
            }
            const auto& roomCatalogs = catalogs[room];
            // Resolve the optional player filter to a player id (0 = all).
            int filterPid = 0;
            if (!playerArg.empty()) {
                if (playerArg.rfind("player-id=", 0) == 0) {
                    filterPid = std::atoi(playerArg.c_str() + 10);
                } else if (playerArg.find_first_not_of("0123456789") == std::string::npos) {
                    filterPid = std::atoi(playerArg.c_str());
                } else {
                    for (const auto& [pid, cat] : roomCatalogs) {
                        if (cat.name == playerArg) {
                            filterPid = pid;
                            break;
                        }
                    }
                    for (const RelayClient& c : clients) {
                        if (c.room == room && c.name == playerArg) {
                            filterPid = c.playerId;
                        }
                    }
                    if (filterPid == 0) {
                        std::cout << "[relay] no player named '" << playerArg << "' in " << room << std::endl;
                        return;
                    }
                }
            }
            if (filterPid == 0) {
                // Summary across all known players (catalog upload required for totals).
                if (roomCatalogs.empty()) {
                    // No catalog yet — fall back to just the stored completions.
                    size_t total = 0;
                    for (const auto& [src, done] : completedByPlayer) {
                        std::cout << "  player " << src << ": " << done.size() << " completed" << std::endl;
                        total += done.size();
                    }
                    std::cout << "[relay] " << total << " completed check(s) stored for " << room
                              << " (no catalog uploaded — connect a client for full totals)" << std::endl;
                    return;
                }
                for (const auto& [pid, cat] : roomCatalogs) {
                    const auto doneIt = completedByPlayer.find(pid);
                    const size_t done = doneIt != completedByPlayer.end() ? doneIt->second.size() : 0;
                    std::cout << "  player " << pid << " ('" << cat.name << "'): " << done << "/"
                              << cat.checkIds.size() << " checks" << std::endl;
                }
                std::cout << "[relay] use /list-checks " << room << " <player> for the full check list"
                          << std::endl;
                return;
            }
            // Full per-check listing for one player.
            const auto catIt = roomCatalogs.find(filterPid);
            const std::set<std::string>& done = completedByPlayer[filterPid];
            if (catIt == roomCatalogs.end()) {
                std::cout << "[relay] no catalog for player " << filterPid << " in " << room
                          << " (that player has not connected this session) — showing "
                          << done.size() << " stored completion(s):" << std::endl;
                for (const std::string& id : done) {
                    std::cout << "  [x] " << id << std::endl;
                }
                return;
            }
            size_t completed = 0;
            for (const std::string& id : catIt->second.checkIds) {
                const bool got = done.count(id) != 0;
                if (got) {
                    ++completed;
                }
                std::cout << "  " << (got ? "[x] " : "[ ] ") << id << std::endl;
            }
            std::cout << "[relay] player " << filterPid << " ('" << catIt->second.name << "'): " << completed << "/"
                      << catIt->second.checkIds.size() << " checks in " << room << std::endl;
            return;
        }
        if (cmd == "/unlock-check" || cmd == "/lock-check") {
            std::string room;
            std::string checkId;
            in >> room >> checkId;
            if (room.empty() || checkId.empty()) {
                std::cout << "[relay] usage: " << cmd << " <session-id> <ootmm-check-id>" << std::endl;
                return;
            }
            if (cmd == "/unlock-check") {
                // The same line a client emits when it collects the check; every client (and
                // every future joiner, via the store) treats it like a real collection.
                injectRoomLine(room, std::string("{\"type\":\"") + ootmm::anchor::PacketCheckComplete +
                                         "\",\"seedId\":\"" + room + "\",\"checkId\":\"" + checkId +
                                         "\",\"admin\":true}");
            } else {
                RoomStore& store = rooms[room];
                LoadRoomStore(store, storeDir, room);
                const size_t removed = EraseRoomStoreCheck(store, storeDir, room, checkId);
                std::cout << "[relay] removed " << removed << " stored packet(s) for " << checkId
                          << " (connected clients keep their local state until a fresh save)" << std::endl;
            }
            return;
        }
        if (cmd == "/unlock-all") {
            std::string room;
            std::string who;
            std::string game;
            in >> room >> who >> game;
            if (room.empty() || who.empty()) {
                std::cout << "[relay] usage: /unlock-all <session-id> <username|player-id=N|all> [oot|mm|all]"
                          << std::endl;
                return;
            }
            if (game.empty()) {
                game = "all";
            }
            if (game != "oot" && game != "mm" && game != "all") {
                std::cout << "[relay] game scope must be oot, mm, or all" << std::endl;
                return;
            }
            // The game owns the seed + the collect pipeline, so it performs the unlock (marks every
            // scoped check complete, delivers owned items, broadcasts CheckComplete to the relay).
            // The launcher routes this to the addressed player(s), exactly like /teleport.
            injectRoomLine(room, std::string("{\"type\":\"OOTMM_ADMIN_UNLOCK_ALL\",\"seedId\":\"") + room +
                                     "\",\"who\":\"" + who + "\",\"game\":\"" + game + "\"}");
            std::cout << "[relay] unlock-all dispatched to " << who << " (game=" << game << ") in " << room
                      << std::endl;
            return;
        }
        if (cmd == "/teleport") {
            std::string room;
            std::string who;
            std::string dest;
            in >> room >> who >> dest;
            if (room.empty() || who.empty() || dest.empty()) {
                std::cout << "[relay] usage: /teleport <session-id> <username|player-id=N> "
                             "<username|player-id=N|entrance=0xNNNN>"
                          << std::endl;
                return;
            }
            injectRoomLine(room, std::string("{\"type\":\"OOTMM_TELEPORT\",\"seedId\":\"") + room + "\",\"who\":\"" +
                                     who + "\",\"dest\":\"" + dest + "\"}");
            return;
        }
        if (cmd == "/say") {
            std::string room;
            in >> room;
            std::string message;
            std::getline(in, message);
            while (!message.empty() && message.front() == ' ') {
                message.erase(message.begin());
            }
            if (room.empty() || message.empty()) {
                std::cout << "[relay] usage: /say <session-id> <message>" << std::endl;
                return;
            }
            std::string escaped;
            for (const char ch : message) {
                if (ch == '"' || ch == '\\') {
                    escaped += '\\';
                }
                escaped += ch;
            }
            injectRoomLine(room, std::string("{\"type\":\"OOTMM_CHAT\",\"seedId\":\"") + room +
                                     "\",\"from\":\"server\",\"text\":\"" + escaped + "\"}");
            return;
        }
        std::cout << "[relay] unknown command '" << cmd << "' — /help lists commands" << std::endl;
    };

    while (true) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listener, &readSet);
        SocketHandle maxFd = listener;
        for (const RelayClient& c : clients) {
            FD_SET(c.sock, &readSet);
            if (c.sock > maxFd) {
                maxFd = c.sock;
            }
        }
        timeval tv{ 1, 0 };
        const int ready = select(static_cast<int>(maxFd) + 1, &readSet, nullptr, nullptr, &tv);
        if (ready < 0) {
            std::cerr << "[relay] select failed" << std::endl;
            break;
        }

        // Console commands (stdin thread or host UI) run between selects on this thread.
        for (;;) {
            std::string command;
            {
                std::lock_guard<std::mutex> lock(gCommandMutex);
                if (gCommandQueue.empty()) {
                    break;
                }
                command = std::move(gCommandQueue.front());
                gCommandQueue.pop_front();
            }
            runCommand(command);
        }

        // Session-rules refresh: immediately on a live change, otherwise every few seconds.
        const auto now = std::chrono::steady_clock::now();
        const std::string currentMeta = sessionMetaLine();
        if (currentMeta != lastMeta || now >= nextMetaBroadcast) {
            if (currentMeta != lastMeta) {
                lastMeta = currentMeta;
                std::cout << "[relay] session rules changed: " << currentMeta;
            }
            nextMetaBroadcast = now + std::chrono::seconds(5);
            for (const RelayClient& c : clients) {
                if (!c.room.empty()) {
                    send(c.sock, currentMeta.data(), static_cast<int>(currentMeta.size()), 0);
                }
            }
        }

        if (FD_ISSET(listener, &readSet)) {
            const SocketHandle accepted = static_cast<SocketHandle>(accept(listener, nullptr, nullptr));
            if (accepted != kInvalidSocket) {
                clients.push_back(RelayClient{ accepted, {}, {}, {} });
                std::cout << "[relay] client connected (" << clients.size() << " total)" << std::endl;
            }
        }

        for (size_t i = 0; i < clients.size();) {
            RelayClient& c = clients[i];
            bool drop = false;
            if (FD_ISSET(c.sock, &readSet)) {
                char chunk[4096];
                const int received = recv(c.sock, chunk, sizeof(chunk), 0);
                if (received <= 0) {
                    drop = true;
                } else {
                    c.buffer.append(chunk, static_cast<size_t>(received));
                    size_t newline;
                    while ((newline = c.buffer.find('\n')) != std::string::npos) {
                        std::string line = c.buffer.substr(0, newline);
                        c.buffer.erase(0, newline + 1);
                        if (line.empty()) {
                            continue;
                        }
                        const std::string type = ExtractJsonStringField(line, "type");
                        if (type == ootmm::anchor::PacketHandshake) {
                            c.room = ExtractJsonStringField(line, "seedId");
                            c.name = ExtractJsonStringField(line, "clientName");
                            c.playerId = ExtractJsonNumberField(line, "playerId", 0);
                            std::cout << "[relay] '" << c.name << "' joined room " << c.room << " (player "
                                      << c.playerId << ")" << std::endl;
                            // Tell the joiner the server's session rules.
                            const std::string metaWire = sessionMetaLine();
                            send(c.sock, metaWire.data(), static_cast<int>(metaWire.size()), 0);
                            // Replay the room's durable history (completed checks + sent items):
                            // the server is the source of truth for check state, so a fresh
                            // native save gets everything back without recollecting.
                            RoomStore& store = rooms[c.room];
                            LoadRoomStore(store, storeDir, c.room);
                            if (!store.lines.empty()) {
                                std::string replay;
                                for (const std::string& stored : store.lines) {
                                    replay += stored;
                                    replay += "\n";
                                }
                                send(c.sock, replay.data(), static_cast<int>(replay.size()), 0);
                                std::cout << "[relay] replayed " << store.lines.size() << " stored packet(s) to '"
                                          << c.name << "'" << std::endl;
                            }
                            continue; // handshakes are not relayed
                        }
                        if (c.room.empty()) {
                            continue; // no handshake yet — ignore
                        }
                        // Check catalog upload: parse into the in-memory per-player catalog for
                        // /list-checks. Not relayed to other clients and not persisted (it is large
                        // and every player re-uploads on reconnect).
                        if (type == ootmm::anchor::PacketCheckCatalog) {
                            const int pid = ExtractJsonNumberField(line, "playerId", c.playerId);
                            const std::string checksCsv = ExtractJsonStringField(line, "checks");
                            PlayerCatalog cat;
                            cat.name = c.name;
                            size_t pos = 0;
                            while (pos < checksCsv.size()) {
                                size_t comma = checksCsv.find(',', pos);
                                if (comma == std::string::npos) {
                                    comma = checksCsv.size();
                                }
                                if (comma > pos) {
                                    cat.checkIds.push_back(checksCsv.substr(pos, comma - pos));
                                }
                                pos = comma + 1;
                            }
                            catalogs[c.room][pid] = std::move(cat);
                            std::cout << "[relay] catalog: player " << pid << " in " << c.room << " -> "
                                      << catalogs[c.room][pid].checkIds.size() << " checks" << std::endl;
                            continue; // not relayed, not persisted
                        }
                        // Durable multiworld state: record check completions and item sends so
                        // future joiners (and fresh saves) replay them.
                        if (IsPersistedPacketType(type)) {
                            RoomStore& store = rooms[c.room];
                            LoadRoomStore(store, storeDir, c.room);
                            AppendRoomStore(store, storeDir, c.room, line);
                        }
                        const std::string wire = line + "\n";
                        for (RelayClient& other : clients) {
                            if (other.sock != c.sock && other.room == c.room) {
                                send(other.sock, wire.data(), static_cast<int>(wire.size()), 0);
                            }
                        }
                    }
                }
            }
            if (drop) {
                std::cout << "[relay] client '" << c.name << "' disconnected" << std::endl;
                CloseSocket(c.sock);
                clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }
    CloseSocket(listener);
    return 0;
}

} // namespace ootmm::relay
