#pragma once

// OoTMM multiworld relay: rooms clients by the seedId in their handshake packet and broadcasts
// every other packet line to the room's other members. Transport = TCP, newline-delimited JSON —
// the same packet dialect the session file bridge uses. Shared by the launcher (--serve / the
// "host relay" checkbox) and the dedicated ootmm_server executable. Socket portability helpers
// live here too, shared with the launcher's MultiworldClient.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace ootmm::relay {

#ifdef _WIN32
// Matches winsock's SOCKET (UINT_PTR) without pulling windows headers into every includer.
using SocketHandle = uintptr_t;
inline constexpr SocketHandle kInvalidSocket = ~static_cast<SocketHandle>(0); // INVALID_SOCKET
#else
using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocket = -1;
#endif

// The launcher's default relay port (the dedicated server uses it too when no port is given).
inline constexpr uint16_t kDefaultRelayPort = 42801;

void CloseSocket(SocketHandle s);
bool SocketWouldBlock();
void SetNonBlocking(SocketHandle s);
// One-time platform socket init (WSAStartup on Windows; no-op elsewhere). Safe to call repeatedly.
bool InitSockets();

// Minimal string-field extraction for the relay's room keying. Packet fields the ports emit
// (seedId/settingsHash/clientName) are plain alphanumeric hashes, so no unescaping is needed.
std::string ExtractJsonStringField(const std::string& line, const std::string& key);

// SERVER-side session rules: every client that joins a room is told the state via an
// OOTMM_SESSION_META packet, and the launchers mirror it to their games. Players cannot
// override them. All fields are atomics so a host UI (or the console) can flip them live;
// the relay re-broadcasts the meta line a few times a minute so a one-shot line lost to a
// reconnect can't strand a client on stale rules.
struct RelayRules {
    std::atomic<bool> pvp{ false };
    std::atomic<bool> deathLink{ false };  // one player's death kills every player in the room
    std::atomic<bool> timerEnabled{ true }; // whether launchers may offer the game-speed slider
    std::atomic<int> timerMinPercent{ 0 };  // slider range the launchers must clamp to
    std::atomic<int> timerMaxPercent{ 250 };
};

// Submit a console command ("/help", "/list-checks <session>", ...) to the running relay; it
// executes on the relay thread between selects and prints its answer to stdout. Thread-safe.
void SubmitRelayCommand(const std::string& line);

// Runs the relay on the given port until the process is interrupted. Returns nonzero on a
// socket-setup failure (message already printed to stderr).
//
// `storeDir` (optional): when set, the relay PERSISTS every check/item packet per room to
// <storeDir>/room_<seedId>.jsonl and replays the stored history to every client that joins,
// so check state lives on the SERVER — a player who starts a fresh save gets their collected
// checks and every item other players sent them back without recollecting anything.
int RunRelayServer(uint16_t port, const RelayRules& rules, const std::filesystem::path& storeDir = {});
int RunRelayServer(uint16_t port, const std::atomic<bool>& pvp);
int RunRelayServer(uint16_t port, bool pvp = false);

} // namespace ootmm::relay
