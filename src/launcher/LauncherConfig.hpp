#pragma once

// Persisted launcher GUI configuration (launcher.config.json next to the executable)
// plus the mods manager: each port auto-mounts every archive in "<exe dir>/mods", so
// enabling/disabling a mod = renaming it between "<name>.o2r" and "<name>.o2r.disabled".

#include <filesystem>
#include <string>
#include <vector>

namespace ootmm::launcher {

struct LauncherConfig {
    std::string seedPath;
    std::string ootExe;
    std::string mmExe;

    // Multiworld (only used when the loaded seed has playerCount > 1).
    std::string multiworldAddress; // host:port
    std::string playerName = "ootmm-pc";
    // Co-op player model archive SETS: ';' or ',' separated file names in each game's mods dir
    // (a model often needs child + adult + textures + voice archives); empty = vanilla.
    std::string playerModelOot;
    std::string playerModelMm;
    bool hostRelay = false;
    int relayPort = 42801;

    // Co-op: draw remote players' client names above their in-game puppets (live toggle).
    bool showPlayerNametags = true;
    // Co-op PvP: players can damage each other. Session-wide — toggling broadcasts the new state
    // to every connected launcher (last writer wins).
    bool enablePvp = false;

    // Appearance (runtime manager sidebar).
    int theme = 0; // 0 = auto (follow active game), 1 = OoT gold, 2 = MM violet
    float uiScale = 1.0f;
    int sidebarWidth = 380;
    bool showSidebar = true;

    // Cheat gate: debug / cheats / enhancements menus in both ports stay hidden unless enabled here.
    bool allowDebugMenus = false;

    // Game speed ("timer"): pace the scaled logic rate against the render framerate (fractional
    // accumulator) instead of whole-Hz steps, so speed changes stay smooth.
    bool speedMatchFramerate = true;
};

[[nodiscard]] LauncherConfig LoadLauncherConfig(const std::filesystem::path& path);
void SaveLauncherConfig(const std::filesystem::path& path, const LauncherConfig& config);

// ---------------------------------------------------------------------------
// Mods manager
// ---------------------------------------------------------------------------

struct ModEntry {
    std::filesystem::path path; // current on-disk path (with or without .disabled)
    std::string name;           // display name, e.g. "ootmm_mm_models.o2r"
    bool enabled = true;        // desired state (UI toggles this)
    bool onDiskEnabled = true;  // current on-disk state
    bool required = false;      // the OoTMM cross-game model archives must stay enabled
};

// Scans "<exeDir>/mods" for *.o2r / *.otr archives (and their .disabled forms).
[[nodiscard]] std::vector<ModEntry> ScanMods(const std::filesystem::path& exeDir);

// Applies the desired enabled state by renaming files; returns per-file errors.
[[nodiscard]] std::vector<std::string> ApplyModStates(std::vector<ModEntry>& mods);

} // namespace ootmm::launcher
