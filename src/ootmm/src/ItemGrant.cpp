#include "ootmm/ItemGrant.hpp"

#include <array>
#include <optional>
#include <string_view>

namespace ootmm {
namespace {

struct SharedItemMapping {
    std::string_view shared;
    std::string_view oot;
    std::string_view mm;
};

constexpr std::array<SharedItemMapping, 33> kSharedItemMappings = {{
    { "SHARED_BOW", "OOT_BOW", "MM_BOW" },
    { "SHARED_BOMB_BAG", "OOT_BOMB_BAG", "MM_BOMB_BAG" },
    { "SHARED_BOMBCHU_BAG", "OOT_BOMBCHU_BAG", "MM_BOMBCHU_BAG" },
    { "SHARED_DEFENSE_UPGRADE", "OOT_DEFENSE_UPGRADE", "MM_DEFENSE_UPGRADE" },
    { "SHARED_GREAT_FAIRY_SWORD", "OOT_GREAT_FAIRY_SWORD", "MM_GREAT_FAIRY_SWORD" },
    { "SHARED_HAMMER", "OOT_HAMMER", "MM_HAMMER" },
    { "SHARED_HEART_CONTAINER", "OOT_HEART_CONTAINER", "MM_HEART_CONTAINER" },
    { "SHARED_HEART_PIECE", "OOT_HEART_PIECE", "MM_HEART_PIECE" },
    { "SHARED_HOOKSHOT", "OOT_HOOKSHOT", "MM_HOOKSHOT" },
    { "SHARED_LENS", "OOT_LENS", "MM_LENS" },
    { "SHARED_MAGIC_UPGRADE", "OOT_MAGIC_UPGRADE", "MM_MAGIC_UPGRADE" },
    { "SHARED_OCARINA", "OOT_OCARINA", "MM_OCARINA" },
    { "SHARED_SCALE", "OOT_SCALE", "MM_SCALE" },
    { "SHARED_SHIELD", "OOT_SHIELD", "MM_SHIELD" },
    { "SHARED_SHIELD_DEKU", "OOT_SHIELD_DEKU", "MM_SHIELD_DEKU" },
    { "SHARED_SHIELD_HYLIAN", "OOT_SHIELD_HYLIAN", "MM_SHIELD_HERO" },
    { "SHARED_SHIELD_MIRROR", "OOT_SHIELD_MIRROR", "MM_SHIELD_MIRROR" },
    { "SHARED_SLINGSHOT", "OOT_SLINGSHOT", "MM_SLINGSHOT" },
    { "SHARED_SONG_TIME", "OOT_SONG_TIME", "MM_SONG_TIME" },
    { "SHARED_SONG_EPONA", "OOT_SONG_EPONA", "MM_SONG_EPONA" },
    { "SHARED_SONG_STORMS", "OOT_SONG_STORMS", "MM_SONG_STORMS" },
    { "SHARED_SONG_HEALING", "OOT_SONG_HEALING", "MM_SONG_HEALING" },
    { "SHARED_SONG_SOARING", "OOT_SONG_SOARING", "MM_SONG_SOARING" },
    { "SHARED_SONG_AWAKENING", "OOT_SONG_AWAKENING", "MM_SONG_AWAKENING" },
    { "SHARED_STICK", "OOT_STICK", "MM_STICK" },
    { "SHARED_STICK_UPGRADE", "OOT_STICK_UPGRADE", "MM_STICK_UPGRADE" },
    { "SHARED_STRENGTH", "OOT_STRENGTH", "MM_STRENGTH" },
    { "SHARED_NUT", "OOT_NUT", "MM_NUT" },
    { "SHARED_NUT_UPGRADE", "OOT_NUT_UPGRADE", "MM_NUT_UPGRADE" },
    { "SHARED_WALLET", "OOT_WALLET", "MM_WALLET" },
    { "SHARED_BOOTS_IRON", "OOT_BOOTS_IRON", "MM_BOOTS_IRON" },
    { "SHARED_BOOTS_HOVER", "OOT_BOOTS_HOVER", "MM_BOOTS_HOVER" },
    { "SHARED_POWDER_KEG", "OOT_POWDER_KEG", "MM_POWDER_KEG" },
}};

[[nodiscard]] bool StartsWith(std::string_view value, std::string_view prefix) {
    return value.rfind(prefix, 0) == 0;
}

[[nodiscard]] bool EndsWith(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

[[nodiscard]] bool Contains(std::string_view value, std::string_view needle) {
    return value.find(needle) != std::string_view::npos;
}

[[nodiscard]] Game GameFromItemPrefix(std::string_view id, Game fallback) {
    if (StartsWith(id, "MM_")) {
        return Game::Mm;
    }
    if (StartsWith(id, "OOT_")) {
        return Game::Oot;
    }
    return fallback;
}

[[nodiscard]] std::optional<std::string> ResolveSharedItem(std::string_view id, Game activeGame) {
    for (const SharedItemMapping& mapping : kSharedItemMappings) {
        if (mapping.shared == id) {
            return std::string(activeGame == Game::Oot ? mapping.oot : mapping.mm);
        }
    }
    // OoTMM's sharing convention is UNIFORM: every shared item maps SHARED_<X> -> OOT_<X> / MM_<X>
    // with the SAME suffix (verified against the generator's logic/shared.ts). The table above is a
    // fast path; resolve any other SHARED_* id programmatically by swapping the prefix to the active
    // game, else ~120 shared ids fall through to a RawItem "SHARED_" id no adapter recognizes.
    constexpr std::string_view kSharedPrefix = "SHARED_";
    if (StartsWith(id, kSharedPrefix)) {
        const std::string_view suffix = id.substr(kSharedPrefix.size());
        return (activeGame == Game::Oot ? std::string("OOT_") : std::string("MM_")) + std::string(suffix);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<int> RupeeAmount(std::string_view id) {
    if (EndsWith(id, "RUPEE_GREEN")) return 1;
    if (EndsWith(id, "RUPEE_BLUE")) return 5;
    if (EndsWith(id, "RUPEE_RED")) return 20;
    if (EndsWith(id, "RUPEE_PURPLE")) return 50;
    if (EndsWith(id, "RUPEE_SILVER")) return 100;
    if (EndsWith(id, "RUPEE_GOLD") || EndsWith(id, "RUPEE_HUGE")) return 200;
    return std::nullopt;
}

[[nodiscard]] std::optional<int> CountSuffix(std::string_view id, std::string_view prefix) {
    const std::size_t pos = id.rfind(prefix);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view suffix = id.substr(pos + prefix.size());
    if (suffix == "5") return 5;
    if (suffix == "10") return 10;
    if (suffix == "20") return 20;
    if (suffix == "30") return 30;
    if (suffix == "40") return 40;
    return std::nullopt;
}

[[nodiscard]] std::vector<GrantOperation> ResolveConcreteItem(std::string itemId, Game activeGame) {
    const Game targetGame = GameFromItemPrefix(itemId, activeGame);
    const std::string_view id = itemId;

    if (const std::optional<int> amount = RupeeAmount(id)) {
        return { { .kind = GrantOperationKind::AddRupees, .targetGame = targetGame, .itemId = itemId, .amount = *amount } };
    }

    if (const std::optional<int> amount = CountSuffix(id, "ARROWS_")) {
        return { { .kind = GrantOperationKind::AddAmmo, .targetGame = targetGame, .itemId = itemId, .slot = "BOW", .amount = *amount } };
    }
    if (const std::optional<int> amount = CountSuffix(id, "BOMBS_")) {
        return { { .kind = GrantOperationKind::AddAmmo, .targetGame = targetGame, .itemId = itemId, .slot = "BOMB", .amount = *amount } };
    }
    if (const std::optional<int> amount = CountSuffix(id, "BOMBCHU_")) {
        return { { .kind = GrantOperationKind::AddAmmo, .targetGame = targetGame, .itemId = itemId, .slot = "BOMBCHU", .amount = *amount } };
    }
    if (const std::optional<int> amount = CountSuffix(id, "NUTS_")) {
        return { { .kind = GrantOperationKind::AddAmmo, .targetGame = targetGame, .itemId = itemId, .slot = "NUT", .amount = *amount } };
    }
    if (const std::optional<int> amount = CountSuffix(id, "STICKS_")) {
        return { { .kind = GrantOperationKind::AddAmmo, .targetGame = targetGame, .itemId = itemId, .slot = "STICK", .amount = *amount } };
    }
    if (const std::optional<int> amount = CountSuffix(id, "SEEDS_")) {
        return { { .kind = GrantOperationKind::AddAmmo, .targetGame = targetGame, .itemId = itemId, .slot = "SLINGSHOT", .amount = *amount } };
    }

    if (EndsWith(id, "RECOVERY_HEART")) {
        return { { .kind = GrantOperationKind::AddHealth, .targetGame = targetGame, .itemId = itemId, .amount = 16 } };
    }
    if (EndsWith(id, "HEART_PIECE")) {
        // Health capacity is in quarter-heart units (16 = one full heart), matching
        // AddHealth (a recovery heart heals 16). A heart piece is a quarter heart (+4);
        // four pieces therefore total one full heart, exactly like a heart container.
        return { { .kind = GrantOperationKind::AddHealthCapacity, .targetGame = targetGame, .itemId = itemId, .slot = "HEART_PIECE", .amount = 4 } };
    }
    if (EndsWith(id, "HEART_CONTAINER")) {
        return { { .kind = GrantOperationKind::AddHealthCapacity, .targetGame = targetGame, .itemId = itemId, .slot = "HEART_CONTAINER", .amount = 16 } };
    }
    if (EndsWith(id, "MAGIC_JAR_SMALL")) {
        return { { .kind = GrantOperationKind::AddMagic, .targetGame = targetGame, .itemId = itemId, .amount = 8 } };
    }
    if (EndsWith(id, "MAGIC_JAR_LARGE")) {
        return { { .kind = GrantOperationKind::AddMagic, .targetGame = targetGame, .itemId = itemId, .amount = 16 } };
    }
    if (Contains(id, "GS_TOKEN")) {
        return { { .kind = GrantOperationKind::AddToken, .targetGame = targetGame, .itemId = itemId, .slot = "GOLD_SKULLTULA", .amount = 1 } };
    }
    if (Contains(id, "OWL_")) {
        return { { .kind = GrantOperationKind::ActivateOwl, .targetGame = targetGame, .itemId = itemId, .amount = 1 } };
    }
    if (Contains(id, "_BOSS_KEY") || Contains(id, "_SMALL_KEY_") || Contains(id, "_KEY_RING_") ||
        StartsWith(id, "OOT_MAP_") || StartsWith(id, "MM_MAP_") || Contains(id, "_COMPASS_")) {
        return { { .kind = GrantOperationKind::AddDungeonItem, .targetGame = targetGame, .itemId = itemId, .amount = 1 } };
    }
    if (Contains(id, "SONG_") || Contains(id, "MEDALLION_") || Contains(id, "STONE_")) {
        return { { .kind = GrantOperationKind::SetQuestFlag, .targetGame = targetGame, .itemId = itemId, .amount = 1 } };
    }

    return { { .kind = GrantOperationKind::RawItem, .targetGame = targetGame, .itemId = itemId, .amount = 1 } };
}

} // namespace

std::vector<GrantOperation> ResolveGrantOperations(const ItemRef& item, Game activeGame) {
    if (const std::optional<std::string> concrete = ResolveSharedItem(item.id, activeGame)) {
        return ResolveConcreteItem(*concrete, activeGame);
    }
    return ResolveConcreteItem(item.id, activeGame);
}

std::string ToString(GrantOperationKind kind) {
    switch (kind) {
        case GrantOperationKind::RawItem:
            return "RawItem";
        case GrantOperationKind::AddRupees:
            return "AddRupees";
        case GrantOperationKind::AddAmmo:
            return "AddAmmo";
        case GrantOperationKind::AddHealth:
            return "AddHealth";
        case GrantOperationKind::AddHealthCapacity:
            return "AddHealthCapacity";
        case GrantOperationKind::AddMagic:
            return "AddMagic";
        case GrantOperationKind::AddToken:
            return "AddToken";
        case GrantOperationKind::AddDungeonItem:
            return "AddDungeonItem";
        case GrantOperationKind::SetQuestFlag:
            return "SetQuestFlag";
        case GrantOperationKind::ActivateOwl:
            return "ActivateOwl";
    }
    return "Unknown";
}

} // namespace ootmm
