// OoTMM PC coordinator / launcher.
//
// OoTMM is a single combined game where Ocarina of Time is the host runtime and
// Majora's Mask is entered through cross-game load zones (the Happy Mask Shop
// portal, cross-game warp songs, Song of Soaring, or a seed that starts in MM).
// The PC port realizes this with two separate executables (SoH for OoT, 2S2H for
// MM). This coordinator is the glue: it owns the seed, decides which game to
// boot, and performs the handoff whenever the active port reports a cross-game
// transition.
//
// On Windows the launcher is a GUI application with two stages sharing one window:
//   1. CONFIG SCREEN — seed picker, game executable paths, per-game mod toggles,
//      multiworld settings (shown only for multiworld seeds), validated before launch.
//   2. RUNTIME MANAGER — the window hosts the active game's render window embedded
//      as a child, with a sidebar panel: live check tracker, chronological check log,
//      session controls (restart / reset seed progress / return to launcher), and
//      appearance options. Debug/cheat menus in both ports are disabled unless
//      explicitly enabled from the sidebar's advanced options.
// The handoff is still save-and-relaunch underneath (one game at a time). On
// non-Windows the launcher stays a console coordinator with the original loop.
//
// Protocol (all paths live under a session directory):
//   * boot.json   - BootConfig the next port reads on startup (OOTMM_BOOT_CONFIG).
//   * state.json  - shared Runtime progress snapshot carried across the handoff.
//   * <game>.outbox.jsonl - packets emitted by a port (check/give-item/transition).
//   * <game>.inbox.jsonl  - packets the coordinator delivers to a port.

#include "ootmm/AnchorBridge.hpp"
#include "ootmm/Presence.hpp"
#include "ootmm/BootConfig.hpp"
#include "ootmm/FilePacketBridge.hpp"
#include "ootmm/Seed.hpp"

#include "RelayServer.hpp"

#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <future>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <dwmapi.h>

#include "ForeignModelMods.hpp"
#include "GameConfig.hpp"
#include "LauncherConfig.hpp"
#include "LauncherUi.hpp"
#include "LogicSolver.hpp"
#include "ModelRepack.hpp"
#include "SimpleJson.hpp"
#include "Tracker.hpp"

#include "imgui.h"
#else
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

struct Args {
    fs::path seedPath;
    fs::path ootExe;
    fs::path mmExe;
    fs::path sessionDir;
    std::optional<ootmm::Game> startOverride;
    // Multiworld: connect this player's session to a relay server ("host:port"), and/or run
    // the relay server itself (--serve <port>, no game processes).
    std::string multiworldServer;
    std::string multiworldName = "ootmm-pc";
    std::optional<uint16_t> servePort;
    bool servePvp = false; // --pvp with --serve: server-wide PvP rule
    // Diagnostic: run the reverse-logic solver on a seed with only its starting items
    // and print the reachable locations (JSON array) — diffed against the generator's
    // Pathfinder by tools' validation flow. --solve-items adds a JSON item-count map.
    fs::path solveSeed;
    fs::path solveItems;
    // In-game self-test: arms the scene-sweep check-claim audit in the booted game; the JSONL
    // report lands in the session dir as selftest-report.jsonl.
    bool selftest = false;
    // Build the cross-game model archives from the games' extracted base archives, then exit
    // (the same native builder the GUI Setup flow runs). Requires --oot and --mm.
    bool buildMods = false;

    [[nodiscard]] bool HasGameArgs() const {
        return !seedPath.empty() || !ootExe.empty() || !mmExe.empty() || !multiworldServer.empty();
    }
    [[nodiscard]] bool CompleteGameArgs() const {
        return !seedPath.empty() && !ootExe.empty() && !mmExe.empty();
    }
};

void PrintUsage() {
    std::cerr << "Usage: ootmm_launcher                      (opens the GUI launcher)\n"
                 "       ootmm_launcher --seed <seed.ootmm.json> --oot <soh.exe> --mm <2ship.exe>\n"
                 "                      [--session <dir>] [--start oot|mm]\n"
                 "                      [--multiworld <host:port>] [--name <player name>] [--selftest]\n"
                 "       ootmm_launcher --serve <port> [--pvp]   (run the multiworld relay server)\n"
                 "       ootmm_launcher --build-mods --oot <soh.exe> --mm <2ship.exe>\n"
                 "                      (build the cross-game model mod archives, then exit)\n";
}

std::optional<Args> ParseArgs(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        const auto next = [&](const char* name) -> std::optional<std::string> {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                return std::nullopt;
            }
            return std::string(argv[++i]);
        };

        if (flag == "--seed") {
            const auto value = next("--seed");
            if (!value) return std::nullopt;
            args.seedPath = *value;
        } else if (flag == "--oot") {
            const auto value = next("--oot");
            if (!value) return std::nullopt;
            args.ootExe = *value;
        } else if (flag == "--mm") {
            const auto value = next("--mm");
            if (!value) return std::nullopt;
            args.mmExe = *value;
        } else if (flag == "--session") {
            const auto value = next("--session");
            if (!value) return std::nullopt;
            args.sessionDir = *value;
        } else if (flag == "--start") {
            const auto value = next("--start");
            if (!value) return std::nullopt;
            const auto game = ootmm::GameFromString(*value);
            if (!game) {
                std::cerr << "Invalid --start value (expected oot or mm)\n";
                return std::nullopt;
            }
            args.startOverride = *game;
        } else if (flag == "--multiworld") {
            const auto value = next("--multiworld");
            if (!value) return std::nullopt;
            args.multiworldServer = *value;
        } else if (flag == "--name") {
            const auto value = next("--name");
            if (!value) return std::nullopt;
            args.multiworldName = *value;
        } else if (flag == "--serve") {
            const auto value = next("--serve");
            if (!value) return std::nullopt;
            args.servePort = static_cast<uint16_t>(std::stoul(*value));
        } else if (flag == "--pvp") {
            args.servePvp = true;
        } else if (flag == "--selftest") {
            args.selftest = true;
        } else if (flag == "--build-mods") {
            args.buildMods = true;
        } else if (flag == "--solve") {
            const auto value = next("--solve");
            if (!value) return std::nullopt;
            args.solveSeed = *value;
        } else if (flag == "--solve-items") {
            const auto value = next("--solve-items");
            if (!value) return std::nullopt;
            args.solveItems = *value;
        } else if (flag == "-h" || flag == "--help") {
            return std::nullopt;
        } else {
            std::cerr << "Unknown argument: " << flag << "\n";
            return std::nullopt;
        }
    }
    return args;
}

void WriteFile(const fs::path& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << contents;
}

void TruncateFile(const fs::path& path) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
}

const char* GameName(ootmm::Game game) {
    return game == ootmm::Game::Oot ? "oot" : "mm";
}

const char* GameTitle(ootmm::Game game) {
    return game == ootmm::Game::Oot ? "Ocarina of Time" : "Majora's Mask";
}

// Filesystem-safe per-seed tag (base, no player suffix): the sanitized settingsHash (fallback
// seedId), max 32 chars. Used as the per-seed SESSION-DIR key for solo seeds. The ports' save
// folders and the launcher's reset path use SaveTag() instead (which adds _p<playerId> for
// multiworld) — see below.
std::string SeedTag(const ootmm::Seed& seed) {
    const std::string& src = !seed.settingsHash.empty() ? seed.settingsHash : seed.seedId;
    std::string tag;
    for (const char c : src) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (ok) {
            tag.push_back(c);
        }
        if (tag.size() >= 32) {
            break;
        }
    }
    return tag;
}

// Save-folder tag: SeedTag + _p<playerId> for multiworld. Mirrors the ports' ActiveSeedTag
// (which appends the same suffix) so the launcher can locate the per-seed, per-player save
// folders ("Save/ootmm-<tag>" for SoH, "saves/ootmm-<tag>" for 2S2H) that the ports create.
// Solo seeds (playerCount <= 1) keep the un-suffixed tag, so existing solo saves are untouched.
std::string SaveTag(const ootmm::Seed& seed) {
    std::string tag = SeedTag(seed);
    if (seed.playerCount > 1) {
        tag += "_p" + std::to_string(seed.playerId);
    }
    return tag;
}

// Per-seed session key: the seed tag plus a format suffix that forces a clean reinit if the
// state.json/bridge layout changes. The same seed RESUMES instead of being wiped each launch.
constexpr int kSessionFormat = 1;
std::string SeedSessionKey(const ootmm::Seed& seed) {
    std::string tag = SeedTag(seed);
    if (tag.empty()) {
        tag = "default";
    }
    // Multiworld seeds share one seedId across every player; two launchers on the same
    // machine (seed files in the same folder) would otherwise share ONE session dir and
    // cross-contaminate each other's inbox/state/presence files — each launcher then relays
    // the OTHER port's pose stamped with its own player id (a ghost puppet riding the local
    // player) and item routing breaks.
    if (seed.playerCount > 1) {
        tag += "_p" + std::to_string(seed.playerId);
    }
    return tag + "_v" + std::to_string(kSessionFormat);
}

// ---------------------------------------------------------------------------
// Minimal cross-platform child-process control.
// ---------------------------------------------------------------------------

class ChildProcess {
  public:
#ifdef _WIN32
    // parentHwnd: when non-null, the game embeds its render window as a child of it
    // (passed via OOTMM_PARENT_HWND); null => the game opens its own top-level window.
    // setupOnly: spawn the game in OoTMM setup mode — it runs its own first-boot ROM
    // extraction (asking the user for the ROM) and exits without booting into gameplay;
    // setupNeedMq additionally requires the OoT Master Quest archive (seed uses MQ dungeons).
    ChildProcess(const fs::path& exe, const fs::path& bootConfigPath, HWND parentHwnd, bool setupOnly = false,
                 bool setupNeedMq = false) {
        // Env vars are inherited at CreateProcess time; stale values from a previous spawn
        // must be cleared explicitly (nullptr removes the variable).
        SetEnvironmentVariableW(L"OOTMM_BOOT_CONFIG",
                                bootConfigPath.empty() ? nullptr : bootConfigPath.wstring().c_str());
        SetEnvironmentVariableW(L"OOTMM_SETUP_ONLY", setupOnly ? L"1" : nullptr);
        SetEnvironmentVariableW(L"OOTMM_SETUP_NEED_MQ", setupOnly && setupNeedMq ? L"1" : nullptr);
        if (parentHwnd != nullptr) {
            const std::wstring handle = std::to_wstring(reinterpret_cast<uintptr_t>(parentHwnd));
            SetEnvironmentVariableW(L"OOTMM_PARENT_HWND", handle.c_str());
        } else {
            SetEnvironmentVariableW(L"OOTMM_PARENT_HWND", nullptr);
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};

        std::wstring commandLine = L"\"" + exe.wstring() + L"\"";
        std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
        mutableCommand.push_back(L'\0');

        const std::wstring workingDir = exe.parent_path().wstring();
        const BOOL ok = CreateProcessW(exe.wstring().c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, 0,
                                       nullptr, workingDir.empty() ? nullptr : workingDir.c_str(), &startup, &info);
        if (ok) {
            handle_ = info.hProcess;
            pid_ = info.dwProcessId;
            CloseHandle(info.hThread);
        }
    }
#else
    ChildProcess(const fs::path& exe, const fs::path& bootConfigPath, bool setupOnly = false,
                 bool setupNeedMq = false) {
        const pid_t pid = fork();
        if (pid == 0) {
            if (bootConfigPath.empty()) {
                unsetenv("OOTMM_BOOT_CONFIG");
            } else {
                setenv("OOTMM_BOOT_CONFIG", bootConfigPath.string().c_str(), 1);
            }
            if (setupOnly) {
                setenv("OOTMM_SETUP_ONLY", "1", 1);
            } else {
                unsetenv("OOTMM_SETUP_ONLY");
            }
            if (setupOnly && setupNeedMq) {
                setenv("OOTMM_SETUP_NEED_MQ", "1", 1);
            } else {
                unsetenv("OOTMM_SETUP_NEED_MQ");
            }
            const std::string exeStr = exe.string();
            char* const childArgv[] = { const_cast<char*>(exeStr.c_str()), nullptr };
            execv(exeStr.c_str(), childArgv);
            _exit(127);
        }
        if (pid > 0) {
            pid_ = pid;
        }
    }
#endif

    [[nodiscard]] bool Valid() const {
#ifdef _WIN32
        return handle_ != nullptr;
#else
        return pid_ > 0;
#endif
    }

#ifdef _WIN32
    [[nodiscard]] DWORD Pid() const { return pid_; }
#endif

    [[nodiscard]] bool Running() {
#ifdef _WIN32
        if (handle_ == nullptr) return false;
        return WaitForSingleObject(handle_, 0) == WAIT_TIMEOUT;
#else
        if (pid_ <= 0) return false;
        int status = 0;
        const pid_t result = waitpid(pid_, &status, WNOHANG);
        if (result == 0) return true;
        pid_ = -1;
        return false;
#endif
    }

    void WaitForExit() {
#ifdef _WIN32
        if (handle_ != nullptr) {
            WaitForSingleObject(handle_, INFINITE);
        }
#else
        if (pid_ > 0) {
            int status = 0;
            waitpid(pid_, &status, 0);
            pid_ = -1;
        }
#endif
    }

    void Terminate() {
#ifdef _WIN32
        if (handle_ != nullptr) {
            TerminateProcess(handle_, 0);
            // The game renders into a WS_CHILD window of the launcher, so its teardown
            // needs this thread's message pump. A plain WaitForSingleObject(INFINITE)
            // here deadlocks the UI ("Not Responding") — pump while waiting instead.
            const DWORD deadline = GetTickCount() + 5000;
            for (;;) {
                DWORD remaining = deadline - GetTickCount();
                if (static_cast<int>(remaining) <= 0) {
                    break;
                }
                DWORD r = MsgWaitForMultipleObjects(1, &handle_, FALSE, remaining, QS_ALLINPUT);
                if (r == WAIT_OBJECT_0 + 1) {
                    MSG msg;
                    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&msg);
                        DispatchMessageW(&msg);
                    }
                    continue;
                }
                break;
            }
        }
#else
        if (pid_ > 0) {
            kill(pid_, SIGTERM);
            int status = 0;
            waitpid(pid_, &status, 0);
            pid_ = -1;
        }
#endif
    }

    ~ChildProcess() {
#ifdef _WIN32
        if (handle_ != nullptr) {
            CloseHandle(handle_);
        }
#endif
    }

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

  private:
#ifdef _WIN32
    HANDLE handle_ = nullptr;
    DWORD pid_ = 0;
#else
    pid_t pid_ = -1;
#endif
};

// ===========================================================================
// Multiworld networking. The relay server (--serve) rooms clients by seedId and
// broadcasts every packet line to the other clients of the room; the client
// (--multiworld host:port) rides inside the coordinator loop: outbox packets
// addressed to a REMOTE player go over the wire instead of the local inbox, and
// wire packets addressed to the LOCAL player are appended to the active game's
// inbox (delivery then flows through the normal remote-give-item path, which
// dedups by deliveryKey). Transport = TCP, newline-delimited JSON — the same
// packet dialect the file bridge uses.
// ===========================================================================

// Socket shims, JSON field extraction and the relay implementation live in RelayServer.{hpp,cpp}
// (shared with the dedicated ootmm_server executable).
using ootmm::relay::CloseSocket;
using ootmm::relay::ExtractJsonStringField;
using ootmm::relay::InitSockets;
using ootmm::relay::kInvalidSocket;
using ootmm::relay::RunRelayServer;
using ootmm::relay::SetNonBlocking;
using ootmm::relay::SocketHandle;
using ootmm::relay::SocketWouldBlock;

// Pre-boot reachability check: TCP-connect to the relay with a short timeout. Multiworld
// sessions refuse to start when this fails — booting the games without a reachable relay
// strands the session (no items, no presence, no session rules) and has ended in players
// blaming the build for a dead server.
bool PingRelayServer(const std::string& host, uint16_t port, int timeoutMs, std::string& error) {
    if (!InitSockets()) {
        error = "socket init failed";
        return false;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0 || result == nullptr) {
        error = "cannot resolve " + host;
        return false;
    }
    bool connected = false;
    for (addrinfo* it = result; it != nullptr && !connected; it = it->ai_next) {
        const SocketHandle s = static_cast<SocketHandle>(socket(it->ai_family, it->ai_socktype, it->ai_protocol));
        if (s == kInvalidSocket) {
            continue;
        }
        SetNonBlocking(s);
        if (connect(s, it->ai_addr, static_cast<int>(it->ai_addrlen)) == 0) {
            connected = true;
        } else {
            fd_set writeSet;
            FD_ZERO(&writeSet);
            FD_SET(s, &writeSet);
            timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
            if (select(static_cast<int>(s) + 1, nullptr, &writeSet, nullptr, &tv) == 1) {
                int soError = 0;
#ifdef _WIN32
                int len = sizeof(soError);
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &len);
#else
                socklen_t len = sizeof(soError);
                getsockopt(s, SOL_SOCKET, SO_ERROR, &soError, &len);
#endif
                connected = soError == 0;
            }
        }
        CloseSocket(s);
    }
    freeaddrinfo(result);
    if (!connected) {
        error = "no relay is listening on " + host + ":" + std::to_string(port);
    }
    return connected;
}

class MultiworldClient {
  public:
    MultiworldClient(std::string host, uint16_t port, std::string handshakeLine)
        : host_(std::move(host)), port_(port), handshakeLine_(std::move(handshakeLine)) {}

    ~MultiworldClient() {
        if (socket_ != kInvalidSocket) {
            CloseSocket(socket_);
        }
    }

    void QueueLine(const std::string& line) {
        sendBuffer_ += line;
        sendBuffer_ += '\n';
    }

    // Drives the connection (connect / reconnect with backoff), flushes queued lines, and
    // appends every complete inbound line to `inbound`. Call once per coordinator tick.
    void Pump(std::vector<std::string>& inbound) {
        if (socket_ == kInvalidSocket) {
            if (std::chrono::steady_clock::now() < nextConnectAttempt_) {
                return;
            }
            Connect();
            if (socket_ == kInvalidSocket) {
                return;
            }
        }

        // Flush pending output.
        while (!sendBuffer_.empty()) {
            const int sent =
                send(socket_, sendBuffer_.data(), static_cast<int>(sendBuffer_.size()), 0);
            if (sent > 0) {
                sendBuffer_.erase(0, static_cast<size_t>(sent));
            } else if (SocketWouldBlock()) {
                break;
            } else {
                Disconnect("send failed");
                return;
            }
        }

        // Read whatever is available.
        char chunk[4096];
        while (true) {
            const int received = recv(socket_, chunk, sizeof(chunk), 0);
            if (received > 0) {
                recvBuffer_.append(chunk, static_cast<size_t>(received));
            } else if (received == 0) {
                Disconnect("server closed the connection");
                break;
            } else if (SocketWouldBlock()) {
                break;
            } else {
                Disconnect("recv failed");
                break;
            }
        }

        size_t newline;
        while ((newline = recvBuffer_.find('\n')) != std::string::npos) {
            std::string line = recvBuffer_.substr(0, newline);
            recvBuffer_.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (!line.empty()) {
                inbound.push_back(std::move(line));
            }
        }
    }

    [[nodiscard]] bool Connected() const {
        return socket_ != kInvalidSocket;
    }

  private:
    void Connect() {
        nextConnectAttempt_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        if (!InitSockets()) {
            return;
        }
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* result = nullptr;
        const std::string portText = std::to_string(port_);
        if (getaddrinfo(host_.c_str(), portText.c_str(), &hints, &result) != 0 || result == nullptr) {
            std::cerr << "[multiworld] cannot resolve " << host_ << ":" << port_ << std::endl;
            return;
        }
        SocketHandle s = kInvalidSocket;
        for (addrinfo* it = result; it != nullptr; it = it->ai_next) {
            s = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
            if (s == kInvalidSocket) {
                continue;
            }
            if (connect(s, it->ai_addr, static_cast<int>(it->ai_addrlen)) == 0) {
                break;
            }
            CloseSocket(s);
            s = kInvalidSocket;
        }
        freeaddrinfo(result);
        if (s == kInvalidSocket) {
            std::cerr << "[multiworld] connection to " << host_ << ":" << port_ << " failed; retrying" << std::endl;
            return;
        }
        SetNonBlocking(s);
        socket_ = s;
        // (Re)introduce ourselves first, then let any queued packets follow.
        sendBuffer_ = handshakeLine_ + "\n" + sendBuffer_;
        std::cout << "[multiworld] connected to " << host_ << ":" << port_ << std::endl;
    }

    void Disconnect(const char* reason) {
        std::cerr << "[multiworld] disconnected (" << reason << "); reconnecting" << std::endl;
        if (socket_ != kInvalidSocket) {
            CloseSocket(socket_);
            socket_ = kInvalidSocket;
        }
        recvBuffer_.clear();
        nextConnectAttempt_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    }

    std::string host_;
    uint16_t port_;
    std::string handshakeLine_;
    SocketHandle socket_ = kInvalidSocket;
    std::string sendBuffer_;
    std::string recvBuffer_;
    std::chrono::steady_clock::time_point nextConnectAttempt_{};
};

struct Session {
    fs::path seedPath;
    fs::path ootExe;
    fs::path mmExe;
    fs::path sessionDir;
    fs::path statePath;
    fs::path bootConfigPath;
    fs::path ootInbox;
    fs::path ootOutbox;
    fs::path mmInbox;
    fs::path mmOutbox;
    fs::path presenceOut; // the running port's own pose (atomic last-value)
    fs::path presenceIn;  // remote-player roster the coordinator mirrors for the port
    fs::path selftestReport; // non-empty arms the in-game check-claim audit (--selftest)
    ootmm::Seed seed;
    bool allowDebugMenus = false; // gate for the ports' debug/cheats/enhancements menus
    std::unique_ptr<MultiworldClient> multiworld; // null when playing single-world
};

ootmm::BootConfig MakeBootConfig(const Session& s, ootmm::Game game, std::optional<uint32_t> bootEntrance,
                                 std::optional<uint32_t> ootAge = std::nullopt) {
    const bool isOot = game == ootmm::Game::Oot;
    ootmm::BootConfig boot;
    boot.seedPath = s.seedPath.string();
    boot.statePath = s.statePath.string();
    boot.inboxPath = (isOot ? s.ootInbox : s.mmInbox).string();
    boot.outboxPath = (isOot ? s.ootOutbox : s.mmOutbox).string();
    // The MM port stores its per-seed saves under "<mmExe dir>/saves" (2S2H portable saves resolve
    // under the child's cwd == mmExe.parent_path(), matching its SaveManager savesFolderPath base).
    // Hand the OoT process that root so it can delete the paired MM save when its OoT save is erased.
    boot.mmSaveDir = (s.mmExe.parent_path() / "saves").string();
    if (s.multiworld) { // co-op presence only matters with a live session
        boot.presenceOutPath = s.presenceOut.string();
        boot.presenceInPath = s.presenceIn.string();
    }
    boot.allowDebugMenus = s.allowDebugMenus;
    boot.selftestReportPath = s.selftestReport.string();
    boot.bootGame = game;
    boot.bootEntrance = bootEntrance;
    boot.ootAge = ootAge;
    return boot;
}

// Builds/rebinds the per-seed session paths and resumes or initializes the session dir.
// Returns an error message on failure, empty optional on success.
std::optional<std::string> PrepareSession(Session& session, const fs::path& sessionDirOverride) {
    const fs::path sessionRoot =
        !sessionDirOverride.empty() ? fs::absolute(sessionDirOverride)
                                    : fs::absolute(session.seedPath.parent_path() / ".ootmm-session");
    const std::string seedKey = SeedSessionKey(session.seed);
    const fs::path sessionDir = sessionRoot / seedKey;

    std::error_code ec;
    fs::create_directories(sessionDir, ec);
    if (ec) {
        return "cannot create session dir " + sessionDir.string() + ": " + ec.message();
    }

    session.sessionDir = sessionDir;
    session.statePath = sessionDir / "state.json";
    session.bootConfigPath = sessionDir / "boot.json";
    session.ootInbox = sessionDir / "oot.inbox.jsonl";
    session.ootOutbox = sessionDir / "oot.outbox.jsonl";
    session.mmInbox = sessionDir / "mm.inbox.jsonl";
    session.mmOutbox = sessionDir / "mm.outbox.jsonl";
    session.presenceOut = sessionDir / "presence.out.json";
    session.presenceIn = sessionDir / "presence.in.json";

    // A marker records which seed owns this dir. Match => RESUME; missing/mismatch => new seed.
    const fs::path markerPath = sessionDir / "session.id";
    bool resume = false;
    {
        std::ifstream marker(markerPath, std::ios::binary);
        if (marker) {
            std::string existing((std::istreambuf_iterator<char>(marker)), std::istreambuf_iterator<char>());
            resume = (existing == seedKey);
        }
    }

    // Outboxes are pure port->launcher transport, re-truncated at each boot, so clearing them
    // here is always safe and drops any leftover CROSS_GAME_TRANSITION so it cannot re-fire.
    TruncateFile(session.ootOutbox);
    TruncateFile(session.mmOutbox);

    if (resume) {
        // Keep state.json (deliveredItems_/completedChecks_/itemQueue_) and the inboxes (unconsumed
        // remote items) so cross-game progress survives a full launcher restart. The port re-reads
        // the inbox from offset 0 each boot and dedups via deliveredItems_, so nothing is
        // double-granted and state.json stays in lockstep with the per-seed native saves.
        std::cout << "[launcher] resuming seed " << session.seed.seedId << " (" << sessionDir.string() << ")"
                  << std::endl;
    } else {
        TruncateFile(session.ootInbox);
        TruncateFile(session.mmInbox);
        fs::remove(session.statePath, ec);
        ec.clear();
        WriteFile(markerPath, seedKey);
    }
    return std::nullopt;
}

// Wipes ALL progress for the session's seed: the shared ledger + bridge files and both
// ports' per-seed native save folders. The session dir itself is reinitialized.
void ResetSeedProgress(Session& session) {
    std::error_code ec;
    fs::remove(session.statePath, ec);
    if (ec) {
        std::cout << "[launcher] reset: failed to remove " << session.statePath.string() << ": " << ec.message()
                  << std::endl;
    }
    TruncateFile(session.ootInbox);
    TruncateFile(session.ootOutbox);
    TruncateFile(session.mmInbox);
    TruncateFile(session.mmOutbox);
    const std::string tag = SaveTag(session.seed);
    if (!tag.empty()) {
        const fs::path saveDirs[] = {
            session.ootExe.parent_path() / "Save" / ("ootmm-" + tag),
            session.mmExe.parent_path() / "saves" / ("ootmm-" + tag),
        };
        for (const fs::path& dir : saveDirs) {
            ec.clear();
            fs::remove_all(dir, ec);
            if (ec) {
                std::cout << "[launcher] reset: failed to remove " << dir.string() << ": " << ec.message()
                          << std::endl;
            }
        }
    }
}

} // namespace

#ifdef _WIN32
// ===========================================================================
// GUI launcher (Win32 + DX11 + ImGui). One top-level window drives two stages:
// the CONFIG SCREEN (full-client ImGui) and the RUNTIME MANAGER (the active game
// embedded as a child window on the left, the manager sidebar rendered in the
// uncovered strip on the right). A PeekMessage loop renders every frame and steps
// the coordinator state machine (outbox drain / multiworld pump / transition
// relaunch) on a 100 ms cadence.
// ===========================================================================
namespace {

using ootmm::launcher::Accent;
using ootmm::launcher::LauncherConfig;
using ootmm::launcher::ModEntry;
using ootmm::launcher::Tracker;
using ootmm::launcher::UiHost;

constexpr wchar_t kHostClassName[] = L"OotmmLauncherHost";
constexpr int kCollapsedSidebarWidth = 30;

enum class Stage {
    Config,
    Runtime,
};

struct RuntimeState {
    ootmm::Game activeGame = ootmm::Game::Oot;
    std::optional<uint32_t> bootEntrance;
    std::optional<uint32_t> bootOotAge;

    std::unique_ptr<ChildProcess> process;
    uintmax_t outboxOffset = 0;
    fs::path activeOutbox;
    fs::path activeInbox;
    fs::path otherInbox;

    std::optional<ootmm::CrossGameTransition> pendingTransition;
    std::chrono::steady_clock::time_point graceDeadline;
    std::chrono::steady_clock::time_point startedAt;
    std::chrono::steady_clock::time_point lastStateSync;

    HWND gameChild = nullptr; // discovered render window of the active game
    DWORD activePid = 0;
};

// Edit-buffer state of the config screen. Text lives in fixed buffers for ImGui.
struct ConfigForm {
    char seedPath[1024] = {};
    char ootExe[1024] = {};
    char mmExe[1024] = {};
    char mwAddress[256] = {};
    char mwName[128] = {};
    // Co-op player model archive sets (';' separated file names inside each game's mods dir;
    // empty = vanilla Link). Edited via the model picker lists in the Multiworld section.
    char playerModelOot[1024] = {};
    char playerModelMm[1024] = {};
    bool hostRelay = false;
    int relayPort = 42801;
    bool hostPvp = false; // server rule when hosting: PvP for every session on this relay

    std::optional<ootmm::Seed> seed; // preview of the seed at seedPath
    std::string seedError;
    std::string loadedSeedPath; // seedPath the preview was loaded from

    std::vector<ModEntry> ootMods;
    std::vector<ModEntry> mmMods;
    std::string ootModsDir; // exe dir the oot mod list was scanned from
    std::string mmModsDir;

    std::vector<std::string> validationErrors;
    std::string statusMessage; // e.g. "session ended" after returning from runtime
};

struct GuiApp {
    Stage stage = Stage::Config;
    HWND host = nullptr;
    UiHost ui;

    fs::path configPath;
    LauncherConfig cfg;
    ConfigForm form;

    Session session;
    RuntimeState run;
    Tracker tracker;
    ootmm::launcher::LogicSolver solver;
    uint64_t solvedRevision = 0; // tracker revision the last solve ran against
    int inLogicCount = 0;        // open checks currently reachable in logic

    // Per-game configuration editor (edits the port's own config JSON on disk).
    struct ConfigEditor {
        bool open = false;
        bool openedThisFrame = false;
        bool isOot = true;
        bool dirty = false;
        std::string error;
        ootmm::launcher::GameConfig config;
    } cfgEditor;

    fs::path sessionDirOverride; // from --session
    bool selftestArmed = false;  // from --selftest: arm the in-game check-claim audit
    std::optional<ootmm::Game> startOverride;

    // First-time setup flow: games queued for a setup-only run (the port's own first-boot
    // ROM extraction, then exit), followed by a native cross-game model-mods build. One child
    // at a time; the queue is fixed at click time so a declined extraction cannot re-launch
    // the same game in a loop.
    struct SetupFlow {
        std::vector<ootmm::Game> queue;
        std::unique_ptr<ChildProcess> child;
        std::future<ootmm::launcher::ForeignModsResult> modsBuild; // valid() while building
        bool modsPending = false; // build the model archives once the game archives are ready
        [[nodiscard]] bool Active() const {
            return child != nullptr || !queue.empty() || modsBuild.valid() || modsPending;
        }
    } setup;

    bool relayStarted = false;
    bool autoLaunch = false; // full CLI args given: launch immediately
    bool cheatGateDirty = false; // allowDebugMenus changed; applies on next game boot
    int sidebarTab = 0;          // 0 tracker, 1 log, 2 session, 3 options
    bool quit = false;
};

Accent CurrentAccent(const GuiApp& app); // defined with the window loop below
void OpenGameConfigEditor(GuiApp& app, bool isOot);
void DrawGameConfigModal(GuiApp& app);

// ---------------------------------------------------------------------------
// Embedded-game window management
// ---------------------------------------------------------------------------

struct FindChildCtx {
    DWORD pid = 0;
    HWND found = nullptr;
};

BOOL CALLBACK FindGameChildProc(HWND child, LPARAM lparam) {
    auto* ctx = reinterpret_cast<FindChildCtx*>(lparam);
    DWORD pid = 0;
    GetWindowThreadProcessId(child, &pid);
    if (pid == ctx->pid) {
        ctx->found = child;
        return FALSE; // stop
    }
    return TRUE;
}

// Find the active game's render window (a child of the host owned by the game process).
HWND FindGameChild(HWND host, DWORD pid) {
    if (host == nullptr || pid == 0) {
        return nullptr;
    }
    FindChildCtx ctx;
    ctx.pid = pid;
    EnumChildWindows(host, FindGameChildProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

int SidebarWidth(const GuiApp& app) {
    int width = app.cfg.showSidebar ? app.cfg.sidebarWidth : kCollapsedSidebarWidth;
    // The game child window draws OVER everything the launcher renders, so a modal
    // centered on the viewport would be hidden beneath it. While the game-config
    // editor is open, widen the sidebar strip and host the modal inside it.
    if (app.stage == Stage::Runtime && app.cfgEditor.open && width < 640) {
        width = 640;
    }
    return width;
}

// Gives keyboard focus to the embedded game window. The game lives in ANOTHER process,
// so a plain SetFocus fails — the input queues must be attached first. Without this the
// game never receives keyboard input (including Esc for its own settings menu) after the
// user interacts with the sidebar.
void FocusGameChild(GuiApp& app) {
    HWND child = app.run.gameChild;
    if (child == nullptr || !IsWindow(child)) {
        return;
    }
    const DWORD childThread = GetWindowThreadProcessId(child, nullptr);
    const DWORD selfThread = GetCurrentThreadId();
    if (childThread != selfThread) {
        AttachThreadInput(selfThread, childThread, TRUE);
    }
    SetFocus(child);
    if (childThread != selfThread) {
        AttachThreadInput(selfThread, childThread, FALSE);
    }
}

// The rectangle of the host client area covered by the embedded game (left of the sidebar).
bool CursorOverGameArea(const GuiApp& app) {
    if (app.host == nullptr) {
        return false;
    }
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(app.host, &pt);
    RECT rc{};
    GetClientRect(app.host, &rc);
    const int gameWidth = (rc.right - rc.left) - SidebarWidth(app);
    return pt.x >= 0 && pt.x < gameWidth && pt.y >= 0 && pt.y < rc.bottom;
}

// The sidebar strip on the right of the host client area.
bool CursorOverSidebar(const GuiApp& app) {
    if (app.host == nullptr) {
        return false;
    }
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(app.host, &pt);
    RECT rc{};
    GetClientRect(app.host, &rc);
    const int gameWidth = (rc.right - rc.left) - SidebarWidth(app);
    return pt.x >= gameWidth && pt.x < rc.right && pt.y >= 0 && pt.y < rc.bottom;
}

// Opens the active game's own in-game menu (graphics / audio / input / etc.) by
// focusing the game and delivering the Esc toggle its window procedure listens for.
void OpenInGameMenu(GuiApp& app) {
    if (app.run.gameChild == nullptr || !IsWindow(app.run.gameChild)) {
        return;
    }
    FocusGameChild(app);
    PostMessageW(app.run.gameChild, WM_KEYDOWN, VK_ESCAPE, 0x00010001);
    PostMessageW(app.run.gameChild, WM_KEYUP, VK_ESCAPE, 0xC0010001);
}

// Size the embedded game to fill the host client area left of the sidebar strip.
void LayoutGameChild(GuiApp& app) {
    if (app.host == nullptr || app.run.gameChild == nullptr) {
        return;
    }
    RECT rc{};
    GetClientRect(app.host, &rc);
    const int width = (rc.right - rc.left) - SidebarWidth(app);
    MoveWindow(app.run.gameChild, 0, 0, width > 0 ? width : 0, rc.bottom - rc.top, TRUE);
}

// ---------------------------------------------------------------------------
// Coordinator state machine (runtime stage)
// ---------------------------------------------------------------------------

void StartActiveGame(GuiApp& app) {
    Session& s = app.session;
    RuntimeState& run = app.run;
    const bool isOot = run.activeGame == ootmm::Game::Oot;
    const ootmm::BootConfig boot = MakeBootConfig(s, run.activeGame, run.bootEntrance, run.bootOotAge);
    run.activeOutbox = isOot ? s.ootOutbox : s.mmOutbox;
    run.activeInbox = isOot ? s.ootInbox : s.mmInbox;
    run.otherInbox = isOot ? s.mmInbox : s.ootInbox;

    WriteFile(s.bootConfigPath, ootmm::SerializeBootConfig(boot));
    // Clear THIS game's outbox before booting it: it is reused across relaunches and
    // still holds the prior run's CROSS_GAME_TRANSITION packet, which (read from offset 0)
    // would re-fire a transition with no player input and ping-pong the two ports forever.
    TruncateFile(run.activeOutbox);

    std::cout << "[launcher] booting " << GameName(run.activeGame) << " ("
              << (isOot ? s.ootExe : s.mmExe).filename().string() << ")";
    if (run.bootEntrance.has_value()) {
        std::cout << " at entrance 0x" << std::hex << *run.bootEntrance << std::dec;
    }
    std::cout << " embedded in host" << std::endl;

    run.process = std::make_unique<ChildProcess>(isOot ? s.ootExe : s.mmExe, s.bootConfigPath, app.host);
    run.outboxOffset = 0;
    run.pendingTransition.reset();
    run.gameChild = nullptr;
    run.activePid = (run.process && run.process->Valid()) ? run.process->Pid() : 0;
    run.startedAt = std::chrono::steady_clock::now();
    app.cheatGateDirty = false;

    if (!run.process || !run.process->Valid()) {
        std::cerr << "[launcher] failed to launch " << GameName(run.activeGame) << std::endl;
        run.process.reset();
    }
}

// Re-runs the reverse-logic solver against the player's current items and updates
// every tracker check's inLogic flag. Cheap enough to run synchronously; only called
// when the tracker revision changed (a check was collected or an item arrived).
void RecomputeLogic(GuiApp& app) {
    app.solvedRevision = app.tracker.Revision();
    if (!app.solver.Loaded()) {
        return;
    }
    ootmm::launcher::LogicSolver::Input input;
    input.items = app.tracker.ItemCounts(app.session.seed);
    // The generator seeds license counts (but not renewables) from starting items.
    for (const ootmm::StartingItem& starting : app.session.seed.startingItems) {
        input.licenses[starting.item.id] += starting.count;
    }
    // Renewable/license credit accrues from the player's actually-collected checks.
    for (const ootmm::launcher::TrackerCheck& check : app.tracker.Checks()) {
        if (!check.collected || check.ownerPlayer != app.session.seed.playerId) {
            continue;
        }
        if (app.solver.IsRenewableLocation(check.name)) {
            ++input.renewables[check.itemId];
        }
        if (app.solver.IsLicenseLocation(check.name)) {
            ++input.licenses[check.itemId];
        }
    }
    std::unordered_set<std::string> solvedEvents;
    const std::unordered_set<std::string> reachable = app.solver.Solve(input, &solvedEvents);
    int inLogic = 0;
    for (ootmm::launcher::TrackerCheck& check : app.tracker.Checks()) {
        check.inLogic = reachable.contains(check.name);
        if (check.inLogic && !check.collected) {
            ++inLogic;
        }
    }
    app.inLogicCount = inLogic;

    // Solver diagnostics: every recompute snapshots what the solver was TOLD (items,
    // renewable/license credit) and what it CONCLUDED (events + in-logic uncollected checks)
    // to logic_debug.txt in the session folder. When an availability verdict looks wrong in
    // the tracker, this file shows whether the culprit is the input or the graph — e.g. og's
    // own logic grants the OOT_STICKS event (and thus grass-cutting) to any child who can
    // reach Goron City or Zora's Domain, which reads as a false positive but is og-faithful.
    if (!app.session.statePath.empty()) {
        std::ofstream dump(app.session.statePath.parent_path() / "logic_debug.txt",
                           std::ios::binary | std::ios::trunc);
        if (dump) {
            dump << "# solver input items\n";
            for (const auto& [id, count] : input.items) {
                dump << id << " x" << count << "\n";
            }
            dump << "# renewable credit\n";
            for (const auto& [id, count] : input.renewables) {
                dump << id << " x" << count << "\n";
            }
            dump << "# license credit\n";
            for (const auto& [id, count] : input.licenses) {
                dump << id << " x" << count << "\n";
            }
            dump << "# events concluded reachable\n";
            for (const std::string& ev : solvedEvents) {
                dump << ev << "\n";
            }
            dump << "# in-logic, uncollected\n";
            for (const ootmm::launcher::TrackerCheck& check : app.tracker.Checks()) {
                if (check.inLogic && !check.collected) {
                    dump << check.name << "\n";
                }
            }
        }
    }
}

// Tears down the running game (if any) and returns to the config screen.
void ResetPresence();                       // defined with the presence block below
void CleanupCoopModels(const Session& s);   // ditto

void EndSession(GuiApp& app, const std::string& message) {
    if (app.run.process && app.run.process->Running()) {
        app.run.process->Terminate();
    }
    app.run.process.reset();
    app.run.gameChild = nullptr;
    app.run.activePid = 0;
    app.run.pendingTransition.reset();
    app.session.multiworld.reset();
    if (!app.session.ootExe.empty() && !app.session.mmExe.empty()) {
        CleanupCoopModels(app.session);
    }
    ResetPresence();
    app.stage = Stage::Config;
    app.form.statusMessage = message;
}

void DrainActiveOutbox(GuiApp& app) {
    Session& s = app.session;
    RuntimeState& run = app.run;
    ootmm::FilePacketReadResult inbound = ootmm::ReadPacketLines(run.activeOutbox, run.outboxOffset);
    run.outboxOffset = inbound.nextOffset;
    for (const std::string& packet : inbound.packets) {
        if (auto transition = ootmm::anchor::ParseCrossGameTransitionPacket(packet); transition.has_value()) {
            if (!run.pendingTransition.has_value()) {
                run.pendingTransition = transition;
                run.graceDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
                std::cout << "[launcher] cross-game transition -> " << GameName(transition->toGame) << " entrance "
                          << transition->toEntrance << std::endl;
            }
        } else if (ootmm::presence::ParsePvpHitPacket(packet).has_value() ||
                   ootmm::presence::ParseDeathLinkPacket(packet).has_value()) {
            // PvP hits and death-link announcements are relay-only, never the local other-game
            // inbox (inboxes are durable and replayed from offset 0 — neither must repeat).
            if (s.multiworld) {
                s.multiworld->QueueLine(packet);
            }
        } else {
            // Feed the runtime manager's tracker/check log from the live event stream.
            if (const auto check = ootmm::anchor::ParseCheckCompletePacket(packet); check.has_value()) {
                app.tracker.MarkCollected(check->checkGame, check->checkId, check->item.name, check->targetPlayer,
                                          s.seed.playerId, /*logIt=*/true);
            }
            bool remoteOnly = false;
            if (s.multiworld) {
                // Relay every packet to the room; an item OWNED by a remote player must NOT also
                // be delivered locally (its world is elsewhere) — route it over the wire only.
                s.multiworld->QueueLine(packet);
                if (const auto event = ootmm::anchor::ParseGiveItemPacket(packet);
                    event.has_value() && event->targetPlayer != s.seed.playerId) {
                    remoteOnly = true;
                }
            }
            if (!remoteOnly) {
                ootmm::AppendPacketLines(run.otherInbox, { packet });
            }
        }
    }
}


// --- Co-op presence + player-model mod sync ---------------------------------------------------
// The running port writes its pose to session.presenceOut every game frame; we relay fresh
// samples to the room and mirror inbound remote poses into session.presenceIn. Player-model
// archives are announced by hash on join and streamed as base64 chunks into the launcher's
// modelcache; puppets render as vanilla Link until per-player model application lands.
struct PresenceCoord {
    uint32_t lastSentSeq = 0;
    struct Remote {
        ootmm::presence::PlayerPose pose;
        std::chrono::steady_clock::time_point seen;
    };
    std::map<uint16_t, Remote> roster;
    bool rosterDirty = false;
    struct LocalModel {
        ootmm::Game game;
        std::string name;
        std::string hash;
        std::vector<uint8_t> bytes;
    };
    std::vector<LocalModel> localModels;
    struct Incoming {
        ootmm::presence::ModAnnounce meta;
        std::vector<std::string> chunks;
        uint32_t received = 0;
        std::chrono::steady_clock::time_point lastActivity; // stalled transfers are re-requested
    };
    std::map<std::string, Incoming> incoming;
    struct SendJob {
        std::string hash;
        size_t modelIndex = 0;
        uint32_t nextSeq = 0;
        uint32_t total = 0;
    };
    std::vector<SendJob> sending;
    std::map<uint16_t, std::string> remoteModelNote; // playerId -> status line for the UI
    // Remote model SETS grouped per (playerId, game): the announces seen so far. Once every
    // archive of a group is cached, the whole set is repacked into that player's namespace and
    // installed into the game's mods folder. Re-armed whenever a new announce joins the group.
    struct RemoteGroup {
        std::vector<ootmm::presence::ModAnnounce> announces;
        bool installed = false;
    };
    std::map<uint32_t, RemoteGroup> remoteGroups; // key: playerId << 1 | (game == Mm)
    std::set<uint16_t> knownPeers;                // for re-announcing our models to late joiners
    std::set<std::string> verifiedCache;          // cache files whose CONTENT re-hashed clean this run
    std::chrono::steady_clock::time_point lastAnnounce{}; // periodic re-announce (restart recovery)
    // Session PvP state (last writer wins across launchers) + inbound hits addressed to our
    // player, mirrored into the roster file for ~2s so the game reliably sees each one.
    bool pvpActive = false;
    struct TimedHit {
        ootmm::presence::PvpHit hit;
        std::chrono::steady_clock::time_point received;
    };
    std::vector<TimedHit> pendingHits;
    // Death link (server rule) + inbound deaths, mirrored into the roster like hits.
    bool deathLinkActive = false;
    struct TimedDeath {
        ootmm::presence::DeathEvent death;
        std::chrono::steady_clock::time_point received;
    };
    std::vector<TimedDeath> pendingDeaths;
    // Server timer rule + the local game-speed slider value (100 = vanilla). The games read
    // speedPercent live from the roster meta every frame.
    bool timerEnabled = true;
    int timerMin = 0;
    int timerMax = 250;
    int speedPercent = 100;
    // Identity guard: a player is (seedId room) + playerId + client name. A pose claiming OUR
    // playerId from another client, two clients sharing one playerId, or our own game running a
    // different player's seed are all misconfigurations that used to corrupt presence silently.
    std::string identityWarning;
};
PresenceCoord gPresence;
constexpr size_t kModChunkRaw = 48 * 1024;

void ResetPresence() {
    gPresence = PresenceCoord{};
}

std::string SanitizeFileName(const std::string& name) {
    std::string out;
    for (const char c : name) {
        out += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_') ? c : '_';
    }
    return out.empty() ? std::string("model.o2r") : out;
}

fs::path ModelCacheDir(const GuiApp& app) {
    const fs::path dir = app.configPath.parent_path() / "modelcache";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

fs::path CachedModelPath(const GuiApp& app, const ootmm::presence::ModAnnounce& announce) {
    return ModelCacheDir(app) / (announce.hash + "_" + SanitizeFileName(announce.name));
}

// A cached archive is only trusted after its CONTENT re-hashes to the announced hash: a crash
// mid-write leaves a plausibly-named partial file that a bare exists() check would install
// forever. Bad files are deleted so the next announce re-downloads them. Verified hashes are
// memoized for the run - the periodic re-announce would otherwise re-hash every 20 seconds.
bool VerifyCachedModel(const GuiApp& app, const ootmm::presence::ModAnnounce& announce) {
    if (gPresence.verifiedCache.contains(announce.hash)) {
        return true;
    }
    const fs::path cached = CachedModelPath(app, announce);
    std::error_code ec;
    if (!fs::exists(cached, ec)) {
        return false;
    }
    bool ok = announce.size == 0 || fs::file_size(cached, ec) == announce.size;
    if (ok) {
        std::ifstream in(cached, std::ios::binary);
        const std::vector<uint8_t> bytes{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
        ok = in.eof() && !in.bad() && ootmm::presence::HashBytesHex(bytes.data(), bytes.size()) == announce.hash;
    }
    if (!ok) {
        std::cerr << "[presence] cached model " << cached.filename().string()
                  << " failed content verification; deleting for re-download" << std::endl;
        fs::remove(cached, ec);
        return false;
    }
    gPresence.verifiedCache.insert(announce.hash);
    return true;
}

// Installed per-player model archives are named so the ports can (a) auto-mount them at boot
// with the regular mods scan and (b) hot-mount them mid-session by watching for the prefix.
constexpr const char* kCoopModPrefix = "zz_coop_";

fs::path CoopModsDir(const Session& s, ootmm::Game game) {
    return (game == ootmm::Game::Oot ? s.ootExe : s.mmExe).parent_path() / "mods";
}

// The game's own base asset archives next to its exe: the source of the vanilla Link entries
// used to fill each remote player's model namespace.
std::vector<fs::path> BaseGameArchives(const Session& s, ootmm::Game game) {
    std::vector<fs::path> archives;
    const fs::path dir = (game == ootmm::Game::Oot ? s.ootExe : s.mmExe).parent_path();
    const auto names = game == ootmm::Game::Oot ? std::array<const char*, 2>{ "oot.o2r", "soh.o2r" }
                                                : std::array<const char*, 2>{ "mm.o2r", "2ship.o2r" };
    for (const char* name : names) {
        std::error_code ec;
        if (fs::exists(dir / name, ec)) {
            archives.push_back(dir / name);
        }
    }
    return archives;
}

// Every remote player gets a vanilla-fill archive so their puppet ALWAYS renders from its own
// pNNobjs/ namespace: resolving any part of a puppet at the canonical paths would go through
// the ports' alternate-asset substitution and dress it in the LOCAL player's skin. Named to
// sort (and therefore mount) before the "_m_" model archives, which override it entry-by-entry.
void GenerateVanillaFills(GuiApp& app) {
    Session& s = app.session;
    for (const ootmm::Game game : { ootmm::Game::Oot, ootmm::Game::Mm }) {
        const std::vector<fs::path> bases = BaseGameArchives(s, game);
        for (uint16_t pid = 1; pid <= s.seed.playerCount; ++pid) {
            if (pid == s.seed.playerId) {
                continue;
            }
            const fs::path out = CoopModsDir(s, game) /
                                 (kCoopModPrefix + ootmm::launcher::CoopModelPrefix(pid) + "_base.o2r");
            const ootmm::launcher::RepackResult r = ootmm::launcher::RepackVanillaFill(bases, out, pid);
            if (r.ok) {
                std::cout << "[presence] vanilla fill for player " << pid << " ("
                          << (game == ootmm::Game::Oot ? "oot" : "mm") << "): " << r.kept << " entries, "
                          << r.hashRefs << " hash refs" << std::endl;
            } else {
                std::cerr << "[presence] vanilla fill for player " << pid << " failed: " << r.error << std::endl;
            }
        }
    }
}

// Remove installed co-op model archives (previous sessions' leftovers at start; ours at end).
void CleanupCoopModels(const Session& s) {
    for (const ootmm::Game game : { ootmm::Game::Oot, ootmm::Game::Mm }) {
        std::error_code ec;
        for (const auto& p : fs::directory_iterator(CoopModsDir(s, game), ec)) {
            if (p.path().filename().string().rfind(kCoopModPrefix, 0) == 0) {
                fs::remove(p.path(), ec);
            }
        }
    }
}

// Repack + install every remote model group whose archives are all cached. Runs after each
// announce/download; groups install once and re-arm when a new announce extends the set.
void TryInstallRemoteModels(GuiApp& app) {
    Session& s = app.session;
    for (auto& [key, group] : gPresence.remoteGroups) {
        if (group.installed || group.announces.empty()) {
            continue;
        }
        bool allCached = true;
        for (const ootmm::presence::ModAnnounce& announce : group.announces) {
            allCached &= VerifyCachedModel(app, announce);
        }
        if (!allCached) {
            continue;
        }
        const uint16_t playerId = static_cast<uint16_t>(key >> 1);
        const ootmm::Game game = (key & 1) ? ootmm::Game::Mm : ootmm::Game::Oot;
        std::vector<fs::path> inputs, outputs;
        for (const ootmm::presence::ModAnnounce& announce : group.announces) {
            inputs.push_back(CachedModelPath(app, announce));
            // "_m_" sorts after the player's "_base" vanilla fill, so the model archive mounts
            // later and overrides the fill for every entry the set customizes.
            outputs.push_back(CoopModsDir(s, game) / (kCoopModPrefix + ootmm::launcher::CoopModelPrefix(playerId) +
                                                      "_m_" + announce.hash.substr(0, 8) + ".o2r"));
        }
        const std::vector<ootmm::launcher::RepackResult> results =
            ootmm::launcher::RepackModelSet(inputs, outputs, playerId, BaseGameArchives(s, game));
        int installed = 0;
        std::string problems;
        for (size_t i = 0; i < results.size(); ++i) {
            const ootmm::launcher::RepackResult& r = results[i];
            if (r.ok) {
                ++installed;
                std::cout << "[presence] installed model " << group.announces[i].name << " for player " << playerId
                          << " (" << r.kept << " entries, " << r.stringRefs << " path refs, " << r.hashRefs
                          << " hash refs, " << r.dropped << " dropped)" << std::endl;
            } else {
                problems += (problems.empty() ? "" : "; ") + group.announces[i].name + ": " + r.error;
                std::cerr << "[presence] model repack failed for " << group.announces[i].name << ": " << r.error
                          << std::endl;
            }
        }
        group.installed = true; // don't loop on failures; a new announce re-arms the group
        std::string& note = gPresence.remoteModelNote[playerId];
        note = installed > 0 ? std::to_string(installed) + " model archive(s) installed" : "";
        if (!problems.empty()) {
            note += (note.empty() ? "" : "; ") + problems;
        }
    }
}

// Hash + announce this player's configured model archives on session start.
void SetupPresenceModels(GuiApp& app) {
    ResetPresence();
    Session& s = app.session;
    if (!s.multiworld) {
        return;
    }
    CleanupCoopModels(s); // leftovers from a previous (possibly crashed) session
    GenerateVanillaFills(app);
    const auto addOneModel = [&](ootmm::Game game, const std::string& name, const fs::path& modsDir) {
        if (name.empty()) {
            return;
        }
        std::ifstream in(modsDir / name, std::ios::binary);
        if (!in) {
            std::cerr << "[presence] player model not found: " << (modsDir / name).string() << std::endl;
            return;
        }
        PresenceCoord::LocalModel model;
        model.game = game;
        model.name = name;
        model.bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        model.hash = ootmm::presence::HashBytesHex(model.bytes.data(), model.bytes.size());
        ootmm::presence::ModAnnounce announce;
        announce.playerId = s.seed.playerId;
        announce.game = game;
        announce.name = model.name;
        announce.hash = model.hash;
        announce.size = model.bytes.size();
        s.multiworld->QueueLine(ootmm::presence::BuildModAnnouncePacket(s.seed, announce));
        std::cout << "[presence] announcing player model " << model.name << " (" << model.hash << ", "
                  << model.bytes.size() << " bytes)" << std::endl;
        gPresence.localModels.push_back(std::move(model));
    };
    // A "player model" is often a SET of archives (child model + adult model + textures +
    // voice pack); the config value is a ';' or ',' separated list and every listed archive
    // is announced + synced to the session.
    const auto addModelList = [&](ootmm::Game game, const std::string& list, const fs::path& modsDir) {
        std::string name;
        for (size_t i = 0; i <= list.size(); ++i) {
            const char c = i < list.size() ? list[i] : ';';
            if (c == ';' || c == ',') {
                // trim surrounding whitespace
                size_t b = 0, e = name.size();
                while (b < e && std::isspace(static_cast<unsigned char>(name[b]))) ++b;
                while (e > b && std::isspace(static_cast<unsigned char>(name[e - 1]))) --e;
                addOneModel(game, name.substr(b, e - b), modsDir);
                name.clear();
            } else {
                name += c;
            }
        }
    };
    addModelList(ootmm::Game::Oot, app.cfg.playerModelOot, s.ootExe.parent_path() / "mods");
    addModelList(ootmm::Game::Mm, app.cfg.playerModelMm, s.mmExe.parent_path() / "mods");
}

// Consume a presence/mod-sync relay line. Returns true when the line was one of ours.
bool HandlePresenceLine(GuiApp& app, const std::string& line) {
    Session& s = app.session;
    if (auto pose = ootmm::presence::ParsePlayerStatePacket(line); pose.has_value()) {
        if (pose->playerId == s.seed.playerId) {
            // Someone else is broadcasting OUR player id: both launchers loaded the same
            // player's seed file. Surface it — silently dropping hid the misconfiguration.
            if (!pose->clientName.empty() && pose->clientName != app.cfg.playerName) {
                gPresence.identityWarning = "Another client ('" + pose->clientName + "') is also playing as Player " +
                                            std::to_string(pose->playerId) +
                                            " - every launcher must load its OWN player's seed file.";
            }
            return true;
        }
        // Two different clients claiming the same (non-local) player id is the same
        // misconfiguration seen from the outside; keep the first, flag the conflict.
        if (const auto existing = gPresence.roster.find(pose->playerId);
            existing != gPresence.roster.end() && !existing->second.pose.clientName.empty() &&
            !pose->clientName.empty() && existing->second.pose.clientName != pose->clientName) {
            gPresence.identityWarning = "Two clients ('" + existing->second.pose.clientName + "', '" +
                                        pose->clientName + "') both claim Player " + std::to_string(pose->playerId) +
                                        " - check the seed files.";
        }
        {
            // A peer we haven't seen yet (late joiner / relay reconnect) missed our session-start
            // model announces; repeat them so they can request the archives.
            if (gPresence.knownPeers.insert(pose->playerId).second) {
                for (const PresenceCoord::LocalModel& model : gPresence.localModels) {
                    ootmm::presence::ModAnnounce announce;
                    announce.playerId = s.seed.playerId;
                    announce.game = model.game;
                    announce.name = model.name;
                    announce.hash = model.hash;
                    announce.size = model.bytes.size();
                    s.multiworld->QueueLine(ootmm::presence::BuildModAnnouncePacket(s.seed, announce));
                }
            }
            gPresence.roster[pose->playerId] = { std::move(*pose), std::chrono::steady_clock::now() };
            gPresence.rosterDirty = true;
        }
        return true;
    }
    if (auto sessionMeta = ootmm::presence::ParseSessionMetaPacket(line); sessionMeta.has_value()) {
        if (gPresence.pvpActive != sessionMeta->pvp) {
            gPresence.pvpActive = sessionMeta->pvp;
            gPresence.rosterDirty = true; // re-derive the roster meta the games read
            std::cout << "[presence] server rule: PvP " << (sessionMeta->pvp ? "enabled" : "disabled") << std::endl;
        }
        if (gPresence.deathLinkActive != sessionMeta->deathLink) {
            gPresence.deathLinkActive = sessionMeta->deathLink;
            gPresence.rosterDirty = true;
            std::cout << "[presence] server rule: death link " << (sessionMeta->deathLink ? "enabled" : "disabled")
                      << std::endl;
        }
        if (gPresence.timerEnabled != sessionMeta->timerEnabled || gPresence.timerMin != sessionMeta->timerMin ||
            gPresence.timerMax != sessionMeta->timerMax) {
            gPresence.timerEnabled = sessionMeta->timerEnabled;
            gPresence.timerMin = sessionMeta->timerMin;
            gPresence.timerMax = sessionMeta->timerMax;
            // Server rule change can shrink the allowed range — clamp the live slider value.
            gPresence.speedPercent = (std::clamp)(gPresence.speedPercent, gPresence.timerMin,
                                                  (std::max)(gPresence.timerMin, gPresence.timerMax));
            if (!gPresence.timerEnabled) {
                gPresence.speedPercent = 100;
            }
            gPresence.rosterDirty = true;
        }
        return true;
    }
    if (auto hit = ootmm::presence::ParsePvpHitPacket(line); hit.has_value()) {
        if (hit->targetPlayer == s.seed.playerId) {
            gPresence.pendingHits.push_back({ *hit, std::chrono::steady_clock::now() });
            gPresence.rosterDirty = true;
        }
        return true;
    }
    if (auto death = ootmm::presence::ParseDeathLinkPacket(line); death.has_value()) {
        // Everyone in the room dies with the sender (except the sender itself, which already
        // died). Only honored while the server's death-link rule is on.
        if (gPresence.deathLinkActive && death->sourcePlayer != s.seed.playerId) {
            gPresence.pendingDeaths.push_back({ *death, std::chrono::steady_clock::now() });
            gPresence.rosterDirty = true;
        }
        return true;
    }
    if (auto announce = ootmm::presence::ParseModAnnouncePacket(line); announce.has_value()) {
        if (announce->playerId == s.seed.playerId) {
            return true;
        }
        // Track the player's model set for this game; a new archive re-arms installation.
        const uint32_t groupKey = (static_cast<uint32_t>(announce->playerId) << 1) |
                                  (announce->game == ootmm::Game::Mm ? 1u : 0u);
        PresenceCoord::RemoteGroup& group = gPresence.remoteGroups[groupKey];
        bool inGroup = false;
        for (const ootmm::presence::ModAnnounce& known : group.announces) {
            inGroup |= known.hash == announce->hash;
        }
        if (!inGroup) {
            group.announces.push_back(*announce);
            group.installed = false;
        }
        if (VerifyCachedModel(app, *announce)) {
            std::string& note = gPresence.remoteModelNote[announce->playerId];
            if (note.find(announce->name) == std::string::npos) {
                note += (note.empty() ? "" : ", ") + announce->name + " (cached)";
            }
            TryInstallRemoteModels(app);
            return true;
        }
        // A transfer that stopped making progress (its sender crashed or disconnected
        // mid-stream) is abandoned so the re-announce below re-requests it from scratch.
        if (const auto it = gPresence.incoming.find(announce->hash);
            it != gPresence.incoming.end() &&
            std::chrono::steady_clock::now() - it->second.lastActivity > std::chrono::seconds(30)) {
            std::cerr << "[presence] transfer of " << announce->name << " stalled; re-requesting" << std::endl;
            gPresence.incoming.erase(it);
        }
        if (gPresence.incoming.find(announce->hash) == gPresence.incoming.end()) {
            gPresence.incoming[announce->hash] =
                PresenceCoord::Incoming{ *announce, {}, 0, std::chrono::steady_clock::now() };
            ootmm::presence::ModRequest request;
            request.playerId = s.seed.playerId;
            request.targetPlayer = announce->playerId;
            request.hash = announce->hash;
            s.multiworld->QueueLine(ootmm::presence::BuildModRequestPacket(s.seed, request));
            gPresence.remoteModelNote[announce->playerId] = announce->name + " (downloading...)";
            std::cout << "[presence] requesting model " << announce->name << " from player " << announce->playerId
                      << std::endl;
        }
        return true;
    }
    if (auto request = ootmm::presence::ParseModRequestPacket(line); request.has_value()) {
        if (request->targetPlayer != s.seed.playerId) {
            return true;
        }
        for (size_t i = 0; i < gPresence.localModels.size(); ++i) {
            const PresenceCoord::LocalModel& model = gPresence.localModels[i];
            if (model.hash != request->hash) {
                continue;
            }
            bool already = false;
            for (const PresenceCoord::SendJob& job : gPresence.sending) {
                already |= job.hash == request->hash;
            }
            if (!already) {
                gPresence.sending.push_back(PresenceCoord::SendJob{
                    model.hash, i, 0,
                    static_cast<uint32_t>((model.bytes.size() + kModChunkRaw - 1) / kModChunkRaw) });
                std::cout << "[presence] streaming model " << model.name << " to the session" << std::endl;
            }
        }
        return true;
    }
    if (auto chunk = ootmm::presence::ParseModChunkPacket(line); chunk.has_value()) {
        const auto it = gPresence.incoming.find(chunk->hash);
        if (it == gPresence.incoming.end() || chunk->total == 0 || chunk->seq >= chunk->total) {
            return true;
        }
        PresenceCoord::Incoming& incoming = it->second;
        if (incoming.chunks.size() != chunk->total) {
            incoming.chunks.assign(chunk->total, std::string());
        }
        if (incoming.chunks[chunk->seq].empty()) {
            incoming.chunks[chunk->seq] = chunk->dataB64;
            ++incoming.received;
            incoming.lastActivity = std::chrono::steady_clock::now();
        }
        if (incoming.received == chunk->total) {
            std::vector<uint8_t> bytes;
            for (const std::string& part : incoming.chunks) {
                const std::vector<uint8_t> decoded = ootmm::presence::Base64Decode(part);
                bytes.insert(bytes.end(), decoded.begin(), decoded.end());
            }
            const std::string hash = ootmm::presence::HashBytesHex(bytes.data(), bytes.size());
            bool stored = false;
            if (hash == chunk->hash) {
                // Write-then-rename so a crash mid-write can never leave a plausibly-named
                // partial file in the cache; only a fully-flushed archive gets the real name.
                const fs::path cached = CachedModelPath(app, incoming.meta);
                const fs::path partial = cached.string() + ".part";
                std::ofstream out(partial, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                out.close();
                std::error_code ec;
                if (out.good()) {
                    fs::remove(cached, ec);
                    fs::rename(partial, cached, ec);
                    stored = !ec;
                }
                if (stored) {
                    gPresence.verifiedCache.insert(incoming.meta.hash);
                    gPresence.remoteModelNote[incoming.meta.playerId] = incoming.meta.name + " (downloaded)";
                    std::cout << "[presence] downloaded model " << incoming.meta.name << " (" << bytes.size()
                              << " bytes) -> " << cached.string() << std::endl;
                } else {
                    fs::remove(partial, ec);
                    gPresence.remoteModelNote[incoming.meta.playerId] = incoming.meta.name + " (cache write failed)";
                    std::cerr << "[presence] failed to write cache file for " << incoming.meta.name << std::endl;
                }
            }
            if (!stored && hash != chunk->hash) {
                // The assembled bytes don't match the announce: whatever arrived is unusable.
                // Reset the transfer and ask the sender to stream it again from memory.
                gPresence.remoteModelNote[incoming.meta.playerId] = incoming.meta.name + " (retrying...)";
                std::cerr << "[presence] model transfer hash mismatch for " << incoming.meta.name
                          << "; re-requesting" << std::endl;
                incoming.chunks.clear();
                incoming.received = 0;
                incoming.lastActivity = std::chrono::steady_clock::now();
                ootmm::presence::ModRequest request;
                request.playerId = s.seed.playerId;
                request.targetPlayer = incoming.meta.playerId;
                request.hash = incoming.meta.hash;
                s.multiworld->QueueLine(ootmm::presence::BuildModRequestPacket(s.seed, request));
                return true;
            }
            if (stored) {
                TryInstallRemoteModels(app);
            }
            gPresence.incoming.erase(it);
        }
        return true;
    }
    return false;
}

// Per-UI-frame presence pump: forward fresh local poses, stream pending model chunks, and
// mirror the (expired-filtered) remote roster into the active port's presence-in file.
void PumpPresence(GuiApp& app) {
    Session& s = app.session;
    // The roster-file mirror at the bottom must run even in SOLO sessions: the roster is empty
    // there, but the META line (game speed, nametag toggle) is how those options reach the
    // game — gating everything on multiworld silently disconnected the timer slider in solo.
    if (s.multiworld && !s.presenceOut.empty()) {
        if (auto pose = ootmm::presence::ReadPoseFile(s.presenceOut);
            pose.has_value() && pose->seq != gPresence.lastSentSeq) {
            gPresence.lastSentSeq = pose->seq;
            // The port stamps the pose from the seed it actually booted; never overwrite it.
            // A mismatch means the game is running a DIFFERENT player's seed than this
            // session — forwarding it restamped is how ghost puppets are born.
            const bool poseOwned = pose->playerId == 0 || pose->playerId == s.seed.playerId;
            if (!poseOwned) {
                gPresence.identityWarning =
                    "The running game reports Player " + std::to_string(pose->playerId) +
                    " but this session is Player " + std::to_string(s.seed.playerId) +
                    " - it booted another player's seed. Restart the session from this launcher.";
            }
            pose->playerId = s.seed.playerId; // 0 = older port build without pose ids
            pose->clientName = app.cfg.playerName;
            // The model identity is the whole SET of archives for this game (child/adult/
            // textures/voice), joined deterministically so receivers can tell when they have
            // every piece cached.
            std::string setHash;
            for (const PresenceCoord::LocalModel& model : gPresence.localModels) {
                if (model.game == pose->game) {
                    if (!setHash.empty()) {
                        setHash += "+";
                    }
                    setHash += model.hash;
                }
            }
            pose->modelHash = setHash;
            if (poseOwned) {
                s.multiworld->QueueLine(ootmm::presence::BuildPlayerStatePacket(s.seed, *pose));
            }
        }
    }
    // Periodic model re-announce: a launcher that restarts (or crashes) mid-session loses
    // every announce it ever received, and peers' knownPeers gate suppresses a re-send - so
    // its remote players would stay vanilla forever. Announces are tiny and receivers dedup
    // by hash, so a low-rate broadcast makes the session converge no matter who rebooted.
    {
        const auto now = std::chrono::steady_clock::now();
        if (s.multiworld && !gPresence.localModels.empty() &&
            now - gPresence.lastAnnounce >= std::chrono::seconds(20)) {
            gPresence.lastAnnounce = now;
            for (const PresenceCoord::LocalModel& model : gPresence.localModels) {
                ootmm::presence::ModAnnounce announce;
                announce.playerId = s.seed.playerId;
                announce.game = model.game;
                announce.name = model.name;
                announce.hash = model.hash;
                announce.size = model.bytes.size();
                s.multiworld->QueueLine(ootmm::presence::BuildModAnnouncePacket(s.seed, announce));
            }
        }
    }
    // Model streaming: a couple of chunks per frame keeps the relay responsive.
    for (int budget = 2; s.multiworld && budget > 0 && !gPresence.sending.empty(); --budget) {
        PresenceCoord::SendJob& job = gPresence.sending.front();
        const PresenceCoord::LocalModel& model = gPresence.localModels[job.modelIndex];
        const size_t offset = static_cast<size_t>(job.nextSeq) * kModChunkRaw;
        const size_t remaining = model.bytes.size() > offset ? model.bytes.size() - offset : 0;
        const size_t take = remaining < kModChunkRaw ? remaining : kModChunkRaw;
        ootmm::presence::ModChunk chunk;
        chunk.playerId = s.seed.playerId;
        chunk.hash = job.hash;
        chunk.seq = job.nextSeq;
        chunk.total = job.total;
        chunk.dataB64 = ootmm::presence::Base64Encode(model.bytes.data() + offset, take);
        s.multiworld->QueueLine(ootmm::presence::BuildModChunkPacket(s.seed, chunk));
        if (++job.nextSeq >= job.total) {
            std::cout << "[presence] finished streaming model " << model.name << std::endl;
            gPresence.sending.erase(gPresence.sending.begin());
        }
    }
    if (!s.presenceIn.empty()) {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = gPresence.roster.begin(); it != gPresence.roster.end();) {
            if (now - it->second.seen > std::chrono::seconds(3)) {
                it = gPresence.roster.erase(it);
                gPresence.rosterDirty = true;
            } else {
                ++it;
            }
        }
        // PvP hits stay in the roster file for ~2s (the game dedups by attacker+seq), then
        // age out so a later game boot can never replay them.
        for (auto it = gPresence.pendingHits.begin(); it != gPresence.pendingHits.end();) {
            if (now - it->received > std::chrono::seconds(2)) {
                it = gPresence.pendingHits.erase(it);
                gPresence.rosterDirty = true;
            } else {
                ++it;
            }
        }
        // Death-link events age out the same way (the game dedups by source+seq).
        for (auto it = gPresence.pendingDeaths.begin(); it != gPresence.pendingDeaths.end();) {
            if (now - it->received > std::chrono::seconds(2)) {
                it = gPresence.pendingDeaths.erase(it);
                gPresence.rosterDirty = true;
            } else {
                ++it;
            }
        }
        if (gPresence.rosterDirty) {
            std::vector<ootmm::presence::PlayerPose> roster;
            roster.reserve(gPresence.roster.size());
            for (const auto& [id, remote] : gPresence.roster) {
                roster.push_back(remote.pose);
            }
            ootmm::presence::RosterMeta meta;
            meta.showNames = app.cfg.showPlayerNametags;
            meta.pvp = gPresence.pvpActive;
            meta.deathLink = gPresence.deathLinkActive;
            meta.speedPercent = gPresence.timerEnabled ? gPresence.speedPercent : 100;
            meta.speedSmooth = app.cfg.speedMatchFramerate;
            std::vector<ootmm::presence::PvpHit> hits;
            hits.reserve(gPresence.pendingHits.size());
            for (const PresenceCoord::TimedHit& timed : gPresence.pendingHits) {
                hits.push_back(timed.hit);
            }
            std::vector<ootmm::presence::DeathEvent> deaths;
            deaths.reserve(gPresence.pendingDeaths.size());
            for (const PresenceCoord::TimedDeath& timed : gPresence.pendingDeaths) {
                deaths.push_back(timed.death);
            }
            ootmm::presence::WriteRosterFile(s.presenceIn, roster, &meta, &hits, &deaths);
            gPresence.rosterDirty = false;
        }
    }
}

// Exchange packets with the multiworld relay: flush queued outbound lines and deliver every
// inbound item addressed to the local player into the active game's inbox (the port replays
// inboxes from offset 0 and dedups by deliveryKey, so replays across relaunches are safe).
void PumpMultiworld(GuiApp& app) {
    Session& s = app.session;
    if (!s.multiworld) {
        return;
    }
    std::vector<std::string> inbound;
    s.multiworld->Pump(inbound);
    std::vector<std::string> forActiveGame;
    for (const std::string& line : inbound) {
        if (HandlePresenceLine(app, line)) {
            continue; // ephemeral presence / model-sync traffic never touches the item inbox
        }
        if (const auto event = ootmm::anchor::ParseGiveItemPacket(line);
            event.has_value() && event->targetPlayer == s.seed.playerId) {
            forActiveGame.push_back(line);
            // Count the receipt for the logic solver (deduped by delivery key).
            app.tracker.AddRemoteReceipt(event->item.id, event->deliveryKey);
            continue;
        }
        // Check completions from other players AND the server's join replay (the relay stores
        // every check/item packet and replays the room history to joiners, so a fresh save
        // restores its collected-check state without recollecting anything).
        if (const auto check = ootmm::anchor::ParseCheckCompletePacket(line); check.has_value()) {
            app.tracker.MarkCollected(check->checkGame, check->checkId, check->item.name, check->targetPlayer,
                                      s.seed.playerId, /*logIt=*/false);
            forActiveGame.push_back(line); // the game marks the check completed in its runtime
            continue;
        }
        // Server console chat (/say) and teleport orders are surfaced/forwarded as-is.
        const std::string type = ootmm::relay::ExtractJsonStringField(line, "type");
        if (type == "OOTMM_CHAT") {
            std::cout << "[server] " << ootmm::relay::ExtractJsonStringField(line, "from") << ": "
                      << ootmm::relay::ExtractJsonStringField(line, "text") << std::endl;
            continue;
        }
        if (type == "OOTMM_TELEPORT") {
            // The launcher owns identity: forward only orders addressed to OUR player, matched
            // by client name or player-id=N. The game then executes the dest.
            const std::string who = ootmm::relay::ExtractJsonStringField(line, "who");
            bool forMe = false;
            if (who.rfind("player-id=", 0) == 0) {
                forMe = std::atoi(who.c_str() + 10) == static_cast<int>(s.seed.playerId);
            } else {
                forMe = !who.empty() && who == app.cfg.playerName;
            }
            if (forMe) {
                // Resolve a username dest to player-id=N here — only the launcher knows the
                // name<->id mapping (from the roster poses). entrance=/player-id= pass through.
                std::string dest = ootmm::relay::ExtractJsonStringField(line, "dest");
                if (dest.rfind("entrance=", 0) != 0 && dest.rfind("player-id=", 0) != 0) {
                    uint16_t resolved = 0;
                    for (const auto& [pid, remote] : gPresence.roster) {
                        if (remote.pose.clientName == dest) {
                            resolved = pid;
                            break;
                        }
                    }
                    if (resolved == 0) {
                        std::cout << "[server] teleport order dropped: unknown destination '" << dest << "'"
                                  << std::endl;
                        continue;
                    }
                    dest = "player-id=" + std::to_string(resolved);
                }
                std::cout << "[server] teleport order: " << dest << std::endl;
                forActiveGame.push_back(std::string("{\"type\":\"OOTMM_TELEPORT\",\"dest\":\"") + dest + "\"}");
            }
            continue;
        }
        if (type == "OOTMM_ADMIN_UNLOCK_ALL") {
            // Server /unlock-all: same identity rules as teleport, plus "all". The GAME executes
            // it (it owns the seed and the collect pipeline); the launcher only routes.
            const std::string who = ootmm::relay::ExtractJsonStringField(line, "who");
            bool forMe = who == "all";
            if (!forMe && who.rfind("player-id=", 0) == 0) {
                forMe = std::atoi(who.c_str() + 10) == static_cast<int>(s.seed.playerId);
            } else if (!forMe) {
                forMe = !who.empty() && who == app.cfg.playerName;
            }
            if (forMe) {
                std::string game = ootmm::relay::ExtractJsonStringField(line, "game");
                if (game != "oot" && game != "mm") {
                    game = "all";
                }
                std::cout << "[server] unlock-all order: game=" << game << std::endl;
                forActiveGame.push_back(std::string("{\"type\":\"OOTMM_ADMIN_UNLOCK_ALL\",\"game\":\"") + game +
                                        "\"}");
            }
            continue;
        }
    }
    if (!forActiveGame.empty()) {
        std::cout << "[multiworld] received " << forActiveGame.size() << " item(s)" << std::endl;
        ootmm::AppendPacketLines(app.run.activeInbox, forActiveGame);
    }
}

// One coordinator step (100 ms cadence while in the runtime stage).
void TickRuntime(GuiApp& app) {
    RuntimeState& run = app.run;
    if (!run.process) {
        return;
    }

    DrainActiveOutbox(app);
    PumpMultiworld(app);

    if (run.pendingTransition.has_value()) {
        // Give the game a brief moment to flush its save, then relaunch the destination.
        if (std::chrono::steady_clock::now() >= run.graceDeadline) {
            if (run.process->Running()) {
                run.process->Terminate();
            }
            run.activeGame = run.pendingTransition->toGame;
            run.bootEntrance = run.pendingTransition->toNativeId;
            run.bootOotAge = run.pendingTransition->ootAge;
            StartActiveGame(app);
        }
    } else if (!run.process->Running()) {
        // The game exited without signalling a transition -> the player quit (or it crashed).
        std::cout << "[launcher] " << GameName(run.activeGame) << " exited; ending session." << std::endl;
        EndSession(app, std::string(GameTitle(run.activeGame)) + " exited - session closed.");
        return;
    }

    // Reconcile the tracker's durable set from state.json occasionally (the port rewrites it
    // on every in-game save); live packets keep it current between saves.
    const auto now = std::chrono::steady_clock::now();
    if (now - run.lastStateSync > std::chrono::seconds(5)) {
        run.lastStateSync = now;
        app.tracker.SyncFromStateFile(app.session.statePath);
    }

    // Re-run the reverse-logic solver whenever the item/check state changed.
    if (app.tracker.Revision() != app.solvedRevision) {
        RecomputeLogic(app);
    }

    // Keep the embedded game sized to its region; (re)discover after a relaunch.
    if (run.gameChild == nullptr || !IsWindow(run.gameChild)) {
        run.gameChild = FindGameChild(app.host, run.activePid);
        if (run.gameChild != nullptr) {
            LayoutGameChild(app);
            FocusGameChild(app);
        }
    }
}

// ---------------------------------------------------------------------------
// Config screen helpers
// ---------------------------------------------------------------------------

void CopyToBuffer(char* buffer, size_t size, const std::string& value) {
    const size_t n = value.size() < size - 1 ? value.size() : size - 1;
    std::memcpy(buffer, value.data(), n);
    buffer[n] = '\0';
}

void RefreshSeedPreview(ConfigForm& form) {
    form.loadedSeedPath = form.seedPath;
    form.seed.reset();
    form.seedError.clear();
    if (form.loadedSeedPath.empty()) {
        return;
    }
    std::error_code ec;
    if (!fs::is_regular_file(fs::path(form.loadedSeedPath), ec)) {
        form.seedError = "File not found.";
        return;
    }
    try {
        form.seed = ootmm::LoadSeedFromFile(form.loadedSeedPath);
    } catch (const std::exception& error) {
        form.seedError = error.what();
    }
}

void RefreshMods(ConfigForm& form, bool oot) {
    const std::string exePath = oot ? form.ootExe : form.mmExe;
    std::string& scannedDir = oot ? form.ootModsDir : form.mmModsDir;
    std::vector<ModEntry>& mods = oot ? form.ootMods : form.mmMods;
    const std::string dir = exePath.empty() ? std::string() : fs::path(exePath).parent_path().string();
    scannedDir = dir;
    mods = dir.empty() ? std::vector<ModEntry>() : ootmm::launcher::ScanMods(dir);
}

void SyncFormFromConfig(GuiApp& app) {
    ConfigForm& form = app.form;
    CopyToBuffer(form.seedPath, sizeof(form.seedPath), app.cfg.seedPath);
    CopyToBuffer(form.ootExe, sizeof(form.ootExe), app.cfg.ootExe);
    CopyToBuffer(form.mmExe, sizeof(form.mmExe), app.cfg.mmExe);
    CopyToBuffer(form.mwAddress, sizeof(form.mwAddress), app.cfg.multiworldAddress);
    CopyToBuffer(form.mwName, sizeof(form.mwName), app.cfg.playerName);
    CopyToBuffer(form.playerModelOot, sizeof(form.playerModelOot), app.cfg.playerModelOot);
    CopyToBuffer(form.playerModelMm, sizeof(form.playerModelMm), app.cfg.playerModelMm);
    form.hostRelay = app.cfg.hostRelay;
    form.relayPort = app.cfg.relayPort;
    form.hostPvp = app.cfg.enablePvp;
    RefreshSeedPreview(form);
    RefreshMods(form, true);
    RefreshMods(form, false);
}

void SyncConfigFromForm(GuiApp& app) {
    ConfigForm& form = app.form;
    app.cfg.seedPath = form.seedPath;
    app.cfg.ootExe = form.ootExe;
    app.cfg.mmExe = form.mmExe;
    app.cfg.multiworldAddress = form.mwAddress;
    app.cfg.playerName = form.mwName;
    app.cfg.playerModelOot = form.playerModelOot;
    app.cfg.playerModelMm = form.playerModelMm;
    app.cfg.hostRelay = form.hostRelay;
    app.cfg.relayPort = form.relayPort;
    app.cfg.enablePvp = form.hostPvp;
    ootmm::launcher::SaveLauncherConfig(app.configPath, app.cfg);
}

std::vector<std::string> SplitModelList(const char* buffer); // defined with the config UI below
void JoinModelList(char* buffer, size_t bufferSize, const std::vector<std::string>& names); // ditto

// Re-list a game's mods dir while keeping the user's un-applied checkbox states, then drop
// player-model selections whose archives no longer exist on disk. Ran throttled while the
// config screen is up and again at launch: dropping a model archive into mods/ while the
// launcher is open must be enough to see it, and a stale selection (files renamed/removed)
// must not silently announce nothing to the session.
void RescanMods(ConfigForm& form, bool oot) {
    std::vector<ModEntry>& mods = oot ? form.ootMods : form.mmMods;
    std::map<std::string, bool> toggles;
    for (const ModEntry& mod : mods) {
        toggles[mod.name] = mod.enabled;
    }
    RefreshMods(form, oot);
    for (ModEntry& mod : mods) {
        const auto it = toggles.find(mod.name);
        if (it != toggles.end() && !mod.required) {
            mod.enabled = it->second;
        }
    }
    if ((oot ? form.ootModsDir : form.mmModsDir).empty()) {
        return; // no exe path yet - keep the selection until we can list real files
    }
    char* modelBuffer = oot ? form.playerModelOot : form.playerModelMm;
    const size_t modelBufferSize = oot ? sizeof(form.playerModelOot) : sizeof(form.playerModelMm);
    const std::vector<std::string> selected = SplitModelList(modelBuffer);
    std::vector<std::string> kept;
    for (const std::string& name : selected) {
        for (const ModEntry& mod : mods) {
            if (mod.name == name) {
                kept.push_back(name);
                break;
            }
        }
    }
    if (kept.size() != selected.size()) {
        JoinModelList(modelBuffer, modelBufferSize, kept);
    }
}

// The player-model set only works when its archives are enabled mods: locally they ARE the
// player's skin (plus they must be readable to announce/stream them to the session).
void ForceEnableModelMods(std::vector<ModEntry>& mods, const char* modelList) {
    for (const std::string& name : SplitModelList(modelList)) {
        for (ModEntry& mod : mods) {
            if (mod.name == name) {
                mod.enabled = true;
            }
        }
    }
}

// Sync a port's own config with the launcher's mods list before it boots:
// - Force the alternate-assets CVar (the ports' "Enable Mods" master toggle) ON so enabled
//   mods actually apply from the first boot. Per-mod control stays with the launcher's list:
//   disabled archives are renamed .disabled and never mount. Alt-based model mods must apply
//   through this flag (NOT as canonical-strip overrides): the canonical Link assets must stay
//   vanilla so remote puppets without a synced model don't inherit the local player's skin.
// - Mirror the enabled archives into the port's EnabledMods CVar so the in-game Mod Menu
//   matches the launcher's mods list (names without extension, '|'-separated, mount order =
//   list order so later entries override earlier ones). Launcher-generated zz_* archives are
//   auto-added by the port's own scan at boot.
void SyncPortModConfig(const fs::path& exe, const char* configName, const char* altAssetsKey,
                       const std::vector<ModEntry>& mods) {
    ootmm::launcher::GameConfig config;
    if (!config.Load(exe.parent_path() / configName)) {
        return;
    }
    config.SetCVarInt(altAssetsKey, 1);
    std::string enabled;
    for (const ModEntry& mod : mods) {
        if (!(mod.required || mod.enabled)) {
            continue;
        }
        enabled += (enabled.empty() ? "" : "|") + fs::path(mod.name).stem().string();
    }
    config.SetCVarString("gSettings.EnabledMods", enabled);
    if (!config.Save()) {
        std::cerr << "[launcher] failed to update " << configName << " mod settings" << std::endl;
    }
    // Retired local canonical-strip skins from earlier builds: they overrode the canonical
    // Link assets, so puppets' vanilla fallback wore the LOCAL player's model.
    std::error_code ec;
    for (const auto& p : fs::directory_iterator(exe.parent_path() / "mods", ec)) {
        if (p.path().filename().string().rfind("zz_localmodel_", 0) == 0) {
            fs::remove(p.path(), ec);
        }
    }
}

bool ParseHostPort(const std::string& address, std::string& host, uint16_t& port) {
    const size_t colon = address.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= address.size()) {
        return false;
    }
    try {
        const unsigned long value = std::stoul(address.substr(colon + 1));
        if (value == 0 || value > 65535) {
            return false;
        }
        port = static_cast<uint16_t>(value);
    } catch (...) {
        return false;
    }
    host = address.substr(0, colon);
    return true;
}

// True when the loaded seed places checks on Master Quest dungeon layouts — those scenes
// only exist in oot-mq.o2r, so setup must also extract a Master Quest ROM. Derived from
// the placements themselves (MQ check ids are namespaced OOT_MQ_*), not a hand-kept list,
// so new seed content keeps working without launcher changes.
bool SeedNeedsMq(const ConfigForm& form) {
    if (!form.seed.has_value()) {
        return false;
    }
    for (const ootmm::ItemPlacement& p : form.seed->placements) {
        if (p.check.game == ootmm::Game::Oot && p.check.id.rfind("OOT_MQ_", 0) == 0) {
            return true;
        }
    }
    return false;
}

// The archives a game still needs before it can boot this seed, checked next to the exe
// (the ports run portable: GetAppDirectoryPath resolves to "." and the launcher sets the
// child's cwd to the exe dir, so that is where their extractors read and write archives).
// Empty when the exe itself is missing — that is reported separately.
std::vector<std::string> MissingGameArchives(const char* exeBuffer, bool isOot, bool needMq) {
    std::vector<std::string> missing;
    std::error_code ec;
    const fs::path exe(exeBuffer);
    if (exeBuffer[0] == '\0' || !fs::is_regular_file(exe, ec)) {
        return missing;
    }
    const fs::path dir = exe.parent_path();
    std::vector<const char*> required = isOot ? std::vector<const char*>{ "soh.o2r", "oot.o2r" }
                                              : std::vector<const char*>{ "2ship.o2r", "mm.o2r" };
    if (isOot && needMq) {
        required.push_back("oot-mq.o2r");
    }
    for (const char* name : required) {
        ec.clear();
        if (!fs::exists(dir / name, ec)) {
            missing.emplace_back(name);
        }
    }
    return missing;
}

// True when both exes are set and every base game archive is present.
bool GameArchivesReady(const ConfigForm& form) {
    std::error_code ec;
    return form.ootExe[0] != '\0' && fs::is_regular_file(fs::path(form.ootExe), ec) && form.mmExe[0] != '\0' &&
           fs::is_regular_file(fs::path(form.mmExe), ec) &&
           MissingGameArchives(form.ootExe, true, SeedNeedsMq(form)).empty() &&
           MissingGameArchives(form.mmExe, false, false).empty();
}

// True when the cross-game model archives need a (re)build: missing, or older than the base
// archives they are derived from. Only meaningful once both exes are set.
bool ModelModsNeeded(const ConfigForm& form, bool checkOot = true, bool checkMm = true) {
    std::error_code ec;
    if (form.ootExe[0] == '\0' || !fs::is_regular_file(fs::path(form.ootExe), ec) || form.mmExe[0] == '\0' ||
        !fs::is_regular_file(fs::path(form.mmExe), ec)) {
        return false;
    }
    return ootmm::launcher::ForeignModelModsStale(fs::absolute(fs::path(form.ootExe)).parent_path(),
                                                  fs::absolute(fs::path(form.mmExe)).parent_path(), checkOot,
                                                  checkMm);
}

// Queues a setup-only run for every game with missing archives; the model-mods build always
// re-evaluates once the queue drains (fresh extractions make the old archives stale).
void BeginSetup(GuiApp& app) {
    if (app.setup.Active()) {
        return;
    }
    ConfigForm& form = app.form;
    form.validationErrors.clear();
    if (!MissingGameArchives(form.ootExe, true, SeedNeedsMq(form)).empty()) {
        app.setup.queue.push_back(ootmm::Game::Oot);
    }
    if (!MissingGameArchives(form.mmExe, false, false).empty()) {
        app.setup.queue.push_back(ootmm::Game::Mm);
    }
    app.setup.modsPending = true;
}

// Drives the setup pipeline one stage per frame: game extraction children (one at a time),
// then the native model-mods build (worker thread), then the final report.
void TickSetup(GuiApp& app) {
    GuiApp::SetupFlow& setup = app.setup;
    if (setup.child) {
        if (setup.child->Running()) {
            return;
        }
        setup.child.reset();
    }
    if (!setup.queue.empty()) {
        const ootmm::Game game = setup.queue.front();
        setup.queue.erase(setup.queue.begin());
        const bool isOot = game == ootmm::Game::Oot;
        const fs::path exe = fs::absolute(fs::path(isOot ? app.form.ootExe : app.form.mmExe));
        auto child =
            std::make_unique<ChildProcess>(exe, fs::path(), nullptr, true, isOot && SeedNeedsMq(app.form));
        if (child->Valid()) {
            setup.child = std::move(child);
            app.form.statusMessage = std::string("Setup: ") + GameTitle(game) +
                                     " is open - follow its prompts to select your ROM.";
        } else {
            app.form.statusMessage = std::string("Setup: failed to start ") + GameTitle(game) + ".";
        }
        return;
    }
    if (setup.modsBuild.valid()) {
        if (setup.modsBuild.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return;
        }
        const ootmm::launcher::ForeignModsResult result = setup.modsBuild.get();
        for (const std::string& line : result.log) {
            std::cout << "[setup] " << line << std::endl;
        }
        app.form.statusMessage = result.ok
                                     ? "Setup complete - all game assets are ready."
                                     : "Setup: building the model archives failed - " + result.error;
        return;
    }
    if (setup.modsPending) {
        setup.modsPending = false;
        if (!GameArchivesReady(app.form)) {
            app.form.statusMessage = "Setup did not finish - some game assets are still missing.";
            return;
        }
        if (!ModelModsNeeded(app.form)) {
            app.form.statusMessage = "Setup complete - all game assets are ready.";
            return;
        }
        const fs::path sohDir = fs::absolute(fs::path(app.form.ootExe)).parent_path();
        const fs::path mmDir = fs::absolute(fs::path(app.form.mmExe)).parent_path();
        app.form.statusMessage = "Setup: building the cross-game model archives...";
        setup.modsBuild = std::async(std::launch::async, [sohDir, mmDir]() {
            return ootmm::launcher::BuildForeignModelMods(sohDir, mmDir);
        });
    }
}

// Validates the form and, when valid, builds the Session, applies mod toggles, spins up
// multiworld, and boots the starting game. Returns true when the runtime stage started.
bool LaunchFromForm(GuiApp& app) {
    ConfigForm& form = app.form;
    form.validationErrors.clear();
    form.statusMessage.clear();

    // Fresh mods-dir truth at the moment of launch: files added/removed since the last scan
    // are picked up, and stale player-model selections are pruned instead of silently
    // announcing nothing to the co-op session.
    RescanMods(form, true);
    RescanMods(form, false);

    // Re-resolve the preview in case the path was edited without deactivating the field.
    if (form.loadedSeedPath != form.seedPath) {
        RefreshSeedPreview(form);
    }
    if (!form.seed.has_value()) {
        form.validationErrors.push_back(form.seedError.empty() ? "Select a seed file."
                                                               : "Seed: " + form.seedError);
    }
    std::error_code ec;
    if (form.ootExe[0] == '\0' || !fs::is_regular_file(fs::path(form.ootExe), ec)) {
        form.validationErrors.push_back("Ocarina of Time executable (soh.exe) not found.");
    }
    if (form.mmExe[0] == '\0' || !fs::is_regular_file(fs::path(form.mmExe), ec)) {
        form.validationErrors.push_back("Majora's Mask executable (2ship.exe) not found.");
    }

    // Extracted ROM archives: the game cannot boot the seed without them. The port
    // archives (soh.o2r / 2ship.o2r) ship with the build; the rest come from Setup.
    const auto archiveError = [](const char* game, const std::string& name) {
        if (name == "soh.o2r" || name == "2ship.o2r") {
            return std::string(game) + " is missing " + name +
                   " (ships with the game build - re-copy it next to the exe).";
        }
        return std::string(game) + " is missing " + name + " - run Setup to extract it from your ROM.";
    };
    for (const std::string& name : MissingGameArchives(form.ootExe, true, SeedNeedsMq(form))) {
        form.validationErrors.push_back(archiveError("OoT", name));
    }
    for (const std::string& name : MissingGameArchives(form.mmExe, false, false)) {
        form.validationErrors.push_back(archiveError("MM", name));
    }

    // The cross-game model archives are required for foreign items to render as their
    // real assets; verify they are present (enabled or about to be enabled).
    const auto hasRequiredMod = [](const std::vector<ModEntry>& mods, const char* name) {
        for (const ModEntry& mod : mods) {
            if (mod.name == name) {
                return true;
            }
        }
        return false;
    };
    if (form.ootExe[0] != '\0' && !hasRequiredMod(form.ootMods, "ootmm_mm_models.o2r")) {
        form.validationErrors.push_back(
            "OoT mods folder is missing ootmm_mm_models.o2r (cross-game item models) - run Setup to build it.");
    }
    if (form.mmExe[0] != '\0' && !hasRequiredMod(form.mmMods, "ootmm_oot_models.o2r")) {
        form.validationErrors.push_back(
            "MM mods folder is missing ootmm_oot_models.o2r (cross-game item models) - run Setup to build it.");
    }

    const bool isMultiworld = form.seed.has_value() && form.seed->playerCount > 1;
    std::string mwHost;
    uint16_t mwPort = 0;
    if (isMultiworld) {
        const std::string address = form.hostRelay
                                        ? ("127.0.0.1:" + std::to_string(form.relayPort))
                                        : std::string(form.mwAddress);
        if (!ParseHostPort(address, mwHost, mwPort)) {
            form.validationErrors.push_back("Multiworld server address must be host:port.");
        }
        if (form.mwName[0] == '\0') {
            form.validationErrors.push_back("Enter a player name for multiworld.");
        }
    }

    if (!form.validationErrors.empty()) {
        return false;
    }

    // Apply the mod checkbox states (renames archives to/from .disabled). Archives selected
    // as the player model are forced on: they are the local player's skin and must be
    // readable to announce/stream to the session.
    ForceEnableModelMods(form.ootMods, form.playerModelOot);
    ForceEnableModelMods(form.mmMods, form.playerModelMm);
    for (const std::string& error : ootmm::launcher::ApplyModStates(form.ootMods)) {
        form.validationErrors.push_back("OoT mod " + error);
    }
    for (const std::string& error : ootmm::launcher::ApplyModStates(form.mmMods)) {
        form.validationErrors.push_back("MM mod " + error);
    }
    if (!form.validationErrors.empty()) {
        return false;
    }

    // Push the mods list into each port's own config (they are not running yet): master
    // mods/alt-assets toggle ON + EnabledMods mirroring the launcher's list.
    SyncPortModConfig(fs::path(std::string(form.ootExe)), "shipofharkinian.json", "gSettings.AltAssets",
                      form.ootMods);
    SyncPortModConfig(fs::path(std::string(form.mmExe)), "2ship2harkinian.json",
                      "gEnhancements.Mods.AlternateAssets", form.mmMods);

    SyncConfigFromForm(app);

    Session session;
    session.seed = *form.seed;
    session.seedPath = fs::absolute(fs::path(form.seedPath));
    session.ootExe = fs::absolute(fs::path(form.ootExe));
    session.mmExe = fs::absolute(fs::path(form.mmExe));
    session.allowDebugMenus = app.cfg.allowDebugMenus;

    if (const auto error = PrepareSession(session, app.sessionDirOverride); error.has_value()) {
        form.validationErrors.push_back(*error);
        return false;
    }

    if (isMultiworld) {
        if (form.hostRelay) {
            // The relay thread outlives sessions, so its PvP rule lives in a shared atomic:
            // re-hosting with a different checkbox state updates the running relay (which
            // re-broadcasts the rule to every joined client) instead of being silently ignored.
            static std::atomic<bool> hostedRelayPvp{ false };
            hostedRelayPvp.store(form.hostPvp);
            if (!app.relayStarted) {
                app.relayStarted = true;
                const uint16_t relayPort = static_cast<uint16_t>(form.relayPort);
                std::thread([relayPort]() { RunRelayServer(relayPort, hostedRelayPvp); }).detach();
            }
            std::cout << "[launcher] hosting multiworld relay on port " << form.relayPort
                      << (form.hostPvp ? " (PvP enabled)" : "") << std::endl;
        }
        // HARD gate: a multiworld seed never boots without the relay answering first. Booting
        // anyway strands the session (no items, no presence, no session rules). The hosted
        // relay was just started above, so this also verifies its port actually bound.
        std::string pingError;
        bool reachable = false;
        for (int attempt = 0; attempt < 3 && !reachable; ++attempt) {
            reachable = PingRelayServer(mwHost, mwPort, 2000, pingError);
            if (!reachable && form.hostRelay) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200)); // relay thread spin-up
            }
        }
        if (!reachable) {
            form.validationErrors.push_back("Multiworld server unreachable (" + pingError +
                                            "). Start the server, then launch again.");
            return false;
        }
        std::cout << "[launcher] multiworld server responded at " << mwHost << ":" << mwPort << std::endl;
        session.multiworld = std::make_unique<MultiworldClient>(
            mwHost, mwPort, ootmm::anchor::BuildHandshakePacket(session.seed, form.mwName));
        // Upload our check catalog so the server's /list-checks knows the FULL set, not just the
        // checks already collected. Queued right after the handshake so it lands first thing.
        session.multiworld->QueueLine(ootmm::anchor::BuildCheckCatalogPacket(session.seed));
        std::cout << "[launcher] multiworld: player " << session.seed.playerId << " -> " << mwHost << ":" << mwPort
                  << std::endl;
    }

    if (app.selftestArmed) {
        session.selftestReport = session.statePath.parent_path() / "selftest-report.jsonl";
        std::error_code ec;
        fs::remove(session.selftestReport, ec); // fresh report per run
        std::cout << "[launcher] self-test armed — report: " << session.selftestReport.string() << std::endl;
    }

    app.session = std::move(session);
    SetupPresenceModels(app);
    // PvP is a SERVER rule: gPresence.pvpActive stays off until the relay's session-meta
    // packet arrives (sent on join by both the hosted relay and the dedicated server).
    app.tracker.LoadSeed(app.session.seed);
    app.tracker.SyncFromStateFile(app.session.statePath);
    if (app.solver.LoadFromSeedFile(app.session.seedPath)) {
        std::cout << "[launcher] logic graph loaded (mode " << app.solver.LogicMode() << ")" << std::endl;
    } else {
        std::cout << "[launcher] seed has no logic data; availability tracking disabled" << std::endl;
    }
    // Replay persisted multiworld receipts (the inboxes survive relaunches; the same
    // delivery-key dedup the runtime uses keeps the counts exact).
    for (const fs::path& inbox : { app.session.ootInbox, app.session.mmInbox }) {
        uintmax_t offset = 0;
        const ootmm::FilePacketReadResult lines = ootmm::ReadPacketLines(inbox, offset);
        for (const std::string& line : lines.packets) {
            if (const auto event = ootmm::anchor::ParseGiveItemPacket(line);
                event.has_value() && event->targetPlayer == app.session.seed.playerId) {
                app.tracker.AddRemoteReceipt(event->item.id, event->deliveryKey);
            }
        }
    }

    app.run = RuntimeState{};
    app.run.activeGame = app.startOverride.value_or(app.session.seed.ResolveStartingGame());
    app.startOverride.reset(); // a --start override applies to the first launch only
    if (app.selftestArmed && app.run.activeGame == ootmm::Game::Oot) {
        // Fully unattended self-test: boot straight past title/file-select into gameplay (the
        // same handoff boot the cross-game transition uses); the armed walker takes over from
        // there. 0x00BB = Link's House child spawn.
        app.run.bootEntrance = 0x00BB;
    }
    std::cout << "[launcher] seed " << app.session.seed.seedId << " (player " << app.session.seed.playerId << "/"
              << app.session.seed.playerCount << "), starting in " << GameName(app.run.activeGame) << std::endl;

    StartActiveGame(app);
    if (!app.run.process) {
        form.validationErrors.push_back("Failed to launch " + std::string(GameTitle(app.run.activeGame)) + ".");
        return false;
    }
    app.stage = Stage::Runtime;
    return true;
}

// ---------------------------------------------------------------------------
// ImGui screens
// ---------------------------------------------------------------------------

ImFont* HeaderFont() {
    ImGuiIO& io = ImGui::GetIO();
    return io.Fonts->Fonts.Size > 1 ? io.Fonts->Fonts[1] : io.Fonts->Fonts[0];
}

ImU32 WithA(ImU32 color, float alpha01) {
    const ImU32 a = static_cast<ImU32>(alpha01 * 255.0f) << IM_COL32_A_SHIFT;
    return (color & ~IM_COL32_A_MASK) | a;
}

// The active accent palette (OoT gold / MM violet) as packed colors.
struct Palette {
    ImU32 main;
    ImU32 hot;
    ImU32 dim;
    ImVec4 mainV;
};

Palette CurrentPalette(const GuiApp& app) {
    if (CurrentAccent(app) == Accent::OotGold) {
        return { IM_COL32(202, 163, 61, 255), IM_COL32(238, 199, 87, 255), IM_COL32(122, 97, 36, 255),
                 ImVec4(0.79f, 0.64f, 0.24f, 1.0f) };
    }
    return { IM_COL32(148, 112, 219, 255), IM_COL32(181, 145, 250, 255), IM_COL32(87, 64, 140, 255),
             ImVec4(0.58f, 0.44f, 0.86f, 1.0f) };
}

// Real game icon (see tools/gen_launcher_icons.py); draws a placeholder square when missing.
void DrawGameIcon(GuiApp& app, const char* name, float size, float alpha = 1.0f) {
    if (const ootmm::launcher::IconTexture* icon = app.ui.Icon(name); icon != nullptr) {
        ImGui::ImageWithBg(icon->id, ImVec2(size, size), ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0),
                           ImVec4(1, 1, 1, alpha));
    } else {
        ImGui::Dummy(ImVec2(size, size));
    }
}

// Hero button: pulsing golden glow, vertical gradient fill, icon + large label.
bool GlowButton(GuiApp& app, const char* label, const char* iconName, const ImVec2& size) {
    const Palette pal = CurrentPalette(app);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const ImVec2 end = ImVec2(pos.x + size.x, pos.y + size.y);

    const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 2.3f);
    const float glow = hovered ? 0.65f : 0.28f + 0.22f * pulse;
    for (int layer = 3; layer >= 1; --layer) {
        const float spread = 2.5f * static_cast<float>(layer);
        draw->AddRect(ImVec2(pos.x - spread, pos.y - spread), ImVec2(end.x + spread, end.y + spread),
                      WithA(pal.main, glow / (static_cast<float>(layer) * 2.2f)), 10.0f + spread, 0, 2.0f);
    }
    draw->AddRectFilled(pos, end, held ? WithA(pal.dim, 1.0f) : IM_COL32(30, 24, 14, 235), 10.0f);
    draw->AddRectFilledMultiColor(ImVec2(pos.x + 2, pos.y + 2), ImVec2(end.x - 2, pos.y + size.y * 0.45f),
                                  WithA(pal.main, hovered ? 0.32f : 0.18f), WithA(pal.main, hovered ? 0.32f : 0.18f),
                                  WithA(pal.main, 0.02f), WithA(pal.main, 0.02f));
    draw->AddRect(pos, end, hovered ? pal.hot : pal.main, 10.0f, 0, 2.0f);

    // Icon + label centered as one group.
    const ootmm::launcher::IconTexture* icon = app.ui.Icon(iconName);
    ImFont* font = HeaderFont();
    const float iconSize = size.y * 0.55f;
    const ImVec2 textSize = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, label);
    const float totalWidth = (icon != nullptr ? iconSize + 12.0f : 0.0f) + textSize.x;
    float x = pos.x + (size.x - totalWidth) * 0.5f;
    if (icon != nullptr) {
        const float iconY = pos.y + (size.y - iconSize) * 0.5f;
        draw->AddImage(icon->id, ImVec2(x, iconY), ImVec2(x + iconSize, iconY + iconSize));
        x += iconSize + 12.0f;
    }
    draw->AddText(font, font->FontSize, ImVec2(x, pos.y + (size.y - textSize.y) * 0.5f),
                  hovered ? pal.hot : IM_COL32(240, 230, 200, 255), label);
    return clicked;
}

// A left-aligned action button with a real game icon (session menu rows).
bool IconTextButton(GuiApp& app, const char* iconName, const char* label, float height,
                    std::optional<ImU32> fillOverride = std::nullopt) {
    const Palette pal = CurrentPalette(app);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, height);
    const bool clicked = ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const ImVec2 end = ImVec2(pos.x + size.x, pos.y + size.y);

    ImU32 fill = fillOverride.value_or(WithA(pal.dim, 0.55f));
    if (hovered) {
        fill = fillOverride.has_value() ? WithA(*fillOverride, 1.0f) : WithA(pal.dim, 0.95f);
    }
    if (held) {
        fill = WithA(pal.main, 0.85f);
    }
    draw->AddRectFilled(pos, end, fill, 7.0f);
    draw->AddRect(pos, end, WithA(hovered ? pal.hot : pal.main, hovered ? 0.9f : 0.4f), 7.0f, 0, 1.5f);

    const float iconSize = height * 0.62f;
    float x = pos.x + 12.0f;
    if (const ootmm::launcher::IconTexture* icon = app.ui.Icon(iconName); icon != nullptr) {
        const float iconY = pos.y + (height - iconSize) * 0.5f;
        draw->AddImage(icon->id, ImVec2(x, iconY), ImVec2(x + iconSize, iconY + iconSize));
    }
    x += iconSize + 12.0f;
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    draw->AddText(ImVec2(x, pos.y + (height - textSize.y) * 0.5f), IM_COL32(235, 228, 205, 255), label);
    return clicked;
}

// Sidebar tab rail entry: a big icon button with a selection underline.
bool TabIconButton(GuiApp& app, int index, const char* iconName, const char* tooltip, float width) {
    const Palette pal = CurrentPalette(app);
    const bool selected = app.sidebarTab == index;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(width, 44.0f);
    ImGui::PushID(index);
    const bool clicked = ImGui::InvisibleButton("##tab", size);
    ImGui::PopID();
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 end = ImVec2(pos.x + size.x, pos.y + size.y);

    if (selected || hovered) {
        draw->AddRectFilled(pos, end, WithA(pal.dim, selected ? 0.75f : 0.35f), 7.0f);
    }
    if (selected) {
        draw->AddRectFilled(ImVec2(pos.x + 6, end.y - 3), ImVec2(end.x - 6, end.y - 1), pal.hot, 2.0f);
    }
    const float iconSize = 28.0f;
    if (const ootmm::launcher::IconTexture* icon = app.ui.Icon(iconName); icon != nullptr) {
        const ImVec2 iconPos(pos.x + (size.x - iconSize) * 0.5f, pos.y + (size.y - iconSize) * 0.5f - 1.0f);
        draw->AddImage(icon->id, iconPos, ImVec2(iconPos.x + iconSize, iconPos.y + iconSize), ImVec2(0, 0),
                       ImVec2(1, 1), selected || hovered ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 150));
    }
    if (hovered) {
        ImGui::SetTooltip("%s", tooltip);
    }
    return clicked;
}

void DrawModList(std::vector<ModEntry>& mods, const std::string& scannedDir) {
    if (scannedDir.empty()) {
        ImGui::TextDisabled("Set the game path to list its mods.");
        return;
    }
    if (mods.empty()) {
        ImGui::TextDisabled("No mod archives found in mods/.");
        return;
    }
    for (size_t i = 0; i < mods.size(); ++i) {
        ModEntry& mod = mods[i];
        ImGui::PushID(static_cast<int>(i));
        if (mod.required) {
            ImGui::BeginDisabled();
            bool alwaysOn = true;
            ImGui::Checkbox(mod.name.c_str(), &alwaysOn);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("(required)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Cross-game item models - the port needs this archive.");
            }
        } else {
            ImGui::Checkbox(mod.name.c_str(), &mod.enabled);
        }
        ImGui::PopID();
    }
}

// --- Co-op player-model picker (same style as the mods list, multi-select) --------------------
std::vector<std::string> SplitModelList(const char* buffer) {
    std::vector<std::string> names;
    std::string name;
    const std::string list = buffer != nullptr ? buffer : "";
    for (size_t i = 0; i <= list.size(); ++i) {
        const char c = i < list.size() ? list[i] : ';';
        if (c == ';' || c == ',') {
            size_t b = 0, e = name.size();
            while (b < e && std::isspace(static_cast<unsigned char>(name[b]))) ++b;
            while (e > b && std::isspace(static_cast<unsigned char>(name[e - 1]))) --e;
            if (e > b) {
                names.push_back(name.substr(b, e - b));
            }
            name.clear();
        } else {
            name += c;
        }
    }
    return names;
}

void JoinModelList(char* buffer, size_t bufferSize, const std::vector<std::string>& names) {
    std::string joined;
    for (const std::string& name : names) {
        if (!joined.empty()) {
            joined += ";";
        }
        joined += name;
    }
    if (joined.size() >= bufferSize) {
        joined.resize(bufferSize - 1);
    }
    std::memcpy(buffer, joined.c_str(), joined.size() + 1);
}

// Multi-select checkbox list over the game's scanned mod archives; checked entries form the
// player-model set (child/adult/textures/voice packs) synced to the co-op session. Checking
// a model also enables it in the mods list — the set is the local player's own skin.
void DrawPlayerModelList(char* buffer, size_t bufferSize, std::vector<ModEntry>& mods,
                         const std::string& scannedDir) {
    if (scannedDir.empty()) {
        ImGui::TextDisabled("Set the game path to list its mods.");
        return;
    }
    std::vector<std::string> selected = SplitModelList(buffer);
    bool anyListed = false;
    bool changed = false;
    for (size_t i = 0; i < mods.size(); ++i) {
        ModEntry& mod = mods[i];
        if (mod.required) {
            continue; // the cross-game item-model archives are infrastructure, not player skins
        }
        anyListed = true;
        ImGui::PushID(static_cast<int>(i));
        const auto it = std::find(selected.begin(), selected.end(), mod.name);
        bool isSelected = it != selected.end();
        if (ImGui::Checkbox(mod.name.c_str(), &isSelected)) {
            changed = true;
            if (isSelected) {
                selected.push_back(mod.name);
                mod.enabled = true;
            } else {
                selected.erase(std::find(selected.begin(), selected.end(), mod.name));
            }
        }
        if (isSelected && !mod.enabled) {
            ImGui::SameLine();
            ImGui::TextDisabled("(will be enabled at launch)");
        }
        ImGui::PopID();
    }
    if (!anyListed) {
        ImGui::TextDisabled("No model archives found in mods/.");
    }
    if (changed) {
        JoinModelList(buffer, bufferSize, selected);
    }
}

int CountEnabledMods(const std::vector<ModEntry>& mods) {
    int enabled = 0;
    for (const ModEntry& mod : mods) {
        if (mod.required || mod.enabled) {
            ++enabled;
        }
    }
    return enabled;
}

// One game card: emblem, title, engine name, executable path, status and mods.
bool DrawGameCard(GuiApp& app, bool isOot, float width) {
    ConfigForm& form = app.form;
    const Palette pal = CurrentPalette(app);
    char* buffer = isOot ? form.ootExe : form.mmExe;
    const size_t bufferSize = isOot ? sizeof(form.ootExe) : sizeof(form.mmExe);
    std::vector<ModEntry>& mods = isOot ? form.ootMods : form.mmMods;
    const std::string& scannedDir = isOot ? form.ootModsDir : form.mmModsDir;

    bool changed = false;
    ImGui::BeginChild(isOot ? "cardOot" : "cardMm", ImVec2(width, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    const ImVec2 cardMin = ImGui::GetWindowPos();

    ImGui::BeginGroup();
    DrawGameIcon(app, isOot ? "OcarinaOfTime" : "FierceDeityMask", 44.0f);
    ImGui::EndGroup();
    ImGui::SameLine();
    ImGui::BeginGroup();
    const ImVec4 titleColor = isOot ? ImVec4(0.93f, 0.78f, 0.34f, 1.0f) : ImVec4(0.71f, 0.57f, 0.98f, 1.0f);
    ImGui::TextColored(titleColor, "%s", GameTitle(isOot ? ootmm::Game::Oot : ootmm::Game::Mm));
    ImGui::TextDisabled(isOot ? "Ship of Harkinian" : "2 Ship 2 Harkinian");
    ImGui::EndGroup();

    ImGui::Spacing();
    ImGui::SetNextItemWidth(-96.0f);
    ImGui::PushID(isOot ? "oot" : "mm");
    ImGui::InputTextWithHint("##exe", isOot ? "path to soh.exe" : "path to 2ship.exe", buffer, bufferSize);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse", ImVec2(84, 0))) {
        const std::wstring picked = ootmm::launcher::OpenFileDialog(
            app.host, isOot ? L"Select the Ship of Harkinian executable" : L"Select the 2 Ship 2 Harkinian executable",
            L"Executable (*.exe)\0*.exe\0");
        if (!picked.empty()) {
            const int needed = WideCharToMultiByte(CP_UTF8, 0, picked.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string utf8(needed > 0 ? needed - 1 : 0, '\0');
            if (needed > 1) {
                WideCharToMultiByte(CP_UTF8, 0, picked.c_str(), -1, utf8.data(), needed, nullptr, nullptr);
            }
            CopyToBuffer(buffer, bufferSize, utf8);
            changed = true;
        }
    }
    ImGui::PopID();

    std::error_code ec;
    const bool found = buffer[0] != '\0' && fs::is_regular_file(fs::path(buffer), ec);
    const std::vector<std::string> missingArchives = MissingGameArchives(buffer, isOot, isOot && SeedNeedsMq(form));
    if (!found) {
        ImGui::TextColored(ImVec4(0.92f, 0.40f, 0.34f, 1.0f), buffer[0] == '\0' ? "No executable set" : "Not found");
    } else if (!missingArchives.empty()) {
        std::string names;
        for (const std::string& name : missingArchives) {
            names += (names.empty() ? "" : ", ") + name;
        }
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f), "Setup required: missing %s", names.c_str());
    } else if (ModelModsNeeded(form, isOot, !isOot)) {
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f), "Setup required: model archives out of date");
    } else {
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.50f, 1.0f), "Ready");
    }
    ImGui::SameLine(ImGui::GetWindowWidth() - 116.0f);
    ImGui::BeginDisabled(!found);
    if (ImGui::Button("Settings...", ImVec2(96, 0))) {
        OpenGameConfigEditor(app, isOot);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Graphics, audio and input options for this game.");
    }

    char modsLabel[64];
    std::snprintf(modsLabel, sizeof(modsLabel), "Mods (%d of %d enabled)###mods%d", CountEnabledMods(mods),
                  static_cast<int>(mods.size()), isOot ? 0 : 1);
    if (ImGui::TreeNodeEx(modsLabel, ImGuiTreeNodeFlags_SpanAvailWidth)) {
        DrawModList(mods, scannedDir);
        ImGui::TreePop();
    }

    // Corner brackets in the card's OWN game color (gold vs violet), independent of theme.
    const ImU32 cardAccent = isOot ? IM_COL32(202, 163, 61, 255) : IM_COL32(148, 112, 219, 255);
    (void)pal;
    const ImVec2 cardMax = ImVec2(cardMin.x + ImGui::GetWindowWidth(), cardMin.y + ImGui::GetWindowHeight());
    ootmm::launcher::DrawCornerBrackets(ImGui::GetWindowDrawList(), ImVec2(cardMin.x + 4, cardMin.y + 4),
                                        ImVec2(cardMax.x - 4, cardMax.y - 4), WithA(cardAccent, 0.55f), 16.0f, 2.0f);
    ImGui::EndChild();
    return changed;
}

void DrawConfigScreen(GuiApp& app) {
    ConfigForm& form = app.form;
    const Palette pal = CurrentPalette(app);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.045f, 0.035f, 1.0f));
    ImGui::Begin("##config", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Animated backdrop: fairy dust drifting up through a vignette, sky glow on top.
    const ImVec2 workMin = viewport->WorkPos;
    const ImVec2 workMax = ImVec2(workMin.x + viewport->WorkSize.x, workMin.y + viewport->WorkSize.y);
    ootmm::launcher::DrawFairyDust(ImGui::GetWindowDrawList(), workMin, workMax, pal.main, ImGui::GetTime());

    const float scale = app.cfg.uiScale;
    const float columnWidth = 900.0f * scale;
    const float left = std::floor((ImGui::GetContentRegionAvail().x - columnWidth) * 0.5f);
    if (left > 0) {
        ImGui::SetCursorPosX(std::floor(ImGui::GetCursorPosX() + left));
    }
    // Vertical centering: measured on the previous frame. The auto-resizing child
    // panels need a frame or two to settle, so the raw measurement can oscillate by a
    // couple of pixels frame-to-frame; hysteresis keeps the offset stable (only real
    // layout changes like opening a mods tree move it) and flooring pins the whole
    // column to a whole pixel so text never shimmers.
    static float lastContentHeight = 0.0f;
    const float topGap =
        std::floor((std::max)(0.0f, (ImGui::GetContentRegionAvail().y - lastContentHeight) * 0.45f));
    if (lastContentHeight > 0.0f && topGap > 0.0f) {
        ImGui::SetCursorPosY(std::floor(ImGui::GetCursorPosY() + topGap));
    }
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + columnWidth);

    // --- Hero header: glowing Triforce above the title -----------------------
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 1.7f);
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        const float centerX = cursor.x + columnWidth * 0.5f;
        ootmm::launcher::DrawTriforce(draw, ImVec2(centerX, cursor.y + 34.0f), 26.0f, pal.main, pulse);
        ImGui::Dummy(ImVec2(columnWidth, 68.0f));

        ImGui::PushFont(HeaderFont());
        const char* title = "OoTMM PC";
        const ImVec2 titleSize = ImGui::CalcTextSize(title);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (columnWidth - titleSize.x) * 0.5f);
        ImGui::TextColored(pal.mainV, "%s", title);
        ImGui::PopFont();

        const char* subtitle = "O C A R I N A   O F   T I M E   x   M A J O R A ' S   M A S K";
        const ImVec2 subSize = ImGui::CalcTextSize(subtitle);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (columnWidth - subSize.x) * 0.5f);
        ImGui::TextDisabled("%s", subtitle);
        ImGui::Spacing();
    }

    // --- Quest (seed) banner --------------------------------------------------
    bool seedChanged = false;
    ImGui::BeginChild("seedBanner", ImVec2(columnWidth, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    {
        const ImVec2 bannerMin = ImGui::GetWindowPos();
        DrawGameIcon(app, "Compass", 36.0f);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextColored(pal.mainV, "Quest Seed");
        ImGui::SetNextItemWidth(columnWidth - 96.0f - 76.0f);
        ImGui::InputTextWithHint("##seed", "path to seed .json", form.seedPath, sizeof(form.seedPath));
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            seedChanged = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Browse##seed", ImVec2(84, 0))) {
            const std::wstring picked = ootmm::launcher::OpenFileDialog(
                app.host, L"Select an OoTMM seed", L"OoTMM seed (*.json)\0*.json\0All files (*.*)\0*.*\0");
            if (!picked.empty()) {
                const int needed = WideCharToMultiByte(CP_UTF8, 0, picked.c_str(), -1, nullptr, 0, nullptr, nullptr);
                std::string utf8(needed > 0 ? needed - 1 : 0, '\0');
                if (needed > 1) {
                    WideCharToMultiByte(CP_UTF8, 0, picked.c_str(), -1, utf8.data(), needed, nullptr, nullptr);
                }
                CopyToBuffer(form.seedPath, sizeof(form.seedPath), utf8);
                seedChanged = true;
            }
        }
        if (form.seed.has_value()) {
            const ootmm::Seed& seed = *form.seed;
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.50f, 1.0f), "%s", seed.seedId.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("- %d checks - begins in %s%s", static_cast<int>(seed.placements.size()),
                                GameTitle(seed.ResolveStartingGame()),
                                seed.playerCount > 1 ? " - MULTIWORLD" : "");
        } else if (!form.seedError.empty()) {
            ImGui::TextColored(ImVec4(0.92f, 0.40f, 0.34f, 1.0f), "%s", form.seedError.c_str());
        } else {
            ImGui::TextDisabled("Choose the seed to play.");
        }
        ImGui::EndGroup();
        const ImVec2 bannerMax =
            ImVec2(bannerMin.x + ImGui::GetWindowWidth(), bannerMin.y + ImGui::GetWindowHeight());
        ootmm::launcher::DrawCornerBrackets(ImGui::GetWindowDrawList(), ImVec2(bannerMin.x + 4, bannerMin.y + 4),
                                            ImVec2(bannerMax.x - 4, bannerMax.y - 4), WithA(pal.main, 0.55f), 16.0f,
                                            2.0f);
    }
    ImGui::EndChild();
    if (seedChanged) {
        RefreshSeedPreview(form);
    }

    // --- Game cards side by side ----------------------------------------------
    ImGui::Spacing();
    const float half = (columnWidth - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    const bool ootChanged = DrawGameCard(app, true, half);
    ImGui::SameLine();
    const bool mmChanged = DrawGameCard(app, false, half);
    if (ootChanged || form.ootModsDir != (form.ootExe[0] ? fs::path(form.ootExe).parent_path().string() : "")) {
        RefreshMods(form, true);
    }
    if (mmChanged || form.mmModsDir != (form.mmExe[0] ? fs::path(form.mmExe).parent_path().string() : "")) {
        RefreshMods(form, false);
    }
    // Periodic rescan so archives dropped into mods/ while the launcher is open show up in
    // the lists (and selections pointing at removed files fall out) without a restart.
    {
        static double sLastModScan = -1000.0;
        const double now = ImGui::GetTime();
        if (now - sLastModScan >= 2.0) {
            sLastModScan = now;
            RescanMods(form, true);
            RescanMods(form, false);
        }
    }

    // --- Multiworld (only when the seed is multiworld) -----------------------
    if (form.seed.has_value() && form.seed->playerCount > 1) {
        ImGui::Spacing();
        ImGui::BeginChild("multiworld", ImVec2(columnWidth, 0),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        const ImVec2 mwMin = ImGui::GetWindowPos();
        DrawGameIcon(app, "MoonsTear", 36.0f);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextColored(pal.mainV, "Multiworld");
        ImGui::Text("You are player %u of %u.", form.seed->playerId, form.seed->playerCount);
        ImGui::Checkbox("Host the relay server on this machine", &form.hostRelay);
        if (form.hostRelay) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f * scale);
            ImGui::InputInt("port", &form.relayPort, 0, 0);
            if (form.relayPort < 1) form.relayPort = 1;
            if (form.relayPort > 65535) form.relayPort = 65535;
            ImGui::TextDisabled("Other players connect to this machine's IP on that port.");
            ImGui::Checkbox("Enable PvP (server rule)", &form.hostPvp);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Player-vs-player combat for every player on this server: attacks\n"
                                  "damage and knock back other players. Set by the server host;\n"
                                  "players cannot override it. (Dedicated server: ootmm_server --pvp)");
            }
        } else {
            ImGui::TextUnformatted("Server address");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(260.0f * scale);
            ImGui::InputTextWithHint("##mwaddr", "host:port", form.mwAddress, sizeof(form.mwAddress));
        }
        ImGui::TextUnformatted("Player name");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(260.0f * scale);
        ImGui::InputText("##mwname", form.mwName, sizeof(form.mwName));
        ImGui::Spacing();
        ImGui::TextColored(pal.mainV, "Co-op player model");
        ImGui::TextDisabled("Check every archive your model needs (child + adult + textures + voice).");
        ImGui::TextDisabled("The set is synced to the other players; none checked = vanilla Link.");
        {
            const int ootCount = static_cast<int>(SplitModelList(form.playerModelOot).size());
            if (ImGui::TreeNode("pmodeloot", "OoT player model (%d archive%s selected)", ootCount,
                                ootCount == 1 ? "" : "s")) {
                DrawPlayerModelList(form.playerModelOot, sizeof(form.playerModelOot), form.ootMods,
                                    form.ootModsDir);
                ImGui::TreePop();
            }
            const int mmCount = static_cast<int>(SplitModelList(form.playerModelMm).size());
            if (ImGui::TreeNode("pmodelmm", "MM player model (%d archive%s selected)", mmCount,
                                mmCount == 1 ? "" : "s")) {
                DrawPlayerModelList(form.playerModelMm, sizeof(form.playerModelMm), form.mmMods, form.mmModsDir);
                ImGui::TreePop();
            }
        }
        ImGui::EndGroup();
        const ImVec2 mwMax = ImVec2(mwMin.x + ImGui::GetWindowWidth(), mwMin.y + ImGui::GetWindowHeight());
        ootmm::launcher::DrawCornerBrackets(ImGui::GetWindowDrawList(), ImVec2(mwMin.x + 4, mwMin.y + 4),
                                            ImVec2(mwMax.x - 4, mwMax.y - 4), WithA(pal.main, 0.55f), 16.0f, 2.0f);
        ImGui::EndChild();
    }

    // --- Status / validation / launch ----------------------------------------
    ImGui::Spacing();
    if (!form.statusMessage.empty()) {
        const char* msg = form.statusMessage.c_str();
        const ImVec2 msgSize = ImGui::CalcTextSize(msg);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (columnWidth - msgSize.x) * 0.5f);
        ImGui::TextColored(ImVec4(0.75f, 0.72f, 0.55f, 1.0f), "%s", msg);
    }
    for (const std::string& error : form.validationErrors) {
        ImGui::TextColored(ImVec4(0.92f, 0.40f, 0.34f, 1.0f), "- %s", error.c_str());
    }
    ImGui::Spacing();
    {
        // The adventure only starts once both games have their extracted assets AND the
        // cross-game model archives are built; until then the button IS the setup flow
        // (each port opens once and asks for its ROM — including the Master Quest ROM when
        // the seed needs it — then the launcher builds the model archives natively).
        const bool needsSetup = !MissingGameArchives(form.ootExe, true, SeedNeedsMq(form)).empty() ||
                                !MissingGameArchives(form.mmExe, false, false).empty() || ModelModsNeeded(form);
        const float buttonWidth = 380.0f * scale;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (columnWidth - buttonWidth) * 0.5f);
        if (app.setup.Active()) {
            ImGui::BeginDisabled(true);
            GlowButton(app, "SETUP IN PROGRESS...", "OcarinaOfTime", ImVec2(buttonWidth, 62.0f * scale));
            ImGui::EndDisabled();
        } else if (needsSetup) {
            if (GlowButton(app, "SETUP", "OcarinaOfTime", ImVec2(buttonWidth, 62.0f * scale))) {
                BeginSetup(app);
            }
        } else if (GlowButton(app, "START ADVENTURE", "MasterSword", ImVec2(buttonWidth, 62.0f * scale))) {
            LaunchFromForm(app);
        }
        if (needsSetup || app.setup.Active()) {
            const char* setupHint = "First-time setup: each game opens once and asks for your ROM to extract "
                                    "its assets; the launcher then builds the cross-game model archives.";
            const ImVec2 hintSize = ImGui::CalcTextSize(setupHint);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (columnWidth - hintSize.x) * 0.5f);
            ImGui::TextDisabled("%s", setupHint);
            if (SeedNeedsMq(form)) {
                const char* mqHint = "This seed uses Master Quest dungeons - an OoT Master Quest ROM "
                                     "is also required.";
                const ImVec2 mqSize = ImGui::CalcTextSize(mqHint);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (columnWidth - mqSize.x) * 0.5f);
                ImGui::TextDisabled("%s", mqHint);
            }
        }
    }
    ImGui::Spacing();
    {
        const char* hint = "Settings persist to launcher.config.json next to the launcher.";
        const ImVec2 hintSize = ImGui::CalcTextSize(hint);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (columnWidth - hintSize.x) * 0.5f);
        ImGui::TextDisabled("%s", hint);
    }

    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    // Hysteresis: ignore sub-3px measurement wobble from the settling auto-resize
    // children; only genuine layout changes re-center the column.
    const float measuredHeight = ImGui::GetItemRectSize().y;
    if (std::fabs(measuredHeight - lastContentHeight) > 3.0f) {
        lastContentHeight = measuredHeight;
    }
    DrawGameConfigModal(app);
    ImGui::End();
    ImGui::PopStyleColor();
}

// --- Per-game configuration editor ---------------------------------------------
// Edits the port's own config file (shipofharkinian.json / 2ship2harkinian.json, next
// to the exe). The keys and their EXACT JSON types mirror what the ports write: float
// CVars must stay floats and checkbox/combo CVars must stay integers, or the game's
// strict typed getters silently fall back to defaults. Changes apply on the next boot
// of that game; the UI refuses to edit a game that is currently running (it rewrites
// its config continuously and would clobber the edits).

// Resolves the config file for a game from the current stage's exe path.
fs::path GameConfigPath(GuiApp& app, bool isOot) {
    fs::path exe;
    if (app.stage == Stage::Runtime) {
        exe = isOot ? app.session.ootExe : app.session.mmExe;
    } else {
        exe = fs::path(isOot ? app.form.ootExe : app.form.mmExe);
    }
    if (exe.empty()) {
        return {};
    }
    return exe.parent_path() / (isOot ? "shipofharkinian.json" : "2ship2harkinian.json");
}

void OpenGameConfigEditor(GuiApp& app, bool isOot) {
    GuiApp::ConfigEditor& editor = app.cfgEditor;
    editor.isOot = isOot;
    editor.dirty = false;
    editor.error.clear();
    const fs::path path = GameConfigPath(app, isOot);
    if (path.empty()) {
        editor.error = "Set the game's executable path first.";
    } else if (!editor.config.Load(path)) {
        editor.error = "The existing config file could not be parsed; editing is disabled to avoid data loss.";
    }
    editor.open = true;
    editor.openedThisFrame = true;
    LayoutGameChild(app); // the sidebar strip widens while the editor is open
}

void CloseGameConfigEditor(GuiApp& app) {
    app.cfgEditor.open = false;
    LayoutGameChild(app); // restore the game to its normal size
}

// --- typed widget helpers (each reads the file value, writes back on change) ------

void CfgCheckbox(GuiApp& app, const char* key, const char* label, int defaultValue, const char* tooltip = nullptr) {
    ootmm::launcher::GameConfig& cfg = app.cfgEditor.config;
    bool value = cfg.GetCVarInt(key, defaultValue) != 0;
    if (ImGui::Checkbox(label, &value)) {
        cfg.SetCVarInt(key, value ? 1 : 0);
        app.cfgEditor.dirty = true;
    }
    if (tooltip != nullptr && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }
}

void CfgWindowBool(GuiApp& app, const char* path, const char* label, bool defaultValue, const char* tooltip = nullptr) {
    ootmm::launcher::GameConfig& cfg = app.cfgEditor.config;
    bool value = cfg.GetBoolAt(path, defaultValue);
    if (ImGui::Checkbox(label, &value)) {
        cfg.SetBoolAt(path, value); // Window.* flags are real JSON booleans
        app.cfgEditor.dirty = true;
    }
    if (tooltip != nullptr && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }
}

void CfgSliderInt(GuiApp& app, const char* key, const char* label, int minValue, int maxValue, int defaultValue,
                  const char* format = "%d") {
    ootmm::launcher::GameConfig& cfg = app.cfgEditor.config;
    int value = cfg.GetCVarInt(key, defaultValue);
    ImGui::SetNextItemWidth(-140.0f);
    if (ImGui::SliderInt(label, &value, minValue, maxValue, format)) {
        cfg.SetCVarInt(key, value);
        app.cfgEditor.dirty = true;
    }
}

// Float CVar shown as a percentage slider; stored strictly as a JSON float.
void CfgSliderFloatPct(GuiApp& app, const char* key, const char* label, float minValue, float maxValue,
                       float defaultValue) {
    ootmm::launcher::GameConfig& cfg = app.cfgEditor.config;
    float value = static_cast<float>(cfg.GetCVarFloat(key, defaultValue)) * 100.0f;
    ImGui::SetNextItemWidth(-140.0f);
    if (ImGui::SliderFloat(label, &value, minValue * 100.0f, maxValue * 100.0f, "%.0f%%")) {
        cfg.SetCVarFloat(key, static_cast<double>(value) / 100.0);
        app.cfgEditor.dirty = true;
    }
}

void CfgCombo(GuiApp& app, const char* key, const char* label, const char* items, int defaultValue) {
    ootmm::launcher::GameConfig& cfg = app.cfgEditor.config;
    int value = cfg.GetCVarInt(key, defaultValue);
    ImGui::SetNextItemWidth(-140.0f);
    if (ImGui::Combo(label, &value, items)) {
        cfg.SetCVarInt(key, value);
        app.cfgEditor.dirty = true;
    }
}

void DrawGameConfigModal(GuiApp& app) {
    GuiApp::ConfigEditor& editor = app.cfgEditor;
    if (!editor.open) {
        return;
    }
    if (editor.openedThisFrame) {
        ImGui::OpenPopup("Game Configuration");
        editor.openedThisFrame = false;
    }

    const Palette pal = CurrentPalette(app);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (app.stage == Stage::Runtime) {
        // The game child window covers the viewport center, so the modal must live in
        // the (temporarily widened) sidebar strip on the right, where the launcher's
        // own rendering is actually visible.
        const float panelWidth = static_cast<float>(SidebarWidth(app));
        ImGui::SetNextWindowPos(
            ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - panelWidth + 10.0f, viewport->WorkPos.y + 24.0f),
            ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(panelWidth - 20.0f, 0), ImGuiCond_Always);
    } else {
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                       viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                                ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(600.0f * app.cfg.uiScale, 0), ImGuiCond_Appearing);
    }
    bool keepOpen = true;
    if (!ImGui::BeginPopupModal("Game Configuration", &keepOpen, ImGuiWindowFlags_NoResize)) {
        if (!keepOpen) {
            CloseGameConfigEditor(app);
        }
        return;
    }

    const bool isOot = editor.isOot;
    DrawGameIcon(app, isOot ? "OcarinaOfTime" : "FierceDeityMask", 34.0f);
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextColored(pal.mainV, "%s", GameTitle(isOot ? ootmm::Game::Oot : ootmm::Game::Mm));
    ImGui::TextDisabled("%s", editor.config.Path().string().c_str());
    ImGui::EndGroup();
    ImGui::Spacing();

    if (!editor.error.empty()) {
        ImGui::TextColored(ImVec4(0.92f, 0.40f, 0.34f, 1.0f), "%s", editor.error.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(120, 0))) {
            CloseGameConfigEditor(app);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }

    if (ImGui::BeginTabBar("gameCfgTabs")) {
        if (ImGui::BeginTabItem("Graphics")) {
            ImGui::Spacing();
            CfgWindowBool(app, "Window.Fullscreen.Enabled", "Fullscreen", false,
                          "Start in exclusive fullscreen. F11 also toggles it in game.");
            CfgCheckbox(app, "gSdlWindowedFullscreen", "Windowed fullscreen", 0,
                        "Borderless fullscreen at the desktop resolution.");
            CfgCheckbox(app, "gVsyncEnabled", "VSync", 1, "Sync presentation to the display's refresh rate.");
            ImGui::Spacing();
            {
                // gInternalResolution is a FLOAT CVar (0.5-2.0 = 50%-200%).
                ootmm::launcher::GameConfig& cfg = editor.config;
                float value = static_cast<float>(cfg.GetCVarFloat("gInternalResolution", 1.0)) * 100.0f;
                ImGui::SetNextItemWidth(-140.0f);
                if (ImGui::SliderFloat("Internal resolution", &value, 50.0f, 200.0f, "%.0f%%")) {
                    cfg.SetCVarFloat("gInternalResolution", static_cast<double>(value) / 100.0);
                    editor.dirty = true;
                }
            }
            CfgSliderInt(app, "gMSAAValue", "Anti-aliasing (MSAA)", 1, 8, 1, "%dx");
            CfgSliderInt(app, isOot ? "gSettings.InterpolationFPS" : "gInterpolationFPS", "Frame rate", 20, 360, 20,
                         "%d FPS");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("20 = the original N64 frame pacing.");
            }
            CfgCheckbox(app, isOot ? "gSettings.MatchRefreshRate" : "gMatchRefreshRate", "Match display refresh rate",
                        0, "Sets the frame rate to your monitor's refresh rate.");
            CfgCombo(app, "gTextureFilter", "Texture filter", "Three-point (N64)\0Linear\0None (sharp)\0", 0);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Audio")) {
            ImGui::Spacing();
            if (isOot) {
                // SoH volumes are INTEGER 0-100 CVars.
                CfgSliderInt(app, "gSettings.Volume.Master", "Master volume", 0, 100, 40, "%d%%");
                CfgSliderInt(app, "gSettings.Volume.MainMusic", "Music volume", 0, 100, 100, "%d%%");
                CfgSliderInt(app, "gSettings.Volume.SubMusic", "Sub music volume", 0, 100, 100, "%d%%");
                CfgSliderInt(app, "gSettings.Volume.Fanfare", "Fanfare volume", 0, 100, 100, "%d%%");
                CfgSliderInt(app, "gSettings.Volume.SFX", "Sound effects volume", 0, 100, 100, "%d%%");
            } else {
                // 2S2H volumes are FLOAT 0.0-1.0 CVars.
                CfgSliderFloatPct(app, "gSettings.Audio.MasterVolume", "Master volume", 0.0f, 1.0f, 1.0f);
                CfgSliderFloatPct(app, "gSettings.Audio.MainMusicVolume", "Music volume", 0.0f, 1.0f, 1.0f);
                CfgSliderFloatPct(app, "gSettings.Audio.SubMusicVolume", "Sub music volume", 0.0f, 1.0f, 1.0f);
                CfgSliderFloatPct(app, "gSettings.Audio.SoundEffectsVolume", "Sound effects volume", 0.0f, 1.0f, 1.0f);
                CfgSliderFloatPct(app, "gSettings.Audio.FanfareVolume", "Fanfare volume", 0.0f, 1.0f, 1.0f);
                CfgSliderFloatPct(app, "gSettings.Audio.AmbienceVolume", "Ambience volume", 0.0f, 1.0f, 1.0f);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Input")) {
            ImGui::Spacing();
            CfgCheckbox(app, "gControlNav", "Navigate menus with the controller", 0);
            CfgCheckbox(app, "gSettings.CursorVisibility", "Cursor always visible", 0);
            if (isOot) {
                CfgCheckbox(app, "gAllowBackgroundInputs", "Allow background inputs", 1,
                            "Keep responding to the controller when the window is not focused.");
            }
            ImGui::Spacing();
            ImGui::PushTextWrapPos();
            ImGui::TextDisabled("Controller and keyboard BINDINGS are edited in the game itself: press Esc in game, "
                                "then Settings > Controls. Rumble and gyro live in that binding editor too.");
            ImGui::PopTextWrapPos();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("Changes apply the next time %s starts.", GameTitle(isOot ? ootmm::Game::Oot : ootmm::Game::Mm));
    ImGui::Spacing();
    ImGui::BeginDisabled(!editor.dirty);
    if (ImGui::Button("Save", ImVec2(140, 0))) {
        if (editor.config.Save()) {
            CloseGameConfigEditor(app);
            ImGui::CloseCurrentPopup();
        } else {
            editor.error = "Failed to write the config file.";
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        CloseGameConfigEditor(app);
        ImGui::CloseCurrentPopup();
    }

    if (!keepOpen) {
        CloseGameConfigEditor(app);
    }
    ImGui::EndPopup();
}

// --- Runtime manager sidebar --------------------------------------------------

void DrawTrackerTab(GuiApp& app) {
    Tracker& tracker = app.tracker;
    const Palette pal = CurrentPalette(app);
    const float fraction =
        tracker.TotalCount() > 0 ? static_cast<float>(tracker.CollectedCount()) / tracker.TotalCount() : 0.0f;
    char overlay[64];
    std::snprintf(overlay, sizeof(overlay), "%d / %d checks", tracker.CollectedCount(), tracker.TotalCount());
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, pal.mainV);
    ImGui::ProgressBar(fraction, ImVec2(-1, 0), overlay);
    ImGui::PopStyleColor();
    if (app.solver.Loaded()) {
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.50f, 1.0f), "%d check%s in logic right now", app.inLogicCount,
                           app.inLogicCount == 1 ? "" : "s");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Reachable with your current items, per the seed's logic.");
        }
    } else {
        ImGui::TextDisabled("No logic data in this seed - regenerate it to see check availability.");
    }
    ImGui::Spacing();

    static char filter[128] = {};
    static int gameFilter = 0; // 0 both, 1 oot, 2 mm
    static bool hideCollected = true;
    static bool onlyInLogic = false;
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "Filter checks...", filter, sizeof(filter));
    ImGui::SetNextItemWidth(110.0f * app.cfg.uiScale);
    ImGui::Combo("##game", &gameFilter, "Both\0OoT\0MM\0");
    ImGui::SameLine();
    ImGui::Checkbox("Hide collected", &hideCollected);
    if (app.solver.Loaded()) {
        ImGui::SameLine();
        ImGui::Checkbox("In logic only", &onlyInLogic);
    }
    ImGui::Spacing();

    // Case-insensitive substring filter.
    std::string needle = filter;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    static std::vector<int> visible;
    visible.clear();
    const std::vector<ootmm::launcher::TrackerCheck>& checks = tracker.Checks();
    for (int i = 0; i < static_cast<int>(checks.size()); ++i) {
        const ootmm::launcher::TrackerCheck& check = checks[i];
        if (hideCollected && check.collected) {
            continue;
        }
        if (onlyInLogic && app.solver.Loaded() && (!check.inLogic || check.collected)) {
            continue;
        }
        if (gameFilter == 1 && check.game != ootmm::Game::Oot) {
            continue;
        }
        if (gameFilter == 2 && check.game != ootmm::Game::Mm) {
            continue;
        }
        if (!needle.empty()) {
            std::string haystack = check.name;
            std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (haystack.find(needle) == std::string::npos) {
                continue;
            }
        }
        visible.push_back(i);
    }

    ImGui::BeginChild("trackerList", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visible.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const ootmm::launcher::TrackerCheck& check = checks[visible[row]];
            const bool isOot = check.game == ootmm::Game::Oot;
            const ImVec4 tagColor = isOot ? ImVec4(0.79f, 0.64f, 0.24f, 1.0f) : ImVec4(0.58f, 0.44f, 0.86f, 1.0f);

            // Availability dot: green = reachable in logic right now, gray = not yet.
            if (app.solver.Loaded() && !check.collected) {
                ImDrawList* draw = ImGui::GetWindowDrawList();
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                const ImU32 dotColor =
                    check.inLogic ? IM_COL32(115, 210, 120, 255) : IM_COL32(110, 104, 92, 160);
                draw->AddCircleFilled(ImVec2(pos.x + 5.0f, pos.y + ImGui::GetTextLineHeight() * 0.55f), 3.5f,
                                      dotColor);
                ImGui::Dummy(ImVec2(13.0f, 0.0f));
                ImGui::SameLine(0.0f, 0.0f);
            }

            ImGui::TextColored(tagColor, isOot ? "[OoT]" : "[MM]");
            ImGui::SameLine();
            if (check.collected) {
                // Once collected the placement is public knowledge, so show the item inline.
                ImGui::TextDisabled("%s -> %s%s", check.name.c_str(), check.itemName.c_str(),
                                    check.ownerPlayer != app.session.seed.playerId ? " (another player's item)" : "");
            } else {
                ImGui::TextUnformatted(check.name.c_str());
                if (ImGui::IsItemHovered() && !check.type.empty()) {
                    // Deliberately does NOT reveal the placed item - that would spoil the seed.
                    ImGui::SetTooltip("Type: %s%s", check.type.c_str(),
                                      app.solver.Loaded() ? (check.inLogic ? "\nIn logic with your current items."
                                                                           : "\nNot yet reachable in logic.")
                                                          : "");
                }
            }
        }
    }
    ImGui::EndChild();
}

void DrawLogTab(GuiApp& app) {
    const std::vector<ootmm::launcher::CheckLogEntry>& log = app.tracker.Log();
    if (log.empty()) {
        ImGui::TextDisabled("No checks collected yet this session.");
        return;
    }
    ImGui::TextDisabled("%d check%s collected this session (newest first).", static_cast<int>(log.size()),
                        log.size() == 1 ? "" : "s");
    ImGui::Spacing();
    ImGui::BeginChild("logList", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(log.size()));
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const ootmm::launcher::CheckLogEntry& entry = log[log.size() - 1 - static_cast<size_t>(row)];
            const bool isOot = entry.game == ootmm::Game::Oot;
            ImGui::TextDisabled("%s", entry.time.c_str());
            ImGui::SameLine();
            ImGui::TextColored(isOot ? ImVec4(0.79f, 0.64f, 0.24f, 1.0f) : ImVec4(0.58f, 0.44f, 0.86f, 1.0f),
                               isOot ? "[OoT]" : "[MM]");
            ImGui::SameLine();
            if (entry.sentToRemote) {
                ImGui::Text("%s -> %s (sent to player %u)", entry.checkName.c_str(), entry.itemName.c_str(),
                            entry.targetPlayer);
            } else {
                ImGui::Text("%s -> %s", entry.checkName.c_str(), entry.itemName.c_str());
            }
        }
    }
    ImGui::EndChild();
}

// Hover tooltip clamped to a sane wrap width. Raw ImGui::SetTooltip sizes the popup to the
// longest line, and next to the screen-edge-hugging sidebar a long line spills off the display.
void WrappedTooltip(GuiApp& app, const char* fmt, ...) {
    if (!ImGui::IsItemHovered()) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(300.0f * app.cfg.uiScale);
    ImGui::TextV(fmt, args);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
    va_end(args);
}

void DrawSessionTab(GuiApp& app) {
    const Session& s = app.session;
    RuntimeState& run = app.run;
    const bool running = run.process && run.process->Running();

    ImGui::Text("Active game: %s", GameTitle(run.activeGame));
    ImGui::SameLine();
    if (running) {
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.50f, 1.0f), "(running)");
    } else {
        ImGui::TextColored(ImVec4(0.92f, 0.40f, 0.34f, 1.0f), "(not running)");
    }
    ImGui::Text("Seed: %s", s.seed.seedId.c_str());
    if (s.seed.playerCount > 1) {
        ImGui::Text("Multiworld: player %u of %u -", s.seed.playerId, s.seed.playerCount);
        ImGui::SameLine();
        if (s.multiworld && s.multiworld->Connected()) {
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.50f, 1.0f), "connected");
        } else {
            ImGui::TextColored(ImVec4(0.92f, 0.40f, 0.34f, 1.0f), "disconnected");
        }
        // Live co-op roster: who is in the session (playerId + client name), where, and the
        // state of their synced model set. Identity problems show in red.
        if (!gPresence.identityWarning.empty()) {
            ImGui::PushTextWrapPos();
            ImGui::TextColored(ImVec4(0.92f, 0.40f, 0.34f, 1.0f), "%s", gPresence.identityWarning.c_str());
            ImGui::PopTextWrapPos();
        }
        for (const auto& [id, remote] : gPresence.roster) {
            const char* remoteGame = remote.pose.game == ootmm::Game::Mm ? "MM" : "OoT";
            ImGui::Text("Player %u", id);
            if (!remote.pose.clientName.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", remote.pose.clientName.c_str());
            }
            ImGui::SameLine();
            ImGui::TextDisabled("- %s scene %d", remoteGame, remote.pose.sceneId);
            const auto note = gPresence.remoteModelNote.find(id);
            if (note != gPresence.remoteModelNote.end() && !note->second.empty()) {
                ImGui::PushTextWrapPos();
                ImGui::TextDisabled("  model: %s", note->second.c_str());
                ImGui::PopTextWrapPos();
            }
        }
        if (gPresence.roster.empty()) {
            ImGui::TextDisabled("No other players seen yet.");
        }
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const float buttonHeight = 38.0f * app.cfg.uiScale;
    if (IconTextButton(app, run.activeGame == ootmm::Game::Oot ? "OcarinaOfTime" : "FierceDeityMask",
                       "Restart active game", buttonHeight)) {
        // Relaunch from the last save (title screen boot, no forced entrance).
        if (run.process && run.process->Running()) {
            run.process->Terminate();
        }
        run.bootEntrance.reset();
        run.bootOotAge.reset();
        StartActiveGame(app);
    }
    WrappedTooltip(app, "Closes and relaunches %s from its last save.", GameTitle(run.activeGame));

    // Game speed ("timer"): scales BOTH games' logic rate live — the games read the value from
    // the roster meta every frame, so it applies without a reboot and is independent of the
    // render frame rate. In multiworld the SERVER decides whether the slider exists and what
    // range it may take (timer-enabled / timer-min / timer-max).
    if (gPresence.timerEnabled) {
        ImGui::Spacing();
        // Reset lives right-aligned on the LABEL row: appended after a full-width slider it
        // started past the sidebar's (= the screen's) right edge and was never visible.
        ImGui::TextUnformatted("Game speed");
        if (gPresence.speedPercent != 100) {
            const float resetW =
                ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - resetW);
            if (ImGui::SmallButton("Reset##ootmmGameSpeedReset")) {
                gPresence.speedPercent = 100;
                gPresence.rosterDirty = true;
            }
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        int percent = gPresence.speedPercent;
        const int lo = (std::max)(0, (std::min)(gPresence.timerMin, gPresence.timerMax));
        const int hi = (std::max)(lo, gPresence.timerMax);
        if (ImGui::SliderInt("##ootmmGameSpeed", &percent, lo, hi, "%d%%")) {
            gPresence.speedPercent = (std::clamp)(percent, lo, hi);
            gPresence.rosterDirty = true;
        }
        WrappedTooltip(app, "Speeds up or slows down the game itself (0%% freezes it). "
                            "100%% is normal speed. On a server, the host sets the allowed range.");
        bool matchFps = app.cfg.speedMatchFramerate;
        if (ImGui::Checkbox("Sync updates to render framerate", &matchFps)) {
            app.cfg.speedMatchFramerate = matchFps;
            gPresence.rosterDirty = true;
            ootmm::launcher::SaveLauncherConfig(app.configPath, app.cfg);
        }
        WrappedTooltip(app, "Paces the scaled game updates against the render framerate instead of "
                            "whole-frame steps, so speed-ups and slowdowns stay smooth.");
    }

    ImGui::Spacing();
    // Multiworld: never offer a local reset — the other players' worlds (and the relay's view
    // of this one) keep the progress, so a one-sided wipe just desyncs the session.
    if (app.session.seed.playerCount <= 1 &&
        IconTextButton(app, "BossKey", "Reset seed progress...", buttonHeight, IM_COL32(120, 42, 34, 210))) {
        ImGui::OpenPopup("Reset seed progress?");
    }

    if (ImGui::BeginPopupModal("Reset seed progress?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("This permanently deletes ALL progress for this seed:");
        ImGui::BulletText("both games' save files for the seed");
        ImGui::BulletText("the shared item/check ledger");
        ImGui::BulletText("the check log");
        ImGui::TextColored(ImVec4(0.92f, 0.40f, 0.34f, 1.0f), "This cannot be undone.");
        ImGui::Spacing();
        if (ImGui::Button("Reset everything", ImVec2(160, 0))) {
            if (run.process && run.process->Running()) {
                run.process->Terminate();
            }
            ResetSeedProgress(app.session);
            app.tracker.LoadSeed(app.session.seed);
            run.activeGame = app.session.seed.ResolveStartingGame();
            run.bootEntrance.reset();
            run.bootOotAge.reset();
            StartActiveGame(app);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::Spacing();
    if (IconTextButton(app, "Compass", "Return to launcher", buttonHeight)) {
        EndSession(app, "Session closed.");
        return;
    }
    WrappedTooltip(app, "Saves are kept - relaunching this seed resumes where you left off.");
    ImGui::Spacing();
    if (IconTextButton(app, "MoonsTear", "Quit", buttonHeight)) {
        PostMessageW(app.host, WM_CLOSE, 0, 0);
    }
}

void DrawOptionsTab(GuiApp& app) {
    bool cfgChanged = false;

    // --- Co-op ------------------------------------------------------------------
    if (app.session.seed.playerCount > 1) {
        ImGui::TextUnformatted("Co-op");
        ImGui::Separator();
        if (ImGui::Checkbox("Show player nametags", &app.cfg.showPlayerNametags)) {
            cfgChanged = true;
            gPresence.rosterDirty = true; // applies live via the next roster write
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Draw each remote player's name above their in-game character.");
        }
        if (gPresence.pvpActive) {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "PvP is enabled on this server.");
        }
        ImGui::Spacing();
    }

    // --- Per-game configuration (graphics / audio / input) ---------------------
    ImGui::TextUnformatted("Game settings");
    ImGui::Separator();
    {
        const bool activeIsOot = app.run.activeGame == ootmm::Game::Oot;
        const bool running = app.run.process && app.run.process->Running() && app.run.gameChild != nullptr;

        // Active game: its own in-game menu (applies live, includes input bindings).
        char label[96];
        std::snprintf(label, sizeof(label), "Configure %s (in-game)...", GameTitle(app.run.activeGame));
        ImGui::BeginDisabled(!running);
        if (IconTextButton(app, activeIsOot ? "OcarinaOfTime" : "FierceDeityMask", label,
                           38.0f * app.cfg.uiScale)) {
            OpenInGameMenu(app);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Opens %s's own settings menu: graphics, video, audio,\n"
                              "input bindings and more. You can also just press Esc in the game.",
                              GameTitle(app.run.activeGame));
        }

        // The other game is not running, so its config file is safe to edit here.
        const bool otherIsOot = !activeIsOot;
        ImGui::Spacing();
        std::snprintf(label, sizeof(label), "Configure %s...",
                      GameTitle(otherIsOot ? ootmm::Game::Oot : ootmm::Game::Mm));
        if (IconTextButton(app, otherIsOot ? "OcarinaOfTime" : "FierceDeityMask", label,
                           38.0f * app.cfg.uiScale)) {
            OpenGameConfigEditor(app, otherIsOot);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Graphics, audio and input options; applied when the game next boots.");
        }
        ImGui::PushTextWrapPos();
        ImGui::TextDisabled("Each game keeps its own graphics, audio and input configuration. The running "
                            "game is configured through its in-game menu (Esc); the other one can be "
                            "edited here.");
        ImGui::PopTextWrapPos();
    }
    ImGui::Spacing();

    ImGui::TextUnformatted("Appearance");
    ImGui::Separator();
    ImGui::SetNextItemWidth(-1);
    cfgChanged |= ImGui::Combo("##theme", &app.cfg.theme, "Theme: follow active game\0Theme: OoT gold\0Theme: MM violet\0");
    // Edit a staging value and commit on release: applying the scale rebuilds the font
    // atlas, which would otherwise happen on every pixel of the drag.
    static float scaleEdit = 0.0f;
    if (!ImGui::IsAnyItemActive() || scaleEdit == 0.0f) {
        scaleEdit = app.cfg.uiScale;
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##scale", &scaleEdit, 0.75f, 1.75f, "UI scale %.2fx");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        app.cfg.uiScale = scaleEdit;
        cfgChanged = true;
    }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderInt("##sbwidth", &app.cfg.sidebarWidth, 300, 640, "Sidebar width %d px")) {
        cfgChanged = true;
        LayoutGameChild(app);
    }
    ImGui::Spacing();

    ImGui::TextUnformatted("Advanced");
    ImGui::Separator();
    if (ImGui::Checkbox("Enable debug && cheat menus", &app.cfg.allowDebugMenus)) {
        cfgChanged = true;
        app.session.allowDebugMenus = app.cfg.allowDebugMenus;
        app.cheatGateDirty = true;
    }
    ImGui::PushTextWrapPos();
    ImGui::TextDisabled("Both games hide their debug, cheats and enhancements menus by default so seeds "
                        "stay fair. Enabling this unlocks them - the change applies the next time a game "
                        "boots.");
    ImGui::PopTextWrapPos();
    if (app.cheatGateDirty) {
        ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.35f, 1.0f), "Pending - restart the game to apply.");
        if (ImGui::Button("Restart game now")) {
            if (app.run.process && app.run.process->Running()) {
                app.run.process->Terminate();
            }
            app.run.bootEntrance.reset();
            app.run.bootOotAge.reset();
            StartActiveGame(app);
        }
    }

    if (cfgChanged) {
        ootmm::launcher::SaveLauncherConfig(app.configPath, app.cfg);
    }
}

void DrawRuntimeSidebar(GuiApp& app) {
    const Palette pal = CurrentPalette(app);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float panelWidth = static_cast<float>(SidebarWidth(app));
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - panelWidth, viewport->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(panelWidth, viewport->WorkSize.y));
    ImGui::Begin("##sidebar", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

    if (!app.cfg.showSidebar) {
        // Collapsed strip: just the expand handle.
        if (ImGui::Button("<", ImVec2(-1, 40))) {
            app.cfg.showSidebar = true;
            ootmm::launcher::SaveLauncherConfig(app.configPath, app.cfg);
            LayoutGameChild(app);
        }
        ImGui::End();
        return;
    }

    // Header: mini Triforce + wordmark + collapse chevron.
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 1.7f);
        ootmm::launcher::DrawTriforce(draw, ImVec2(cursor.x + 16.0f, cursor.y + 12.0f), 11.0f, pal.main, pulse);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 38.0f);
        ImGui::PushFont(HeaderFont());
        ImGui::TextColored(pal.mainV, "OoTMM");
        ImGui::PopFont();
        ImGui::SameLine(ImGui::GetWindowWidth() - 40.0f);
        if (ImGui::Button(">", ImVec2(24, 0))) {
            app.cfg.showSidebar = false;
            ootmm::launcher::SaveLauncherConfig(app.configPath, app.cfg);
            LayoutGameChild(app);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Collapse the sidebar");
        }
    }

    // Active-game banner: emblem, title, live status dot.
    {
        const bool isOot = app.run.activeGame == ootmm::Game::Oot;
        const bool running = app.run.process && app.run.process->Running();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec2 size(ImGui::GetContentRegionAvail().x, 52.0f);
        draw->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), WithA(pal.dim, 0.45f), 8.0f);
        draw->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), WithA(pal.main, 0.6f), 8.0f, 0, 1.5f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10.0f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f);
        DrawGameIcon(app, isOot ? "OcarinaOfTime" : "FierceDeityMask", 36.0f);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextColored(pal.mainV, "%s", GameTitle(app.run.activeGame));
        if (running) {
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.50f, 1.0f), "running");
        } else {
            ImGui::TextColored(ImVec4(0.92f, 0.40f, 0.34f, 1.0f), "not running");
        }
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y + 8.0f));
    }

    // Icon tab rail: dungeon map = tracker, notebook = log, sword = session, lens = options.
    {
        const float railWidth = ImGui::GetContentRegionAvail().x;
        const float tabWidth = (railWidth - 3.0f * ImGui::GetStyle().ItemSpacing.x) / 4.0f;
        struct TabDef {
            const char* icon;
            const char* tooltip;
        };
        const TabDef tabs[4] = { { "DungeonMap", "Tracker - open checks" },
                                 { "BombersNotebook", "Check log" },
                                 { "KokiriSword", "Session - restart / reset" },
                                 { "LensOfTruth", "Options & appearance" } };
        for (int i = 0; i < 4; ++i) {
            if (i > 0) {
                ImGui::SameLine();
            }
            if (TabIconButton(app, i, tabs[i].icon, tabs[i].tooltip, tabWidth)) {
                app.sidebarTab = i;
            }
        }
    }
    ImGui::Spacing();

    switch (app.sidebarTab) {
    case 0:
        DrawTrackerTab(app);
        break;
    case 1:
        DrawLogTab(app);
        break;
    case 2:
        DrawSessionTab(app);
        break;
    default:
        DrawOptionsTab(app);
        break;
    }
    DrawGameConfigModal(app);
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Window + main loop
// ---------------------------------------------------------------------------

LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* app = reinterpret_cast<GuiApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (app != nullptr && app->ui.HandleMessage(hwnd, msg, wparam, lparam)) {
        return 1;
    }
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 0;
    }
    case WM_SIZE:
        if (app != nullptr && wparam != SIZE_MINIMIZED) {
            app->ui.OnResize(LOWORD(lparam), HIWORD(lparam));
            LayoutGameChild(*app);
        }
        return 0;
    case WM_LBUTTONDOWN:
        // A click that reaches the host landed on the sidebar strip: take keyboard focus
        // back from the embedded game so ImGui text fields receive typing.
        SetFocus(hwnd);
        return 0;
    case WM_SETFOCUS:
        // Focus arriving at the host (alt-tab back, game relaunch) belongs to the game
        // unless the cursor is over the sidebar (the user is heading for the panel).
        if (app != nullptr && app->stage == Stage::Runtime && !CursorOverSidebar(*app)) {
            FocusGameChild(*app);
        }
        return 0;
    case WM_CLOSE:
        if (app != nullptr && app->run.process && app->run.process->Running()) {
            app->run.process->Terminate();
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

Accent CurrentAccent(const GuiApp& app) {
    if (app.cfg.theme == 1) {
        return Accent::OotGold;
    }
    if (app.cfg.theme == 2) {
        return Accent::MmViolet;
    }
    if (app.stage == Stage::Runtime) {
        return app.run.activeGame == ootmm::Game::Oot ? Accent::OotGold : Accent::MmViolet;
    }
    return Accent::OotGold;
}

int RunGui(GuiApp& app) {
    // Per-monitor-v2 DPI so the embedded child viewport is crisp and not clipped/scaled.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = HostWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW)); // IDC_ARROW is an ordinal
    wc.hbrBackground = nullptr; // DX11 paints the client area; no GDI background flicker
    wc.lpszClassName = kHostClassName;
    if (RegisterClassExW(&wc) == 0) {
        std::cerr << "[launcher] RegisterClassExW failed (" << GetLastError() << ")" << std::endl;
        return 1;
    }

    app.host = CreateWindowExW(0, kHostClassName, L"OoTMM PC", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                               CW_USEDEFAULT, CW_USEDEFAULT, 1680, 940, nullptr, nullptr, instance, &app);
    if (app.host == nullptr) {
        std::cerr << "[launcher] CreateWindowExW failed (" << GetLastError() << ")" << std::endl;
        return 1;
    }
    // Dark title bar to match the theme (a no-op on Windows builds without the attribute).
    const BOOL darkTitle = TRUE;
    DwmSetWindowAttribute(app.host, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &darkTitle, sizeof(darkTitle));
    if (!app.ui.Init(app.host)) {
        std::cerr << "[launcher] Direct3D 11 initialization failed" << std::endl;
        DestroyWindow(app.host);
        return 1;
    }

    ShowWindow(app.host, SW_SHOW);
    UpdateWindow(app.host);

    if (app.autoLaunch) {
        LaunchFromForm(app); // validation failures fall back to the config screen
    }

    auto nextTick = std::chrono::steady_clock::now();
    while (!app.quit) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                app.quit = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (app.quit) {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (app.stage == Stage::Runtime && now >= nextTick) {
            nextTick = now + std::chrono::milliseconds(100);
            TickRuntime(app);
        }
        // Setup-only game runs (ROM extraction) are driven from the config screen.
        if (app.stage == Stage::Config) {
            TickSetup(app);
        }
        // Presence runs at UI cadence (~60Hz): pump the socket + pose/roster/model channels
        // every frame so remote players move smoothly instead of at the 100ms item tick.
        if (app.stage == Stage::Runtime && app.run.process) {
            PumpMultiworld(app);
            PumpPresence(app);
        }

        // Clicking into the game area hands keyboard focus to the embedded game (clicks
        // over the child window never reach the host's WndProc, so poll the transition).
        if (app.stage == Stage::Runtime) {
            static bool wasDown = false;
            const bool isDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            if (isDown && !wasDown && GetForegroundWindow() == app.host && CursorOverGameArea(app)) {
                FocusGameChild(app);
            }
            wasDown = isDown;
        }

        app.ui.ApplyTheme(CurrentAccent(app), app.cfg.uiScale);
        app.ui.BeginFrame();
        if (app.stage == Stage::Config) {
            DrawConfigScreen(app);
        } else {
            DrawRuntimeSidebar(app);
        }
        app.ui.EndFrame(); // vsync present throttles the loop to the refresh rate
    }

    if (app.run.process && app.run.process->Running()) {
        app.run.process->Terminate();
    }
    app.ui.Shutdown();
    return 0;
}

} // namespace
#endif // _WIN32

namespace {

// Console fallback (non-Windows): the original save-and-relaunch loop with no embedding.
#ifndef _WIN32
std::optional<ootmm::CrossGameTransition> RunGameConsole(Session& s, const fs::path& exe,
                                                         const ootmm::BootConfig& boot,
                                                         const fs::path& bootConfigPath, const fs::path& activeInbox,
                                                         const fs::path& otherInbox) {
    WriteFile(bootConfigPath, ootmm::SerializeBootConfig(boot));
    TruncateFile(boot.outboxPath);

    std::cout << "[launcher] booting " << GameName(boot.bootGame) << std::endl;
    ChildProcess process(exe, bootConfigPath);
    if (!process.Valid()) {
        std::cerr << "[launcher] failed to launch " << exe.string() << std::endl;
        return std::nullopt;
    }

    std::optional<ootmm::CrossGameTransition> pendingTransition;
    uintmax_t outboxOffset = 0;
    const auto drainOutbox = [&]() {
        ootmm::FilePacketReadResult inbound = ootmm::ReadPacketLines(boot.outboxPath, outboxOffset);
        outboxOffset = inbound.nextOffset;
        for (const std::string& packet : inbound.packets) {
            if (auto transition = ootmm::anchor::ParseCrossGameTransitionPacket(packet); transition.has_value()) {
                pendingTransition = transition;
            } else {
                bool remoteOnly = false;
                if (s.multiworld) {
                    s.multiworld->QueueLine(packet);
                    if (const auto event = ootmm::anchor::ParseGiveItemPacket(packet);
                        event.has_value() && event->targetPlayer != s.seed.playerId) {
                        remoteOnly = true;
                    }
                }
                if (!remoteOnly) {
                    ootmm::AppendPacketLines(otherInbox, { packet });
                }
            }
        }
        if (s.multiworld) {
            std::vector<std::string> inboundWire;
            s.multiworld->Pump(inboundWire);
            std::vector<std::string> forActiveGame;
            for (const std::string& line : inboundWire) {
                if (const auto event = ootmm::anchor::ParseGiveItemPacket(line);
                    event.has_value() && event->targetPlayer == s.seed.playerId) {
                    forActiveGame.push_back(line);
                }
            }
            if (!forActiveGame.empty()) {
                std::cout << "[multiworld] received " << forActiveGame.size() << " item(s)" << std::endl;
                ootmm::AppendPacketLines(activeInbox, forActiveGame);
            }
        }
    };

    while (process.Running() && !pendingTransition.has_value()) {
        drainOutbox();
        if (!pendingTransition.has_value()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    drainOutbox();
    if (pendingTransition.has_value() && process.Running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(750));
        process.Terminate();
    }
    return pendingTransition;
}

int RunConsole(Session& s, ootmm::Game activeGame) {
    std::optional<uint32_t> bootEntrance;
    std::optional<uint32_t> bootOotAge;
    while (true) {
        const ootmm::BootConfig boot = MakeBootConfig(s, activeGame, bootEntrance, bootOotAge);
        const fs::path& exe = activeGame == ootmm::Game::Oot ? s.ootExe : s.mmExe;
        const fs::path& activeInbox = activeGame == ootmm::Game::Oot ? s.ootInbox : s.mmInbox;
        const fs::path& otherInbox = activeGame == ootmm::Game::Oot ? s.mmInbox : s.ootInbox;
        const auto transition = RunGameConsole(s, exe, boot, s.bootConfigPath, activeInbox, otherInbox);
        if (!transition.has_value()) {
            std::cout << "[launcher] " << GameName(activeGame) << " exited; ending session." << std::endl;
            break;
        }
        activeGame = transition->toGame;
        bootEntrance = transition->toNativeId;
        bootOotAge = transition->ootAge;
    }
    return 0;
}
#endif

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // The launcher builds as a GUI-subsystem executable; when started from a terminal
    // (CLI flags, --serve, --solve), reattach to it so logs and usage errors are
    // visible. Skip when the parent already supplied std handles (a redirect/pipe) —
    // reopening CONOUT$ would clobber the redirection.
    const HANDLE stdoutHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    const bool stdoutProvided = stdoutHandle != nullptr && stdoutHandle != INVALID_HANDLE_VALUE;
    if (!stdoutProvided && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
    }
#endif

    const auto args = ParseArgs(argc, argv);
    if (!args.has_value()) {
        PrintUsage();
        return 2;
    }

    // Relay-server mode: no seed / no games, just the multiworld room broker.
    if (args->servePort.has_value()) {
        return RunRelayServer(*args->servePort, args->servePvp);
    }

#ifdef _WIN32
    // Model-mods builder mode: the native replacement for tools/build_foreign_model_mods.py.
    if (args->buildMods) {
        if (args->ootExe.empty() || args->mmExe.empty()) {
            std::cerr << "--build-mods requires --oot <soh.exe> and --mm <2ship.exe>\n";
            return 2;
        }
        const ootmm::launcher::ForeignModsResult result = ootmm::launcher::BuildForeignModelMods(
            fs::absolute(args->ootExe).parent_path(), fs::absolute(args->mmExe).parent_path());
        for (const std::string& line : result.log) {
            std::cout << line << std::endl;
        }
        if (!result.ok) {
            std::cerr << "[build-mods] FAILED: " << result.error << std::endl;
            return 1;
        }
        std::cout << "[build-mods] ok (" << result.entries << " entries)" << std::endl;
        return 0;
    }
#endif

#ifdef _WIN32
    // Solver diagnostic: reachable locations with starting items only, as sorted JSON.
    if (!args->solveSeed.empty()) {
        ootmm::Seed seed;
        try {
            seed = ootmm::LoadSeedFromFile(args->solveSeed);
        } catch (const std::exception& error) {
            std::cerr << "[solve] failed to load seed: " << error.what() << std::endl;
            return 1;
        }
        ootmm::launcher::LogicSolver solver;
        if (!solver.LoadFromSeedFile(args->solveSeed)) {
            std::cerr << "[solve] seed has no logic data" << std::endl;
            return 1;
        }
        ootmm::launcher::LogicSolver::Input input;
        for (const ootmm::StartingItem& starting : seed.startingItems) {
            input.items[starting.item.id] += starting.count;
            input.licenses[starting.item.id] += starting.count;
        }
        if (!args->solveItems.empty()) {
            std::ifstream itemsIn(args->solveItems, std::ios::binary);
            std::ostringstream itemsBuf;
            itemsBuf << itemsIn.rdbuf();
            try {
                const ootmm::json::Value itemsRoot = ootmm::json::Parse(itemsBuf.str());
                for (const auto& [id, count] : itemsRoot.AsObject()) {
                    const int n = count.IsNumber() ? static_cast<int>(count.AsNumber()) : 0;
                    input.items[id] += n;
                    input.licenses[id] += n;
                }
            } catch (const std::exception& error) {
                std::cerr << "[solve] bad --solve-items file: " << error.what() << std::endl;
                return 1;
            }
        }
        std::unordered_set<std::string> reachedEvents;
        const std::unordered_set<std::string> reachable = solver.Solve(input, &reachedEvents);
        std::vector<std::string> sorted(reachable.begin(), reachable.end());
        std::sort(sorted.begin(), sorted.end());
        std::vector<std::string> sortedEvents(reachedEvents.begin(), reachedEvents.end());
        std::sort(sortedEvents.begin(), sortedEvents.end());
        std::cout << "{\"locations\":[";
        for (size_t i = 0; i < sorted.size(); ++i) {
            std::cout << (i == 0 ? "" : ",") << "\"" << sorted[i] << "\"";
        }
        std::cout << "],\"events\":[";
        for (size_t i = 0; i < sortedEvents.size(); ++i) {
            std::cout << (i == 0 ? "" : ",") << "\"" << sortedEvents[i] << "\"";
        }
        std::cout << "]}" << std::endl;
        std::cerr << "[solve] " << sorted.size() << " locations, " << sortedEvents.size() << " events" << std::endl;
        return 0;
    }
#endif

#ifdef _WIN32
    GuiApp app;
    wchar_t moduleName[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, moduleName, MAX_PATH);
    app.configPath = fs::path(moduleName).parent_path() / "launcher.config.json";
    app.cfg = ootmm::launcher::LoadLauncherConfig(app.configPath);

    // CLI arguments override the persisted config; full args auto-launch straight into
    // the runtime manager (the config screen is skipped unless validation fails).
    if (!args->seedPath.empty()) {
        app.cfg.seedPath = args->seedPath.string();
    }
    if (!args->ootExe.empty()) {
        app.cfg.ootExe = args->ootExe.string();
    }
    if (!args->mmExe.empty()) {
        app.cfg.mmExe = args->mmExe.string();
    }
    if (!args->multiworldServer.empty()) {
        app.cfg.multiworldAddress = args->multiworldServer;
        app.cfg.hostRelay = false;
    }
    if (args->multiworldName != "ootmm-pc") {
        app.cfg.playerName = args->multiworldName;
    }
    app.sessionDirOverride = args->sessionDir;
    app.startOverride = args->startOverride;
    app.selftestArmed = args->selftest;
    app.autoLaunch = args->CompleteGameArgs();

    SyncFormFromConfig(app);
    return RunGui(app);
#else
    if (!args->CompleteGameArgs()) {
        PrintUsage();
        return 2;
    }

    Session session;
    try {
        session.seed = ootmm::LoadSeedFromFile(args->seedPath);
    } catch (const std::exception& error) {
        std::cerr << "[launcher] failed to load seed: " << error.what() << std::endl;
        return 1;
    }
    session.seedPath = fs::absolute(args->seedPath);
    session.ootExe = fs::absolute(args->ootExe);
    session.mmExe = fs::absolute(args->mmExe);

    if (!args->multiworldServer.empty()) {
        std::string host;
        uint16_t port = 0;
        const size_t colon = args->multiworldServer.rfind(':');
        if (colon == std::string::npos) {
            std::cerr << "[launcher] --multiworld expects host:port" << std::endl;
            return 2;
        }
        host = args->multiworldServer.substr(0, colon);
        port = static_cast<uint16_t>(std::stoul(args->multiworldServer.substr(colon + 1)));
        // HARD gate (same as the GUI path): never boot a multiworld seed without the relay
        // answering first — a session without its relay strands items/presence/rules.
        std::string pingError;
        if (!PingRelayServer(host, port, 2000, pingError)) {
            std::cerr << "[launcher] multiworld server unreachable (" << pingError
                      << "); start the server, then launch again." << std::endl;
            return 1;
        }
        session.multiworld = std::make_unique<MultiworldClient>(
            host, port, ootmm::anchor::BuildHandshakePacket(session.seed, args->multiworldName));
        session.multiworld->QueueLine(ootmm::anchor::BuildCheckCatalogPacket(session.seed));
        std::cout << "[launcher] multiworld: player " << session.seed.playerId << " -> " << host << ":" << port
                  << std::endl;
    }

    if (const auto error = PrepareSession(session, args->sessionDir); error.has_value()) {
        std::cerr << "[launcher] " << *error << std::endl;
        return 1;
    }

    const ootmm::Game activeGame = args->startOverride.value_or(session.seed.ResolveStartingGame());
    std::cout << "[launcher] seed " << session.seed.seedId << " (player " << session.seed.playerId << "/"
              << session.seed.playerCount << "), starting in " << GameName(activeGame) << std::endl;
    return RunConsole(session, activeGame);
#endif
}
