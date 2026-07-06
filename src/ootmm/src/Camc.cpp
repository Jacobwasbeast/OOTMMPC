#include "ootmm/Camc.hpp"

#include "ootmm/Seed.hpp"

#include <cstddef>
#include <unordered_map>
#include <utility>

namespace ootmm {
namespace {

// 933-entry id -> category table, generated from OoTMM packages/core/dist/data-gi.json.
#include "CamcItemType.inc"

const std::unordered_map<std::string, CamcCategory>& CamcTable() {
    static const std::unordered_map<std::string, CamcCategory> table = [] {
        std::unordered_map<std::string, CamcCategory> m;
        m.reserve(kCamcItemTypeCount * 2);
        for (std::size_t i = 0; i < kCamcItemTypeCount; ++i) {
            m.emplace(kCamcItemType[i].first, kCamcItemType[i].second);
        }
        return m;
    }();
    return table;
}

// OoTMM csmc_util.c bombchu special-case: a raw bombchu PICKUP becomes Major when that game's
// bombchuBehavior is "bag-first" (the first pickup grants the bag). The *_BOMBCHU_BAG* ids are
// already Major in the table; only the bare pickup ids are remapped here.
bool IsBombchuMajorWhenBagFirst(const std::string& id, Game game) {
    if (game == Game::Oot) {
        return id == "OOT_BOMBCHU_5" || id == "OOT_BOMBCHU_10" || id == "OOT_BOMBCHU_20";
    }
    return id == "MM_BOMBCHU" || id == "MM_BOMBCHU_5" || id == "MM_BOMBCHU_10" ||
           id == "MM_BOMBCHU_20";
}

} // namespace

CamcCategory CamcRawCategoryForItem(const std::string& itemId) {
    const auto& table = CamcTable();
    const auto it = table.find(itemId);
    return it != table.end() ? it->second : CamcCategory::Normal;
}

CamcCategory CamcCategoryForItem(const std::string& itemId, Game game, const Seed& seed) {
    if (IsBombchuMajorWhenBagFirst(itemId, game)) {
        const char* key = game == Game::Oot ? "bombchuBehaviorOot" : "bombchuBehaviorMm";
        if (seed.GetStringSetting(key, "") == "bagFirst") {
            return CamcCategory::Major;
        }
    }

    const CamcCategory category = CamcRawCategoryForItem(itemId);

    // csmcHearts / csmcMapCompass (default true) downgrade these to Normal when off.
    if (category == CamcCategory::Heart && !seed.GetBoolSetting("csmcHearts", true)) {
        return CamcCategory::Normal;
    }
    if (category == CamcCategory::MapCompass && !seed.GetBoolSetting("csmcMapCompass", true)) {
        return CamcCategory::Normal;
    }
    return category;
}

bool CamcEnabled(const Seed& seed, bool hasStoneOfAgony) {
    const std::string mode = seed.GetStringSetting("csmc", "always");
    if (mode == "always") {
        return true;
    }
    if (mode == "agony") {
        return hasStoneOfAgony;
    }
    return false; // "never" or unrecognized
}

bool CamcEnabledSkulltula(const Seed& seed, bool hasStoneOfAgony) {
    return CamcEnabled(seed, hasStoneOfAgony) && seed.GetBoolSetting("csmcSkulltula", false);
}

bool CamcEnabledCow(const Seed& seed, bool hasStoneOfAgony) {
    return CamcEnabled(seed, hasStoneOfAgony) && seed.GetBoolSetting("csmcCow", false);
}

CamcColor CamcTypeColor(CamcCategory category) {
    switch (category) {
    case CamcCategory::BossKey:    return { 0x00, 0x00, 0xFF };
    case CamcCategory::Major:      return { 0xFF, 0xFF, 0x00 };
    case CamcCategory::Key:        return { 0x44, 0x44, 0x44 };
    case CamcCategory::Spider:     return { 0xFF, 0xFF, 0xFF };
    case CamcCategory::Fairy:      return { 0xFF, 0x7A, 0xFB };
    case CamcCategory::Heart:      return { 0xFF, 0x00, 0x00 };
    case CamcCategory::Soul:       return { 0x34, 0x0B, 0x9C };
    case CamcCategory::MapCompass: return { 0xC7, 0x50, 0x00 };
    case CamcCategory::Normal:     break;
    }
    return { 0x29, 0x14, 0x0A };
}

} // namespace ootmm
