#include "ootmm/NativeGrantAction.hpp"

#include <array>
#include <string_view>

namespace ootmm {
namespace {

struct SymbolMapping {
    std::string_view idPart;
    NativePort port;
    std::string_view symbol;
};

constexpr std::array<SymbolMapping, 12> kAmmoSymbols = {{
    { "BOW", NativePort::ShipOfHarkinian, "ITEM_BOW" },
    { "BOMB", NativePort::ShipOfHarkinian, "ITEM_BOMB" },
    { "BOMBCHU", NativePort::ShipOfHarkinian, "ITEM_BOMBCHU" },
    { "NUT", NativePort::ShipOfHarkinian, "ITEM_NUT" },
    { "STICK", NativePort::ShipOfHarkinian, "ITEM_STICK" },
    { "SLINGSHOT", NativePort::ShipOfHarkinian, "ITEM_SLINGSHOT" },
    { "BOW", NativePort::TwoShip2Harkinian, "ITEM_BOW" },
    { "BOMB", NativePort::TwoShip2Harkinian, "ITEM_BOMB" },
    { "BOMBCHU", NativePort::TwoShip2Harkinian, "ITEM_BOMBCHU" },
    { "NUT", NativePort::TwoShip2Harkinian, "ITEM_DEKU_NUT" },
    { "STICK", NativePort::TwoShip2Harkinian, "ITEM_DEKU_STICK" },
    { "SLINGSHOT", NativePort::TwoShip2Harkinian, "ITEM_SLINGSHOT" },
}};

constexpr std::array<SymbolMapping, 16> kOotDungeonSymbols = {{
    { "DEKU_TREE", NativePort::ShipOfHarkinian, "SCENE_DEKU_TREE" },
    { "DODONGOS_CAVERN", NativePort::ShipOfHarkinian, "SCENE_DODONGOS_CAVERN" },
    { "JABU_JABU", NativePort::ShipOfHarkinian, "SCENE_JABU_JABU" },
    { "FOREST", NativePort::ShipOfHarkinian, "SCENE_FOREST_TEMPLE" },
    { "FIRE", NativePort::ShipOfHarkinian, "SCENE_FIRE_TEMPLE" },
    { "WATER", NativePort::ShipOfHarkinian, "SCENE_WATER_TEMPLE" },
    { "SPIRIT", NativePort::ShipOfHarkinian, "SCENE_SPIRIT_TEMPLE" },
    { "SHADOW", NativePort::ShipOfHarkinian, "SCENE_SHADOW_TEMPLE" },
    { "BOTW", NativePort::ShipOfHarkinian, "SCENE_BOTTOM_OF_THE_WELL" },
    { "ICE_CAVERN", NativePort::ShipOfHarkinian, "SCENE_ICE_CAVERN" },
    { "GANON", NativePort::ShipOfHarkinian, "SCENE_GANONS_TOWER" },
    { "DT", NativePort::ShipOfHarkinian, "SCENE_DEKU_TREE" },
    { "DC", NativePort::ShipOfHarkinian, "SCENE_DODONGOS_CAVERN" },
    { "JJ", NativePort::ShipOfHarkinian, "SCENE_JABU_JABU" },
    { "IC", NativePort::ShipOfHarkinian, "SCENE_ICE_CAVERN" },
    { "GTG", NativePort::ShipOfHarkinian, "SCENE_GERUDO_TRAINING_GROUND" },
}};

constexpr std::array<SymbolMapping, 8> kMmDungeonSymbols = {{
    { "WF", NativePort::TwoShip2Harkinian, "DUNGEON_SCENE_INDEX_WOODFALL_TEMPLE" },
    { "WOODFALL", NativePort::TwoShip2Harkinian, "DUNGEON_SCENE_INDEX_WOODFALL_TEMPLE" },
    { "SH", NativePort::TwoShip2Harkinian, "DUNGEON_SCENE_INDEX_SNOWHEAD_TEMPLE" },
    { "SNOWHEAD", NativePort::TwoShip2Harkinian, "DUNGEON_SCENE_INDEX_SNOWHEAD_TEMPLE" },
    { "GB", NativePort::TwoShip2Harkinian, "DUNGEON_SCENE_INDEX_GREAT_BAY_TEMPLE" },
    { "GREAT_BAY", NativePort::TwoShip2Harkinian, "DUNGEON_SCENE_INDEX_GREAT_BAY_TEMPLE" },
    { "ST", NativePort::TwoShip2Harkinian, "DUNGEON_SCENE_INDEX_STONE_TOWER_TEMPLE" },
    { "STONE_TOWER", NativePort::TwoShip2Harkinian, "DUNGEON_SCENE_INDEX_STONE_TOWER_TEMPLE" },
}};

constexpr std::array<SymbolMapping, 30> kQuestSymbols = {{
    { "OOT_SONG_MINUET", NativePort::ShipOfHarkinian, "QUEST_SONG_MINUET" },
    { "OOT_SONG_BOLERO", NativePort::ShipOfHarkinian, "QUEST_SONG_BOLERO" },
    { "OOT_SONG_SERENADE", NativePort::ShipOfHarkinian, "QUEST_SONG_SERENADE" },
    { "OOT_SONG_REQUIEM", NativePort::ShipOfHarkinian, "QUEST_SONG_REQUIEM" },
    { "OOT_SONG_NOCTURNE", NativePort::ShipOfHarkinian, "QUEST_SONG_NOCTURNE" },
    { "OOT_SONG_PRELUDE", NativePort::ShipOfHarkinian, "QUEST_SONG_PRELUDE" },
    { "OOT_SONG_LULLABY", NativePort::ShipOfHarkinian, "QUEST_SONG_LULLABY" },
    { "OOT_SONG_EPONA", NativePort::ShipOfHarkinian, "QUEST_SONG_EPONA" },
    { "OOT_SONG_SARIA", NativePort::ShipOfHarkinian, "QUEST_SONG_SARIA" },
    { "OOT_SONG_SUN", NativePort::ShipOfHarkinian, "QUEST_SONG_SUN" },
    { "OOT_SONG_TIME", NativePort::ShipOfHarkinian, "QUEST_SONG_TIME" },
    { "OOT_SONG_STORMS", NativePort::ShipOfHarkinian, "QUEST_SONG_STORMS" },
    { "OOT_MEDALLION_FOREST", NativePort::ShipOfHarkinian, "QUEST_MEDALLION_FOREST" },
    { "OOT_MEDALLION_FIRE", NativePort::ShipOfHarkinian, "QUEST_MEDALLION_FIRE" },
    { "OOT_MEDALLION_WATER", NativePort::ShipOfHarkinian, "QUEST_MEDALLION_WATER" },
    { "OOT_MEDALLION_SPIRIT", NativePort::ShipOfHarkinian, "QUEST_MEDALLION_SPIRIT" },
    { "OOT_MEDALLION_SHADOW", NativePort::ShipOfHarkinian, "QUEST_MEDALLION_SHADOW" },
    { "OOT_MEDALLION_LIGHT", NativePort::ShipOfHarkinian, "QUEST_MEDALLION_LIGHT" },
    { "OOT_STONE_EMERALD", NativePort::ShipOfHarkinian, "QUEST_KOKIRI_EMERALD" },
    { "OOT_STONE_RUBY", NativePort::ShipOfHarkinian, "QUEST_GORON_RUBY" },
    { "OOT_STONE_SAPPHIRE", NativePort::ShipOfHarkinian, "QUEST_ZORA_SAPPHIRE" },
    { "MM_SONG_SONATA", NativePort::TwoShip2Harkinian, "QUEST_SONG_SONATA" },
    { "MM_SONG_LULLABY", NativePort::TwoShip2Harkinian, "QUEST_SONG_LULLABY" },
    { "MM_SONG_NOVA", NativePort::TwoShip2Harkinian, "QUEST_SONG_BOSSA_NOVA" },
    { "MM_SONG_ELEGY", NativePort::TwoShip2Harkinian, "QUEST_SONG_ELEGY" },
    { "MM_SONG_OATH", NativePort::TwoShip2Harkinian, "QUEST_SONG_OATH" },
    { "MM_SONG_TIME", NativePort::TwoShip2Harkinian, "QUEST_SONG_TIME" },
    { "MM_SONG_HEALING", NativePort::TwoShip2Harkinian, "QUEST_SONG_HEALING" },
    { "MM_SONG_EPONA", NativePort::TwoShip2Harkinian, "QUEST_SONG_EPONA" },
    { "MM_SONG_SOARING", NativePort::TwoShip2Harkinian, "QUEST_SONG_SOARING" },
}};

[[nodiscard]] bool Contains(std::string_view value, std::string_view needle) {
    return value.find(needle) != std::string_view::npos;
}

[[nodiscard]] std::optional<std::string> LookupSymbol(std::string_view value, NativePort port, const auto& mappings) {
    for (const SymbolMapping& mapping : mappings) {
        if (mapping.port == port && Contains(value, mapping.idPart)) {
            return std::string(mapping.symbol);
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> DungeonItemSymbol(const GrantOperation& operation, NativePort port) {
    const std::string_view id = operation.itemId;
    if (Contains(id, "BOSS_KEY")) {
        return port == NativePort::ShipOfHarkinian ? "DUNGEON_KEY_BOSS" : "DUNGEON_BOSS_KEY";
    }
    if (Contains(id, "COMPASS")) {
        return port == NativePort::ShipOfHarkinian ? "DUNGEON_COMPASS" : "DUNGEON_COMPASS";
    }
    if (Contains(id, "MAP")) {
        return port == NativePort::ShipOfHarkinian ? "DUNGEON_MAP" : "DUNGEON_MAP";
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::string> DungeonSceneSymbol(const GrantOperation& operation, NativePort port) {
    if (port == NativePort::ShipOfHarkinian) {
        return LookupSymbol(operation.itemId, port, kOotDungeonSymbols);
    }
    return LookupSymbol(operation.itemId, port, kMmDungeonSymbols);
}

} // namespace

std::optional<NativeGrantAction> ResolveNativeGrantAction(const ResolvedGrant& grant, NativePort port) {
    const GrantOperation& operation = grant.operation;
    switch (operation.kind) {
        case GrantOperationKind::RawItem:
            if (!grant.nativeSymbol.has_value()) {
                return std::nullopt;
            }
            if (port == NativePort::TwoShip2Harkinian && !grant.nativeSymbol->randomizerEnum.empty()) {
                return NativeGrantAction{
                    .port = port,
                    .kind = NativeGrantActionKind::GiveRandomizerItem,
                    .functionName = "Rando::GiveItem",
                    .primarySymbol = grant.nativeSymbol->randomizerEnum,
                    .itemId = operation.itemId,
                    .amount = operation.amount,
                };
            }
            return NativeGrantAction{
                .port = port,
                .kind = NativeGrantActionKind::GiveRawItem,
                .functionName = "Item_Give",
                .primarySymbol = grant.nativeSymbol->itemEnum,
                .itemId = operation.itemId,
                .amount = operation.amount,
            };
        case GrantOperationKind::AddRupees:
            return NativeGrantAction{ .port = port, .kind = NativeGrantActionKind::ChangeRupees, .functionName = "Rupees_ChangeBy", .itemId = operation.itemId, .amount = operation.amount };
        case GrantOperationKind::AddAmmo:
            if (const std::optional<std::string> symbol = LookupSymbol(operation.slot, port, kAmmoSymbols)) {
                return NativeGrantAction{ .port = port, .kind = NativeGrantActionKind::ChangeAmmo, .functionName = "Inventory_ChangeAmmo", .primarySymbol = *symbol, .itemId = operation.itemId, .amount = operation.amount };
            }
            return std::nullopt;
        case GrantOperationKind::AddHealth:
            return NativeGrantAction{ .port = port, .kind = NativeGrantActionKind::ChangeHealth, .functionName = "Health_ChangeBy", .itemId = operation.itemId, .amount = operation.amount };
        case GrantOperationKind::AddHealthCapacity:
            // operation.amount is already in healthCapacity units (quarter-hearts; 16 = one heart),
            // added to gSaveContext.healthCapacity verbatim by the port adapters.
            return NativeGrantAction{ .port = port, .kind = NativeGrantActionKind::AddHealthCapacity, .primarySymbol = "gSaveContext.healthCapacity", .itemId = operation.itemId, .amount = operation.amount };
        case GrantOperationKind::AddMagic:
            return NativeGrantAction{ .port = port, .kind = NativeGrantActionKind::AddMagic, .functionName = port == NativePort::ShipOfHarkinian ? "Magic_RequestChange" : "Magic_Add", .itemId = operation.itemId, .amount = operation.amount };
        case GrantOperationKind::AddToken:
            return NativeGrantAction{ .port = port, .kind = NativeGrantActionKind::AddToken, .primarySymbol = port == NativePort::ShipOfHarkinian ? "gSaveContext.inventory.gsTokens" : "QUEST_SKULL_TOKEN", .itemId = operation.itemId, .amount = operation.amount };
        case GrantOperationKind::AddDungeonItem: {
            if (Contains(operation.itemId, "SMALL_KEY") || Contains(operation.itemId, "KEY_RING")) {
                if (const std::optional<std::string> scene = DungeonSceneSymbol(operation, port)) {
                    return NativeGrantAction{ .port = port, .kind = NativeGrantActionKind::AddDungeonKey, .primarySymbol = *scene, .itemId = operation.itemId, .amount = operation.amount };
                }
                return std::nullopt;
            }
            const std::optional<std::string> dungeonItem = DungeonItemSymbol(operation, port);
            const std::optional<std::string> scene = DungeonSceneSymbol(operation, port);
            if (!dungeonItem.has_value() || !scene.has_value()) {
                return std::nullopt;
            }
            return NativeGrantAction{ .port = port, .kind = NativeGrantActionKind::SetDungeonItem, .functionName = "SET_DUNGEON_ITEM", .primarySymbol = *dungeonItem, .secondarySymbol = *scene, .itemId = operation.itemId, .amount = operation.amount };
        }
        case GrantOperationKind::SetQuestFlag:
            if (const std::optional<std::string> symbol = LookupSymbol(operation.itemId, port, kQuestSymbols)) {
                return NativeGrantAction{ .port = port, .kind = NativeGrantActionKind::SetQuestFlag, .functionName = "SET_QUEST_ITEM", .primarySymbol = *symbol, .itemId = operation.itemId, .amount = operation.amount };
            }
            return std::nullopt;
        case GrantOperationKind::ActivateOwl:
            return NativeGrantAction{ .port = port, .kind = NativeGrantActionKind::ActivateOwl, .functionName = "SET_OWL_STATUE_ACTIVATED", .itemId = operation.itemId, .amount = operation.amount };
    }
    return std::nullopt;
}

std::string ToString(NativeGrantActionKind kind) {
    switch (kind) {
        case NativeGrantActionKind::GiveRawItem:
            return "GiveRawItem";
        case NativeGrantActionKind::GiveRandomizerItem:
            return "GiveRandomizerItem";
        case NativeGrantActionKind::ChangeRupees:
            return "ChangeRupees";
        case NativeGrantActionKind::ChangeAmmo:
            return "ChangeAmmo";
        case NativeGrantActionKind::ChangeHealth:
            return "ChangeHealth";
        case NativeGrantActionKind::AddHealthCapacity:
            return "AddHealthCapacity";
        case NativeGrantActionKind::AddMagic:
            return "AddMagic";
        case NativeGrantActionKind::AddToken:
            return "AddToken";
        case NativeGrantActionKind::SetDungeonItem:
            return "SetDungeonItem";
        case NativeGrantActionKind::AddDungeonKey:
            return "AddDungeonKey";
        case NativeGrantActionKind::SetQuestFlag:
            return "SetQuestFlag";
        case NativeGrantActionKind::ActivateOwl:
            return "ActivateOwl";
    }
    return "Unknown";
}

} // namespace ootmm
