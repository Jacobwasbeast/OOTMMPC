#pragma once

// Native port of tools/build_foreign_model_mods.py (+ build_csmc_assets.py): builds the
// cross-game model mod archives (mods/ootmm_mm_models.o2r next to soh.exe, mods/ootmm_oot_models.o2r
// next to 2ship.exe) from the games' own extracted archives. Table-driven from the generated
// dispatch tables (ForeignModelTables.h) and og CSMC art (CsmcArtData.gen.cpp). Output is
// byte-identical per entry to the Python builder (tools/compare_mods_archives.py); only zip
// metadata may differ.

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace ootmm::launcher {

// One embedded og CSMC art texture (decoded RGBA8888) — defined in CsmcArtData.gen.cpp.
struct CsmcArt {
    const char* rel; // "pots/major_side.png" — the same keys build_csmc_assets.py uses
    int width;
    int height;
    const unsigned char* rgba;
};
const CsmcArt* FindCsmcArt(const std::string& rel);

struct ForeignModsResult {
    bool ok = false;
    int entries = 0;                  // entries written across both archives
    std::vector<std::string> log;     // per-archive summaries + skip diagnostics
    std::string error;                // first fatal error ('' when ok)
};

// Builds both cross-game model archives. sohDir/mmDir hold soh.exe / 2ship.exe (base archives
// beside them, outputs under their mods/); pass an empty path to skip a game.
[[nodiscard]] ForeignModsResult BuildForeignModelMods(const std::filesystem::path& sohDir,
                                                      const std::filesystem::path& mmDir);

// True when a game's cross-game model archive is missing or older than any of its inputs
// (both games' base archives + the launcher itself, whose embedded tables version the bake).
[[nodiscard]] bool ForeignModelModsStale(const std::filesystem::path& sohDir, const std::filesystem::path& mmDir,
                                         bool checkOot, bool checkMm);

} // namespace ootmm::launcher
