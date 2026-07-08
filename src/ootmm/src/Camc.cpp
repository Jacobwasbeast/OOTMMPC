#include "ootmm/Camc.hpp"

#include "ootmm/Seed.hpp"

#include <cstddef>
#include <unordered_map>
#include <utility>
#include <vector>

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

std::string ProgressiveTierName(const Seed& seed, Game game, const std::string& itemId, std::size_t priorGrants) {
    // og gi.yml tier names, chosen by prior-grant count as og's Item_Progressive does (progressive.c).
    std::string suffix = itemId;
    for (const char* prefix : { "OOT_", "MM_", "SHARED_" }) {
        if (suffix.rfind(prefix, 0) == 0) {
            suffix = suffix.substr(std::string(prefix).size());
            break;
        }
    }
    const bool oot = game == Game::Oot;
    std::vector<std::string> tiers;
    if (suffix == "BOMB_BAG") {
        tiers = { "Bomb Bag", "Big Bomb Bag", "Biggest Bomb Bag" };
    } else if (suffix == "BOMBCHU_BAG") {
        tiers = { "Bombchu Bag", "Big Bombchu Bag", "Biggest Bombchu Bag" };
    } else if (suffix == "BOW") {
        tiers = { oot ? "Fairy Bow" : "Hero's Bow", "Big Quiver", "Biggest Quiver" };
    } else if (suffix == "SLINGSHOT") {
        tiers = { "Fairy Slingshot", "Large Bullet Bag", "Largest Bullet Bag" };
    } else if (suffix == "HOOKSHOT") {
        if (itemId.rfind("MM_", 0) == 0) {
            // og MM shortHookshotMm: 2-copy pool = Short Hookshot -> Hookshot; single copy = Hookshot.
            std::size_t copies = 0;
            for (const ItemPlacement& p : seed.placements) {
                if (p.item.id == itemId && p.ownerPlayer == seed.playerId) {
                    ++copies;
                }
            }
            tiers = copies >= 2 ? std::vector<std::string>{ "Short Hookshot", "Hookshot" }
                                : std::vector<std::string>{ "Hookshot" };
        } else {
            tiers = { "Hookshot", "Longshot" };
        }
    } else if (suffix == "OCARINA") {
        tiers = { "Fairy Ocarina", "Ocarina of Time" };
    } else if (suffix == "WALLET") {
        // og progressiveWalletOot/Mm: ladder depends on childWallets/colossalWallets/bottomlessWallets.
        if (seed.GetBoolSetting("childWallets", false)) {
            tiers.push_back(oot ? "Child's Wallet" : "Child Wallet");
        }
        tiers.push_back(oot ? "Adult's Wallet" : "Adult Wallet");
        tiers.push_back(oot ? "Giant's Wallet" : "Giant Wallet");
        if (seed.GetBoolSetting("colossalWallets", false)) {
            tiers.push_back("Colossal Wallet");
        }
        if (seed.GetBoolSetting("bottomlessWallets", false)) {
            tiers.push_back("Bottomless Wallet");
        }
    } else if (suffix == "SCALE") {
        if (seed.GetBoolSetting("bronzeScale", false)) {
            tiers.push_back("Bronze Scale");
        }
        tiers.push_back("Silver Scale");
        tiers.push_back("Golden Scale");
    } else if (suffix == "STRENGTH") {
        tiers = { "Goron's Bracelet", "Silver Gauntlets", "Golden Gauntlets" };
    } else if (suffix == "MAGIC_UPGRADE") {
        tiers = { "Magic Upgrade", "Larger Magic Upgrade" };
    } else if (suffix == "SWORD") {
        tiers = oot ? std::vector<std::string>{ "Kokiri Sword", "Master Sword", "Giant's Knife", "Biggoron's Sword" }
                    : std::vector<std::string>{ "Kokiri Sword", "Razor Sword", "Gilded Sword" };
    } else if (suffix == "SHIELD") {
        tiers = oot ? std::vector<std::string>{ "Deku Shield", "Hylian Shield", "Mirror Shield" }
                    : std::vector<std::string>{ "Hero's Shield", "Mirror Shield" };
    } else if (suffix == "STICK_UPGRADE") {
        tiers = { "Deku Stick Upgrade", "Second Deku Stick Upgrade" };
    } else if (suffix == "NUT_UPGRADE") {
        tiers = { "Deku Nut Upgrade", "Second Deku Nut Upgrade" };
    } else if (suffix == "SONG_GORON_HALF") {
        tiers = { "Goron Lullaby Intro", "Goron Lullaby" };
    } else {
        return {};
    }
    if (tiers.empty()) {
        return {};
    }
    return tiers[priorGrants < tiers.size() ? priorGrants : tiers.size() - 1];
}

} // namespace ootmm
