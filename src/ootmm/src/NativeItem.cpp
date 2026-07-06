#include "ootmm/NativeItem.hpp"

#include <array>
#include <string_view>

namespace ootmm {
namespace {

struct NativeItemMapping {
    std::string_view itemId;
    NativePort port;
    Game game;
    std::string_view itemEnum;
    std::string_view getItemEnum;
    std::string_view randomizerEnum;
};

constexpr std::array<NativeItemMapping, 50> kNativeItemMappings = {{
    { "OOT_BOW", NativePort::ShipOfHarkinian, Game::Oot, "ITEM_BOW", "GI_BOW", "RG_FAIRY_BOW" },
    { "OOT_HOOKSHOT", NativePort::ShipOfHarkinian, Game::Oot, "ITEM_HOOKSHOT", "GI_HOOKSHOT", "RG_HOOKSHOT" },
    { "OOT_LONGSHOT", NativePort::ShipOfHarkinian, Game::Oot, "ITEM_LONGSHOT", "GI_LONGSHOT", "RG_LONGSHOT" },
    { "OOT_SLINGSHOT", NativePort::ShipOfHarkinian, Game::Oot, "ITEM_SLINGSHOT", "GI_SLINGSHOT", "RG_FAIRY_SLINGSHOT" },
    { "OOT_BOOMERANG", NativePort::ShipOfHarkinian, Game::Oot, "ITEM_BOOMERANG", "GI_BOOMERANG", "RG_BOOMERANG" },
    { "OOT_LENS", NativePort::ShipOfHarkinian, Game::Oot, "ITEM_LENS", "GI_LENS", "RG_LENS_OF_TRUTH" },
    { "OOT_HAMMER", NativePort::ShipOfHarkinian, Game::Oot, "ITEM_HAMMER", "GI_HAMMER", "RG_MEGATON_HAMMER" },
    { "OOT_OCARINA", NativePort::ShipOfHarkinian, Game::Oot, "ITEM_OCARINA_TIME", "GI_OCARINA_OOT", "RG_OCARINA_OF_TIME" },
    { "OOT_BOMB_BAG", NativePort::ShipOfHarkinian, Game::Oot, "ITEM_BOMB_BAG_20", "GI_BOMB_BAG_20", "RG_BOMB_BAG" },
    { "OOT_SHIELD_HYLIAN", NativePort::ShipOfHarkinian, Game::Oot, "ITEM_SHIELD_HYLIAN", "GI_SHIELD_HYLIAN", "RG_HYLIAN_SHIELD" },
    { "OOT_SHIELD_MIRROR", NativePort::ShipOfHarkinian, Game::Oot, "ITEM_SHIELD_MIRROR", "GI_SHIELD_MIRROR", "RG_MIRROR_SHIELD" },
    { "OOT_BOOTS_IRON", NativePort::ShipOfHarkinian, Game::Oot, "ITEM_BOOTS_IRON", "GI_BOOTS_IRON", "RG_IRON_BOOTS" },
    { "OOT_BOOTS_HOVER", NativePort::ShipOfHarkinian, Game::Oot, "ITEM_BOOTS_HOVER", "GI_BOOTS_HOVER", "RG_HOVER_BOOTS" },

    { "MM_BOW", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_BOW", "GI_QUIVER_30", "RI_BOW" },
    { "MM_HOOKSHOT", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_HOOKSHOT", "GI_HOOKSHOT", "RI_HOOKSHOT" },
    { "MM_OCARINA", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_OCARINA_OF_TIME", "GI_OCARINA_OF_TIME", "RI_OCARINA" },
    { "MM_LENS", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_LENS_OF_TRUTH", "GI_LENS_OF_TRUTH", "RI_LENS" },
    { "MM_BOMB_BAG", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_BOMB_BAG_20", "GI_BOMB_BAG_20", "RI_BOMB_BAG_20" },
    { "MM_SHIELD_HERO", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_SHIELD_HERO", "GI_SHIELD_HERO", "RI_SHIELD_HERO" },
    { "MM_SHIELD_MIRROR", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_SHIELD_MIRROR", "GI_SHIELD_MIRROR", "RI_SHIELD_MIRROR" },
    { "MM_GREAT_FAIRY_SWORD", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_SWORD_GREAT_FAIRY", "GI_SWORD_GREAT_FAIRY", "RI_GREAT_FAIRY_SWORD" },
    { "MM_BOTTLE_CHATEAU", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_CHATEAU", "GI_CHATEAU", "RI_BOTTLE_CHATEAU_ROMANI" },
    { "MM_BOTTLE_EMPTY", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_BOTTLE", "GI_BOTTLE", "RI_BOTTLE_EMPTY" },
    { "MM_BOTTLED_GOLD_DUST", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_GOLD_DUST", "GI_GOLD_DUST", "RI_BOTTLE_GOLD_DUST" },
    { "MM_BOTTLE_MILK", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_MILK_BOTTLE", "GI_MILK_BOTTLE", "RI_BOTTLE_MILK" },
    { "MM_BOTTLE_POTION_RED", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_POTION_RED", "GI_POTION_RED_BOTTLE", "RI_BOTTLE_RED_POTION" },
    { "MM_MASK_ALL_NIGHT", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_MASK_ALL_NIGHT", "GI_MASK_ALL_NIGHT", "RI_MASK_ALL_NIGHT" },
    { "MM_MASK_BLAST", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_MASK_BLAST", "GI_MASK_BLAST", "RI_MASK_BLAST" },
    { "MM_MASK_BREMEN", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_MASK_BREMEN", "GI_MASK_BREMEN", "RI_MASK_BREMEN" },
    { "MM_MASK_BUNNY", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_MASK_BUNNY", "GI_MASK_BUNNY", "RI_MASK_BUNNY" },
    { "MM_MASK_CAPTAIN", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_MASK_CAPTAIN", "GI_MASK_CAPTAIN", "RI_MASK_CAPTAIN" },
    { "MM_MASK_TROUPE_LEADER", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_MASK_CIRCUS_LEADER", "GI_MASK_CIRCUS_LEADER", "RI_MASK_CIRCUS_LEADER" },
    { "MM_MASK_COUPLE", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_MASK_COUPLE", "GI_MASK_COUPLE", "RI_MASK_COUPLE" },
    { "MM_MASK_DEKU", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_MASK_DEKU", "GI_MASK_DEKU", "RI_MASK_DEKU" },
    { "MM_MASK_DON_GERO", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_MASK_DON_GERO", "GI_MASK_DON_GERO", "RI_MASK_DON_GERO" },
    { "MM_MASK_FIERCE_DEITY", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_MASK_FIERCE_DEITY", "GI_MASK_FIERCE_DEITY", "RI_MASK_FIERCE_DEITY" },
    { "MM_MASK_GARO", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_MASK_GARO", "GI_MASK_GARO", "RI_MASK_GARO" },
    { "MM_MASK_GIANT", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_MASK_GIANT", "GI_MASK_GIANT", "RI_MASK_GIANT" },
    { "MM_MASK_GIBDO", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_MASK_GIBDO", "GI_MASK_GIBDO", "RI_MASK_GIBDO" },
    { "MM_MASK_GORON", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_MASK_GORON", "GI_MASK_GORON", "RI_MASK_GORON" },
    { "MM_MASK_GREAT_FAIRY", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_MASK_GREAT_FAIRY", "GI_MASK_GREAT_FAIRY", "RI_MASK_GREAT_FAIRY" },
    { "MM_MASK_KAFEI", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_MASK_KAFEIS_MASK", "GI_MASK_KAFEIS_MASK", "RI_MASK_KAFEIS_MASK" },
    { "MM_MASK_KAMARO", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_MASK_KAMARO", "GI_MASK_KAMARO", "RI_MASK_KAMARO" },
    { "MM_MASK_KEATON", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_MASK_KEATON", "GI_MASK_KEATON", "RI_MASK_KEATON" },
    { "MM_MASK_POSTMAN", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_MASK_POSTMAN", "GI_MASK_POSTMAN", "RI_MASK_POSTMAN" },
    { "MM_MASK_ROMANI", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_MASK_ROMANI", "GI_MASK_ROMANI", "RI_MASK_ROMANI" },
    { "MM_MASK_SCENTS", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_MASK_SCENTS", "GI_MASK_SCENTS", "RI_MASK_SCENTS" },
    { "MM_MASK_STONE", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_MASK_STONE", "GI_MASK_STONE", "RI_MASK_STONE" },
    { "MM_MASK_TRUTH", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_MASK_TRUTH", "GI_MASK_TRUTH", "RI_MASK_TRUTH" },
    { "MM_MASK_ZORA", NativePort::TwoShip2Harkinian, Game::Mm, "ITEM_MASK_ZORA", "GI_MASK_ZORA", "RI_MASK_ZORA" },
}};

} // namespace

std::optional<NativeItemSymbol> ResolveNativeItemSymbol(const GrantOperation& operation, NativePort port) {
    if (operation.kind != GrantOperationKind::RawItem) {
        return std::nullopt;
    }

    for (const NativeItemMapping& mapping : kNativeItemMappings) {
        if (mapping.itemId == operation.itemId && mapping.port == port && mapping.game == operation.targetGame) {
            return NativeItemSymbol{
                .port = port,
                .game = operation.targetGame,
                .itemId = operation.itemId,
                .itemEnum = std::string(mapping.itemEnum),
                .getItemEnum = std::string(mapping.getItemEnum),
                .randomizerEnum = std::string(mapping.randomizerEnum),
            };
        }
    }

    return std::nullopt;
}

std::optional<NativeItemSymbol> ResolveNativeItemSymbol(const ItemRef& item, Game activeGame, NativePort port) {
    const std::vector<GrantOperation> operations = ResolveGrantOperations(item, activeGame);
    if (operations.empty()) {
        return std::nullopt;
    }
    return ResolveNativeItemSymbol(operations.front(), port);
}

std::string ToString(NativePort port) {
    switch (port) {
        case NativePort::ShipOfHarkinian:
            return "ShipOfHarkinian";
        case NativePort::TwoShip2Harkinian:
            return "TwoShip2Harkinian";
    }
    return "Unknown";
}

} // namespace ootmm
