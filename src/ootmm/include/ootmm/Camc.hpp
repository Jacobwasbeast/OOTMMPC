#pragma once

#include "ootmm/Types.hpp"

#include <cstdint>
#include <string>

namespace ootmm {

struct Seed;

// OoTMM CAMC ("Container Appearance Matches Content"). Chests, pots, grass, gold-skulltulas, etc.
// take on an appearance (color / texture / chest size) indicating the CATEGORY of item inside,
// gated by the `csmc` setting (never / agony — only with the Stone of Agony / always).
//
// The category is the item's INTRINSIC type (OoTMM combo/csmc.h + csmc_util.c csmcFromItem), NOT
// the per-seed `major`/WoTH flag — e.g. silver rupees are Key, masks are Major, shields/bombchu
// are Normal. The authoritative classification comes from OoTMM's generated data-gi.json (baked
// into CamcItemType.inc). Values match the CSMC_* macros so a port can map them onto its native
// CSMC renderer.
enum class CamcCategory : uint8_t {
    Normal = 0,     // junk / rupees / ammo / misc minor
    BossKey = 1,
    Major = 2,      // progression / masks / "Way of the Hero"
    Key = 3,        // small key / key ring / silver rupee / silver-rupee pouch
    Spider = 4,     // gold skulltula token
    Fairy = 5,      // stray fairy
    Heart = 6,      // heart piece / container
    Soul = 7,       // enemy / npc / misc soul
    MapCompass = 8, // dungeon map / compass
};

struct CamcColor {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

// Raw category from the OoTMM id table only (no sub-setting downgrades, no bombchu special-case).
// Unknown ids -> Normal. Mostly useful for testing.
[[nodiscard]] CamcCategory CamcRawCategoryForItem(const std::string& itemId);

// The category for an OoTMM item id, honoring the bombchu special-case and the csmcHearts /
// csmcMapCompass sub-settings (which downgrade Heart / MapCompass to Normal when off). `game` is
// the item's game, used for the bombchuBehaviorOot/Mm setting.
[[nodiscard]] CamcCategory CamcCategoryForItem(const std::string& itemId, Game game, const Seed& seed);

// Whether CAMC is active. csmc=="always" -> true; "agony" -> hasStoneOfAgony; "never" -> false.
// The Stone-of-Agony state is supplied by the caller because it lives in the native save /
// cross-game runtime, not in the seed.
[[nodiscard]] bool CamcEnabled(const Seed& seed, bool hasStoneOfAgony);
[[nodiscard]] bool CamcEnabledSkulltula(const Seed& seed, bool hasStoneOfAgony);
[[nodiscard]] bool CamcEnabledCow(const Seed& seed, bool hasStoneOfAgony);

// OoTMM's canonical RGB color for a category (csmc_util.c csmcTypeColor palette), for the ports'
// custom color-overlay renderers (gold skulltulas, freestanding) where no native CSMC exists.
[[nodiscard]] CamcColor CamcTypeColor(CamcCategory category);

} // namespace ootmm
