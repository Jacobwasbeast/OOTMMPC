#include "ootmm/SpecialCond.hpp"

#include <initializer_list>

namespace ootmm {
namespace {

// Sum ledger counts (regrant duplicates excluded); SHARED_ variants listed wherever OoTMM emits either form.
std::size_t CountAll(const Runtime& runtime, std::initializer_list<const char*> ids) {
    std::size_t owned = 0;
    for (const char* id : ids) {
        owned += runtime.CountGrantedItemIdOnce(id);
    }
    return owned;
}

} // namespace

std::size_t SpecialCondOwned(const Runtime& runtime, const Seed::SpecialCond& cond) {
    const auto field = [&](const char* name) {
        const auto it = cond.fields.find(name);
        return it != cond.fields.end() && it->second;
    };

    std::size_t owned = 0;
    if (field("stones")) {
        owned += CountAll(runtime, { "OOT_STONE_EMERALD", "OOT_STONE_RUBY", "OOT_STONE_SAPPHIRE" });
    }
    if (field("medallions")) {
        owned += CountAll(runtime, { "OOT_MEDALLION_FOREST", "OOT_MEDALLION_FIRE", "OOT_MEDALLION_WATER",
                                     "OOT_MEDALLION_SPIRIT", "OOT_MEDALLION_SHADOW", "OOT_MEDALLION_LIGHT" });
    }
    if (field("remains")) {
        owned += CountAll(runtime, { "MM_REMAINS_ODOLWA", "MM_REMAINS_GOHT", "MM_REMAINS_GYORG",
                                     "MM_REMAINS_TWINMOLD" });
    }
    if (field("skullsGold")) {
        owned += CountAll(runtime, { "OOT_GS_TOKEN", "SHARED_GS_TOKEN" });
    }
    if (field("skullsSwamp")) {
        owned += runtime.CountGrantedItemIdOnce("MM_GS_TOKEN_SWAMP");
    }
    if (field("skullsOcean")) {
        owned += runtime.CountGrantedItemIdOnce("MM_GS_TOKEN_OCEAN");
    }
    if (field("fairiesWF")) {
        owned += runtime.CountGrantedItemIdOnce("MM_STRAY_FAIRY_WF");
    }
    if (field("fairiesSH")) {
        owned += runtime.CountGrantedItemIdOnce("MM_STRAY_FAIRY_SH");
    }
    if (field("fairiesGB")) {
        owned += runtime.CountGrantedItemIdOnce("MM_STRAY_FAIRY_GB");
    }
    if (field("fairiesST")) {
        owned += runtime.CountGrantedItemIdOnce("MM_STRAY_FAIRY_ST");
    }
    if (field("fairyTown")) {
        owned += runtime.CountGrantedItemIdOnce("MM_STRAY_FAIRY_TOWN");
    }
    // og masks families (config.c): 20 regular MM, 4 transformation, OoT trade + Blast/Stone masks.
    if (field("masksRegular")) {
        owned += CountAll(runtime, { "MM_MASK_POSTMAN",       "MM_MASK_ALL_NIGHT", "MM_MASK_BLAST",
                                     "MM_MASK_STONE",         "MM_MASK_GREAT_FAIRY", "MM_MASK_KEATON",
                                     "MM_MASK_BREMEN",        "MM_MASK_BUNNY",     "MM_MASK_DON_GERO",
                                     "MM_MASK_SCENTS",        "MM_MASK_ROMANI",    "MM_MASK_TROUPE_LEADER",
                                     "MM_MASK_KAFEI",         "MM_MASK_COUPLE",    "MM_MASK_TRUTH",
                                     "MM_MASK_KAMARO",        "MM_MASK_GIBDO",     "MM_MASK_GARO",
                                     "MM_MASK_CAPTAIN",       "MM_MASK_GIANT" });
    }
    if (field("masksTransform")) {
        owned += CountAll(runtime, { "MM_MASK_DEKU", "MM_MASK_GORON", "MM_MASK_ZORA", "MM_MASK_FIERCE_DEITY" });
    }
    if (field("masksOot")) {
        owned += CountAll(runtime, { "OOT_MASK_KEATON", "OOT_MASK_SKULL", "OOT_MASK_SPOOKY", "OOT_MASK_BUNNY",
                                     "OOT_MASK_GORON", "OOT_MASK_ZORA", "OOT_MASK_GERUDO", "OOT_MASK_TRUTH",
                                     "OOT_MASK_BLAST", "OOT_MASK_STONE" });
    }
    if (field("triforce")) {
        owned += CountAll(runtime, { "SHARED_TRIFORCE", "OOT_TRIFORCE", "MM_TRIFORCE", "SHARED_TRIFORCE_POWER",
                                     "SHARED_TRIFORCE_COURAGE", "SHARED_TRIFORCE_WISDOM", "OOT_TRIFORCE_POWER",
                                     "OOT_TRIFORCE_COURAGE", "OOT_TRIFORCE_WISDOM" });
    }
    if (field("coinsGreen")) {
        owned += CountAll(runtime, { "OOT_COIN_GREEN", "MM_COIN_GREEN" });
    }
    if (field("coinsBlue")) {
        owned += CountAll(runtime, { "OOT_COIN_BLUE", "MM_COIN_BLUE" });
    }
    if (field("coinsRed")) {
        owned += CountAll(runtime, { "OOT_COIN_RED", "MM_COIN_RED" });
    }
    if (field("coinsYellow")) {
        owned += CountAll(runtime, { "OOT_COIN_YELLOW", "MM_COIN_YELLOW" });
    }
    return owned;
}

bool SpecialCondSatisfied(const Runtime& runtime, const Seed& seed, const std::string& name) {
    const auto it = seed.specialConds.find(name);
    if (it == seed.specialConds.end() || it->second.count <= 0) {
        return true;
    }
    return SpecialCondOwned(runtime, it->second) >= static_cast<std::size_t>(it->second.count);
}

} // namespace ootmm
