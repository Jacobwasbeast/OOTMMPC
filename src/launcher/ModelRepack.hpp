#pragma once

// Per-player model archive namespacing. Both engines resolve resources in one global path space
// (last mounted archive wins), so mounting a downloaded Link-model mod as-is would reskin the LOCAL
// player. This repacker rewrites a model .o2r into a per-player namespace so only that player's
// puppet renders with it:
//   "alt/<rest>"                  -> "pNN/<rest>"                 (SoH alternate-asset sets)
//   "objects/object_link<rest>"   -> "pNNobjs/object_link<rest>"  (canonical-path sets)
// NN = playerId as two lowercase hex digits. Both replacements are the SAME byte length as the
// original prefix, so references are rewritten in place: path strings verbatim, CRC64 path hashes
// (display lists reference textures/vertices/DLs by hash) as 8-byte value swaps. Entries outside
// those prefixes would be global overrides and are dropped. Output is a standard zip (libzip).

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ootmm::launcher {

struct RepackResult {
    bool ok = false;
    int kept = 0;        // entries written to the namespaced archive
    int dropped = 0;     // entries outside the model namespaces (global overrides)
    int stringRefs = 0;  // embedded path-string references rewritten
    int hashRefs = 0;    // embedded CRC64 hash references rewritten
    std::string error;
};

// Repacks a whole model set (child + adult + textures + ... archives; .o2r zip, 7z-format .o2r
// via 7z.exe when installed) into per-player-namespaced outputs. The set is processed jointly:
// the reference-rewrite table spans the union of every archive's entries because a set's
// archives reference each other's resources. Overwrites each output. One result per input.
//
// `baseArchives` (the game's own oot.o2r/soh.o2r or mm.o2r/2ship.o2r) extends the rewrite
// table with every vanilla objects/object_link* path, so references the set does NOT override
// (e.g. vanilla eye/mouth textures a skin reuses) are ALSO redirected into the namespace.
// Those must then resolve against a vanilla-fill archive (RepackVanillaFill below) — resolving
// them at the canonical path instead would go through the engine's alternate-asset
// substitution and leak the LOCAL player's skin onto the puppet.
[[nodiscard]] std::vector<RepackResult> RepackModelSet(const std::vector<std::filesystem::path>& inputs,
                                                       const std::vector<std::filesystem::path>& outputs,
                                                       uint16_t playerId,
                                                       const std::vector<std::filesystem::path>& baseArchives);

// Builds a player's vanilla-fill archive: every objects/object_link* entry of the base game
// archives (later archives win on duplicates), namespaced to pNNobjs/ with internal references
// rewritten. Mounted for EVERY remote player so puppets always render from their own namespace
// (fully vanilla until a real model set installs on top — the fill sorts/mounts first, the
// model archive overrides the entries it customizes).
[[nodiscard]] RepackResult RepackVanillaFill(const std::vector<std::filesystem::path>& baseArchives,
                                             const std::filesystem::path& output, uint16_t playerId);

// CRC-64/ECMA-182 of a resource path (matches libultraship StrHash64: MSB-first, init
// all-ones, no output inversion). Shared with the foreign-model mods builder.
[[nodiscard]] uint64_t PathCrc64(const std::string& s);

// "pNN" prefix for a player id, matching what the ports probe for puppet skeletons.
// NOTE: the local player's own model is NOT repacked to canonical paths — overriding the
// canonical Link assets would make every puppet's vanilla fallback wear the local skin.
// Local application goes through the ports' alternate-assets flag instead (forced on at
// session launch by the launcher).
[[nodiscard]] std::string CoopModelPrefix(uint16_t playerId);

} // namespace ootmm::launcher
