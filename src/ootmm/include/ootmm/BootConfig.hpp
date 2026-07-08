#pragma once

#include "ootmm/Types.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace ootmm {

// Handoff descriptor the coordinator writes before launching a port; the port reads it on
// startup (path passed via the OOTMM_BOOT_CONFIG environment variable).
struct BootConfig {
    std::string seedPath;   // .ootmm.json seed (identical for both games)
    std::string statePath;  // shared Runtime progress snapshot (restore on boot, write on exit)
    std::string inboxPath;  // JSONL packets the coordinator delivers to this port
    std::string outboxPath; // JSONL packets this port emits to the coordinator

    // Co-op presence channel (ephemeral last-value files; empty = disabled). The port rewrites
    // presenceOutPath with its own pose each frame; the coordinator rewrites presenceInPath with
    // the latest roster of REMOTE player poses.
    std::string presenceOutPath;
    std::string presenceInPath;

    // Absolute path to the OTHER port's per-seed saves root ("<exe dir>/saves"). The OoT process
    // needs it to delete the paired MM save "<mmSaveDir>/ootmm-<tag>" when its OoT save is erased.
    // Empty => skip that deletion (co-save ledger is still removed).
    std::string mmSaveDir;

    // Non-empty arms the in-game self-test (scene-sweep check-claim audit): the port sweeps every
    // scene with checks after the save loads and streams a JSONL report here.
    std::string selftestReportPath;

    // Self-test mode: "audit" (default sweep), "collect", or "verify" (audit + collect + cross-game
    // handoff); collection modes run on an isolated "<tag>_verify" save/ledger.
    std::string selftestMode;

    // When false (the default), the port hides its debug / cheats / enhancements menus so players
    // cannot cheat mid-seed; the launcher flips this via an opt-in option, effective next boot.
    bool allowDebugMenus = false;

    Game bootGame = Game::Oot;            // which game this launch represents
    std::optional<uint32_t> bootEntrance; // when set, auto-boot positioned here (no title/file-select)
    std::optional<uint32_t> ootAge;       // OoT Link's age at the handoff (0 = adult, 1 = child)

    // When set, the port creates its render window as a WS_CHILD of this native handle instead of
    // a standalone top-level window. uint64 keeps the descriptor platform-neutral; the port casts
    // it to the native handle. Absent => standalone top-level window.
    std::optional<uint64_t> parentWindow;
};

[[nodiscard]] std::string SerializeBootConfig(const BootConfig& config);
[[nodiscard]] std::optional<BootConfig> ParseBootConfig(const std::string& jsonText);
[[nodiscard]] std::optional<BootConfig> LoadBootConfigFromFile(const std::filesystem::path& path);

} // namespace ootmm
