#include "ootmm/AnchorBridge.hpp"
#include "ootmm/Camc.hpp"
#include "ootmm/FilePacketBridge.hpp"
#include "ootmm/GrantApplier.hpp"
#include "ootmm/ItemGrant.hpp"
#include "ootmm/NativeGrantAction.hpp"
#include "ootmm/NativeItem.hpp"
#include "ootmm/NativePortSession.hpp"
#include "ootmm/PortBridge.hpp"
#include "ootmm/Runtime.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

const char* kSeedJson = R"json(
{
  "format": "ootmm.pc.seed.v1",
  "seedId": "test-seed",
  "settingsHash": "abc123",
  "playerId": 1,
  "playerCount": 2,
  "teamId": "default",
  "startingItems": [
    { "itemGame": "oot", "itemId": "OOT_DEKU_STICK", "itemName": "Deku Stick", "count": 2 }
  ],
  "placements": [
    {
      "checkGame": "oot",
      "checkId": "OOT_KF_MIDOS_TOP_LEFT",
      "checkName": "Mido Top Left",
      "checkAliases": ["OOT_KF_MIDOS_TOP_LEFT_CHEST", "RC_KF_MIDOS_TOP_LEFT_CHEST"],
      "itemGame": "mm",
      "itemId": "MM_BOW",
      "itemName": "Hero's Bow",
      "ownerPlayer": 1,
      "sourcePlayer": 1,
      "major": true
    },
    {
      "checkGame": "mm",
      "checkId": "MM_CLOCK_TOWN_CHEST",
      "checkName": "Clock Town Chest",
      "itemGame": "oot",
      "itemId": "OOT_HOOKSHOT",
      "itemName": "Hookshot",
      "ownerPlayer": 2,
      "sourcePlayer": 1,
      "major": true
    }
  ],
  "entrances": [
    { "fromGame": "oot", "from": "OOT_TEMPLE_OF_TIME_DOOR", "fromNativeId": 604, "toGame": "mm", "to": "MM_CLOCK_TOWER", "toNativeId": 49168 }
  ]
}
)json";

void TestSeedLoad() {
    const ootmm::Seed seed = ootmm::LoadSeedFromJson(kSeedJson);
    assert(seed.IsCompatible());
    assert(seed.playerId == 1);
    assert(seed.playerCount == 2);
    assert(seed.placements.size() == 2);
    assert(seed.entrances.size() == 1);
    assert(seed.startingItems.size() == 1);
    assert(seed.FindPlacement(ootmm::Game::Oot, "OOT_KF_MIDOS_TOP_LEFT") != nullptr);
    const ootmm::EntranceMapping* entrance = seed.FindEntrance(ootmm::Game::Oot, "OOT_TEMPLE_OF_TIME_DOOR");
    assert(entrance != nullptr);
    assert(entrance->fromNativeId.has_value());
    assert(*entrance->fromNativeId == 604);
    assert(entrance->toGame == ootmm::Game::Mm);
    assert(entrance->to == "MM_CLOCK_TOWER");
    assert(entrance->toNativeId.has_value());
    assert(*entrance->toNativeId == 49168);
    assert(seed.FindEntrance(ootmm::Game::Oot, 604) == entrance);
    assert(seed.FindEntrance(ootmm::Game::Mm, "OOT_TEMPLE_OF_TIME_DOOR") == nullptr);
    assert(seed.FindEntrance(ootmm::Game::Oot, 49168) == nullptr);
}

void TestRuntimeLocalAndRemoteChecks() {
    ootmm::Runtime runtime;
    runtime.LoadSeed(ootmm::LoadSeedFromJson(kSeedJson));

    assert(runtime.QueuedItemCount() == 2);
    assert(runtime.PopQueuedItem()->item.id == "OOT_DEKU_STICK");
    assert(runtime.PopQueuedItem()->item.id == "OOT_DEKU_STICK");

    auto localEvents = runtime.CompleteCheck(ootmm::Game::Oot, "OOT_KF_MIDOS_TOP_LEFT");
    assert(localEvents.size() == 1);
    assert(localEvents[0].targetPlayer == 1);
    assert(runtime.IsCheckCompleted(ootmm::Game::Oot, "OOT_KF_MIDOS_TOP_LEFT"));
    assert(runtime.IsCheckCompleted(ootmm::Game::Oot, "Mido Top Left"));
    // The OoT check holds an MM item (the core combo cross-game mechanic): it must NOT
    // be delivered while OoT is the active game — only one engine runs at a time — so
    // nothing is queued locally here.
    assert(runtime.QueuedItemCount() == 0);
    // When MM next boots, the pending cross-game item is queued for delivery in MM,
    // exactly once (idempotent across reboots/handoffs).
    assert(runtime.RegrantCrossGamePendingItems(ootmm::Game::Mm) == 1);
    assert(runtime.QueuedItemCount() == 1);
    assert(runtime.PopQueuedItem()->item.id == "MM_BOW");
    assert(runtime.RegrantCrossGamePendingItems(ootmm::Game::Mm) == 0);
    // Booting OoT does not re-deliver an OoT-bound item that lives in an OoT check
    // (none here) nor the MM-bound item (wrong game).
    assert(runtime.RegrantCrossGamePendingItems(ootmm::Game::Oot) == 0);

    auto duplicateEvents = runtime.CompleteCheck(ootmm::Game::Oot, "OOT_KF_MIDOS_TOP_LEFT_CHEST");
    assert(duplicateEvents.empty());

    auto remoteEvents = runtime.CompleteCheck(ootmm::Game::Mm, "MM_CLOCK_TOWN_CHEST");
    assert(remoteEvents.size() == 1);
    assert(remoteEvents[0].targetPlayer == 2);
    assert(runtime.QueuedItemCount() == 0);

    const ootmm::EntranceMapping* entrance = runtime.FindEntrance(ootmm::Game::Oot, "OOT_TEMPLE_OF_TIME_DOOR");
    assert(entrance != nullptr);
    assert(entrance->toGame == ootmm::Game::Mm);
    assert(entrance->to == "MM_CLOCK_TOWER");
    entrance = runtime.FindEntrance(ootmm::Game::Oot, 604);
    assert(entrance != nullptr);
    assert(entrance->toNativeId.has_value());
    assert(*entrance->toNativeId == 49168);
}

void TestAnchorGiveItemRoundTrip() {
    const ootmm::Seed seed = ootmm::LoadSeedFromJson(kSeedJson);
    ootmm::NetworkEvent event{
        .type = ootmm::NetworkEventType::GiveItem,
        .targetPlayer = 1,
        .sourcePlayer = 2,
        .checkId = "REMOTE_CHECK",
        .checkGame = ootmm::Game::Mm,
        .item = {
            .game = ootmm::Game::Oot,
            .id = "OOT_BOW",
            .name = "Fairy Bow",
        },
        .deliveryKey = "mm:REMOTE_CHECK->p1:OOT_BOW",
    };

    const std::string packet = ootmm::anchor::BuildGiveItemPacket(seed, event);
    const auto parsed = ootmm::anchor::ParseGiveItemPacket(packet);
    assert(parsed.has_value());
    assert(parsed->targetPlayer == 1);
    assert(parsed->sourcePlayer == 2);
    assert(parsed->item.id == "OOT_BOW");

    ootmm::Runtime runtime;
    runtime.LoadSeed(seed);
    (void)runtime.PopQueuedItem();
    (void)runtime.PopQueuedItem();
    assert(runtime.ReceiveRemoteItem(*parsed));
    assert(!runtime.ReceiveRemoteItem(*parsed));
    assert(runtime.PopQueuedItem()->item.id == "OOT_BOW");
}

void TestAnchorEventPacketRouting() {
    const ootmm::Seed seed = ootmm::LoadSeedFromJson(kSeedJson);
    ootmm::Runtime runtime;
    runtime.LoadSeed(seed);
    (void)runtime.PopQueuedItem();
    (void)runtime.PopQueuedItem();

    const auto localEvents = runtime.CompleteCheck(ootmm::Game::Oot, "OOT_KF_MIDOS_TOP_LEFT");
    assert(localEvents.size() == 1);
    const auto localPackets = ootmm::anchor::BuildPacketsForEvent(seed, localEvents[0]);
    assert(localPackets.size() == 1);
    assert(localPackets[0].find("\"type\":\"OOTMM_CHECK_COMPLETE\"") != std::string::npos);
    assert(localPackets[0].find("\"targetPlayer\":1") != std::string::npos);

    const auto remoteEvents = runtime.CompleteCheck(ootmm::Game::Mm, "MM_CLOCK_TOWN_CHEST");
    assert(remoteEvents.size() == 1);
    const auto remotePackets = ootmm::anchor::BuildPacketsForEvent(seed, remoteEvents[0]);
    assert(remotePackets.size() == 2);
    assert(remotePackets[0].find("\"type\":\"OOTMM_CHECK_COMPLETE\"") != std::string::npos);
    assert(remotePackets[1].find("\"type\":\"OOTMM_GIVE_ITEM\"") != std::string::npos);

    const auto parsedGive = ootmm::anchor::ParseGiveItemPacket(remotePackets[1]);
    assert(parsedGive.has_value());
    assert(parsedGive->targetPlayer == 2);
    assert(parsedGive->sourcePlayer == 1);
    assert(parsedGive->checkId == "MM_CLOCK_TOWN_CHEST");
    assert(parsedGive->item.id == "OOT_HOOKSHOT");

    const auto ignoredPackets = ootmm::anchor::BuildPacketsForEvent(seed, {
        .type = ootmm::NetworkEventType::SyncState,
    });
    assert(ignoredPackets.empty());
}

class FakeGameAdapter final : public ootmm::IGameAdapter {
  public:
    std::vector<ootmm::ReceivedItem> granted;

    void GrantItem(const ootmm::ReceivedItem& item) override {
        granted.push_back(item);
    }
};

class FakeNetworkAdapter final : public ootmm::INetworkAdapter {
  public:
    std::vector<ootmm::NetworkEvent> sent;

    void SendEvent(const ootmm::NetworkEvent& event) override {
        sent.push_back(event);
    }
};

class FakeGrantOperationAdapter final : public ootmm::IGrantOperationAdapter {
  public:
    explicit FakeGrantOperationAdapter(ootmm::Game activeGame) : activeGame_(activeGame) {}

    std::vector<ootmm::ResolvedGrant> applied;

    ootmm::Game ActiveGame() const override {
        return activeGame_;
    }

    void ApplyGrant(const ootmm::ResolvedGrant& grant) override {
        applied.push_back(grant);
    }

  private:
    ootmm::Game activeGame_;
};

class ActionCollectingGrantAdapter final : public ootmm::IGrantOperationAdapter {
  public:
    ActionCollectingGrantAdapter(ootmm::Game activeGame, ootmm::NativePort nativePort)
        : activeGame_(activeGame), nativePort_(nativePort) {}

    std::vector<ootmm::NativeGrantAction> actions;

    ootmm::Game ActiveGame() const override {
        return activeGame_;
    }

    void ApplyGrant(const ootmm::ResolvedGrant& grant) override {
        const auto action = ootmm::ResolveNativeGrantAction(grant, nativePort_);
        if (action.has_value()) {
            actions.push_back(*action);
        }
    }

  private:
    ootmm::Game activeGame_;
    ootmm::NativePort nativePort_;
};

void TestPortBridgeAdapters() {
    FakeGameAdapter game;
    FakeNetworkAdapter network;
    ootmm::PortBridge bridge(game, network);
    bridge.LoadSeed(ootmm::LoadSeedFromJson(kSeedJson));

    assert(bridge.PumpQueuedItems() == 2);
    assert(game.granted.size() == 2);

    bridge.OnCheckCollected(ootmm::Game::Oot, "OOT_KF_MIDOS_TOP_LEFT");
    assert(network.sent.size() == 1);
    assert(network.sent[0].targetPlayer == 1);
    // The OoT check holds an MM item: with OoT active it is held cross-game, not
    // delivered now (only one engine runs at a time).
    assert(bridge.PumpQueuedItems() == 0);
    // It is delivered when MM boots and re-grants pending cross-game items.
    assert(bridge.GetRuntime().RegrantCrossGamePendingItems(ootmm::Game::Mm) == 1);
    assert(bridge.PumpQueuedItems() == 1);
    assert(game.granted.back().item.id == "MM_BOW");

    bridge.OnCheckCollected(ootmm::Game::Mm, "MM_CLOCK_TOWN_CHEST");
    assert(network.sent.size() == 2);
    assert(network.sent[1].targetPlayer == 2);
    assert(bridge.PumpQueuedItems() == 0);
}

void TestGrantOperationResolver() {
    {
        const auto ops = ootmm::ResolveGrantOperations({ .game = ootmm::Game::Oot, .id = "OOT_RUPEE_RED" }, ootmm::Game::Oot);
        assert(ops.size() == 1);
        assert(ops[0].kind == ootmm::GrantOperationKind::AddRupees);
        assert(ops[0].targetGame == ootmm::Game::Oot);
        assert(ops[0].amount == 20);
    }
    {
        const auto ops = ootmm::ResolveGrantOperations({ .game = ootmm::Game::Mm, .id = "MM_ARROWS_30" }, ootmm::Game::Mm);
        assert(ops.size() == 1);
        assert(ops[0].kind == ootmm::GrantOperationKind::AddAmmo);
        assert(ops[0].targetGame == ootmm::Game::Mm);
        assert(ops[0].slot == "BOW");
        assert(ops[0].amount == 30);
    }
    {
        const auto ops = ootmm::ResolveGrantOperations({ .game = ootmm::Game::Oot, .id = "SHARED_BOW" }, ootmm::Game::Mm);
        assert(ops.size() == 1);
        assert(ops[0].kind == ootmm::GrantOperationKind::RawItem);
        assert(ops[0].targetGame == ootmm::Game::Mm);
        assert(ops[0].itemId == "MM_BOW");
    }
    {
        // SHARED_* items NOT in the explicit table resolve programmatically (SHARED_<X> -> the active
        // game's OOT_/MM_<X>). These previously fell through as a raw "SHARED_" id and were dropped,
        // soft-locking soul-gated enemies and ocarina note input.
        const auto soulOot =
            ootmm::ResolveGrantOperations({ .game = ootmm::Game::Oot, .id = "SHARED_SOUL_ENEMY_OCTOROK" }, ootmm::Game::Oot);
        assert(soulOot.size() == 1 && soulOot[0].itemId == "OOT_SOUL_ENEMY_OCTOROK");
        assert(soulOot[0].targetGame == ootmm::Game::Oot);

        const auto buttonMm =
            ootmm::ResolveGrantOperations({ .game = ootmm::Game::Mm, .id = "SHARED_BUTTON_C_LEFT" }, ootmm::Game::Mm);
        assert(buttonMm.size() == 1 && buttonMm[0].itemId == "MM_BUTTON_C_LEFT");

        // Scale resolves through the table fast-path identically.
        const auto scaleOot =
            ootmm::ResolveGrantOperations({ .game = ootmm::Game::Oot, .id = "SHARED_SCALE" }, ootmm::Game::Oot);
        assert(scaleOot.size() == 1 && scaleOot[0].itemId == "OOT_SCALE");
    }
    {
        const auto ops = ootmm::ResolveGrantOperations({ .game = ootmm::Game::Oot, .id = "OOT_FOREST_TEMPLE_BOSS_KEY" }, ootmm::Game::Oot);
        assert(ops.size() == 1);
        assert(ops[0].kind == ootmm::GrantOperationKind::AddDungeonItem);
    }
    {
        const auto ops = ootmm::ResolveGrantOperations({ .game = ootmm::Game::Oot, .id = "OOT_BOSS_KEY_FOREST" }, ootmm::Game::Oot);
        assert(ops.size() == 1);
        assert(ops[0].kind == ootmm::GrantOperationKind::AddDungeonItem);
    }
    {
        const auto ops = ootmm::ResolveGrantOperations({ .game = ootmm::Game::Mm, .id = "MM_MAP_WF" }, ootmm::Game::Mm);
        assert(ops.size() == 1);
        assert(ops[0].kind == ootmm::GrantOperationKind::AddDungeonItem);
    }
    {
        const auto ops = ootmm::ResolveGrantOperations({ .game = ootmm::Game::Mm, .id = "MM_WORLD_MAP_WOODFALL" }, ootmm::Game::Mm);
        assert(ops.size() == 1);
        assert(ops[0].kind == ootmm::GrantOperationKind::RawItem);
    }
    {
        const auto ops = ootmm::ResolveGrantOperations({ .game = ootmm::Game::Mm, .id = "MM_SONG_SOARING" }, ootmm::Game::Mm);
        assert(ops.size() == 1);
        assert(ops[0].kind == ootmm::GrantOperationKind::SetQuestFlag);
    }
}

void TestNativeItemSymbols() {
    {
        const auto symbol = ootmm::ResolveNativeItemSymbol({ .game = ootmm::Game::Oot, .id = "OOT_BOW" }, ootmm::Game::Oot, ootmm::NativePort::ShipOfHarkinian);
        assert(symbol.has_value());
        assert(symbol->itemEnum == "ITEM_BOW");
        assert(symbol->getItemEnum == "GI_BOW");
        assert(symbol->randomizerEnum == "RG_FAIRY_BOW");
    }
    {
        const auto symbol = ootmm::ResolveNativeItemSymbol({ .game = ootmm::Game::Oot, .id = "SHARED_BOW" }, ootmm::Game::Mm, ootmm::NativePort::TwoShip2Harkinian);
        assert(symbol.has_value());
        assert(symbol->itemId == "MM_BOW");
        assert(symbol->itemEnum == "ITEM_BOW");
        assert(symbol->getItemEnum == "GI_QUIVER_30");
        assert(symbol->randomizerEnum == "RI_BOW");
    }
    {
        const auto symbol = ootmm::ResolveNativeItemSymbol({ .game = ootmm::Game::Oot, .id = "OOT_RUPEE_RED" }, ootmm::Game::Oot, ootmm::NativePort::ShipOfHarkinian);
        assert(!symbol.has_value());
    }
    {
        const auto symbol = ootmm::ResolveNativeItemSymbol({ .game = ootmm::Game::Mm, .id = "MM_MASK_BUNNY" }, ootmm::Game::Mm, ootmm::NativePort::TwoShip2Harkinian);
        assert(symbol.has_value());
        assert(symbol->itemEnum == "ITEM_MASK_BUNNY");
        assert(symbol->randomizerEnum == "RI_MASK_BUNNY");
    }
    {
        const auto symbol = ootmm::ResolveNativeItemSymbol({ .game = ootmm::Game::Mm, .id = "MM_BOTTLE_EMPTY" }, ootmm::Game::Mm, ootmm::NativePort::TwoShip2Harkinian);
        assert(symbol.has_value());
        assert(symbol->itemEnum == "ITEM_BOTTLE");
        assert(symbol->randomizerEnum == "RI_BOTTLE_EMPTY");
    }
}

void TestResolvingGameAdapter() {
    FakeGrantOperationAdapter operations(ootmm::Game::Mm);
    ootmm::ResolvingGameAdapter game(operations, ootmm::NativePort::TwoShip2Harkinian);
    FakeNetworkAdapter network;
    ootmm::PortBridge bridge(game, network);
    bridge.LoadSeed(ootmm::LoadSeedFromJson(kSeedJson));

    bridge.OnRemoteItem(ootmm::NetworkEvent{
        .type = ootmm::NetworkEventType::GiveItem,
        .targetPlayer = 1,
        .sourcePlayer = 2,
        .checkId = "REMOTE_SHARED_BOW",
        .checkGame = ootmm::Game::Oot,
        .item = {
            .game = ootmm::Game::Oot,
            .id = "SHARED_BOW",
            .name = "Bow",
        },
        .deliveryKey = "remote-shared-bow",
    });

    assert(bridge.PumpQueuedItems() == 3);
    assert(operations.applied.size() == 3);
    assert(operations.applied[0].operation.itemId == "OOT_DEKU_STICK");
    assert(operations.applied[2].operation.kind == ootmm::GrantOperationKind::RawItem);
    assert(operations.applied[2].operation.itemId == "MM_BOW");
    assert(operations.applied[2].nativeSymbol.has_value());
    assert(operations.applied[2].nativeSymbol->randomizerEnum == "RI_BOW");
}

void TestNativeGrantActions() {
    {
        const ootmm::GrantOperation op{ .kind = ootmm::GrantOperationKind::RawItem, .targetGame = ootmm::Game::Mm, .itemId = "MM_BOW" };
        const auto symbol = ootmm::ResolveNativeItemSymbol(op, ootmm::NativePort::TwoShip2Harkinian);
        const auto action = ootmm::ResolveNativeGrantAction({
            .delivery = {},
            .operation = op,
            .nativeSymbol = symbol,
        }, ootmm::NativePort::TwoShip2Harkinian);
        assert(action.has_value());
        assert(action->kind == ootmm::NativeGrantActionKind::GiveRandomizerItem);
        assert(action->functionName == "Rando::GiveItem");
        assert(action->primarySymbol == "RI_BOW");
    }
    {
        const ootmm::GrantOperation op{ .kind = ootmm::GrantOperationKind::RawItem, .targetGame = ootmm::Game::Mm, .itemId = "MM_MASK_BUNNY" };
        const auto symbol = ootmm::ResolveNativeItemSymbol(op, ootmm::NativePort::TwoShip2Harkinian);
        const auto action = ootmm::ResolveNativeGrantAction({
            .delivery = {},
            .operation = op,
            .nativeSymbol = symbol,
        }, ootmm::NativePort::TwoShip2Harkinian);
        assert(action.has_value());
        assert(action->kind == ootmm::NativeGrantActionKind::GiveRandomizerItem);
        assert(action->functionName == "Rando::GiveItem");
        assert(action->primarySymbol == "RI_MASK_BUNNY");
    }
    {
        const ootmm::GrantOperation op{ .kind = ootmm::GrantOperationKind::AddAmmo, .targetGame = ootmm::Game::Oot, .itemId = "OOT_ARROWS_30", .slot = "BOW", .amount = 30 };
        const auto action = ootmm::ResolveNativeGrantAction({ .delivery = {}, .operation = op }, ootmm::NativePort::ShipOfHarkinian);
        assert(action.has_value());
        assert(action->kind == ootmm::NativeGrantActionKind::ChangeAmmo);
        assert(action->functionName == "Inventory_ChangeAmmo");
        assert(action->primarySymbol == "ITEM_BOW");
        assert(action->amount == 30);
    }
    {
        const ootmm::GrantOperation op{ .kind = ootmm::GrantOperationKind::AddDungeonItem, .targetGame = ootmm::Game::Oot, .itemId = "OOT_BOSS_KEY_FOREST" };
        const auto action = ootmm::ResolveNativeGrantAction({ .delivery = {}, .operation = op }, ootmm::NativePort::ShipOfHarkinian);
        assert(action.has_value());
        assert(action->kind == ootmm::NativeGrantActionKind::SetDungeonItem);
        assert(action->primarySymbol == "DUNGEON_KEY_BOSS");
        assert(action->secondarySymbol == "SCENE_FOREST_TEMPLE");
    }
    {
        const ootmm::GrantOperation op{ .kind = ootmm::GrantOperationKind::AddDungeonItem, .targetGame = ootmm::Game::Mm, .itemId = "MM_MAP_WF" };
        const auto action = ootmm::ResolveNativeGrantAction({ .delivery = {}, .operation = op }, ootmm::NativePort::TwoShip2Harkinian);
        assert(action.has_value());
        assert(action->kind == ootmm::NativeGrantActionKind::SetDungeonItem);
        assert(action->primarySymbol == "DUNGEON_MAP");
        assert(action->secondarySymbol == "DUNGEON_SCENE_INDEX_WOODFALL_TEMPLE");
    }
    {
        const ootmm::GrantOperation op{ .kind = ootmm::GrantOperationKind::SetQuestFlag, .targetGame = ootmm::Game::Mm, .itemId = "MM_SONG_SOARING" };
        const auto action = ootmm::ResolveNativeGrantAction({ .delivery = {}, .operation = op }, ootmm::NativePort::TwoShip2Harkinian);
        assert(action.has_value());
        assert(action->kind == ootmm::NativeGrantActionKind::SetQuestFlag);
        assert(action->primarySymbol == "QUEST_SONG_SOARING");
    }
    {
        // Heart capacity is granted in healthCapacity units (16 = one heart), matching
        // AddHealth. A heart piece is +4 (a quarter heart); a heart container is +16
        // (a full heart); four pieces equal exactly one container. Regression test for
        // the old 4x over-grant (amount was multiplied by 16 on already-unit amounts).
        const ootmm::ItemRef piece{ .game = ootmm::Game::Oot, .id = "OOT_HEART_PIECE", .name = "Piece of Heart" };
        const auto pieceOps = ootmm::ResolveGrantOperations(piece, ootmm::Game::Oot);
        assert(pieceOps.size() == 1);
        assert(pieceOps[0].kind == ootmm::GrantOperationKind::AddHealthCapacity);
        const auto pieceAction = ootmm::ResolveNativeGrantAction({ .delivery = {}, .operation = pieceOps[0] }, ootmm::NativePort::ShipOfHarkinian);
        assert(pieceAction.has_value());
        assert(pieceAction->kind == ootmm::NativeGrantActionKind::AddHealthCapacity);
        assert(pieceAction->amount == 4);

        const ootmm::ItemRef container{ .game = ootmm::Game::Oot, .id = "OOT_HEART_CONTAINER", .name = "Heart Container" };
        const auto containerOps = ootmm::ResolveGrantOperations(container, ootmm::Game::Oot);
        assert(containerOps.size() == 1);
        assert(containerOps[0].kind == ootmm::GrantOperationKind::AddHealthCapacity);
        const auto containerAction = ootmm::ResolveNativeGrantAction({ .delivery = {}, .operation = containerOps[0] }, ootmm::NativePort::TwoShip2Harkinian);
        assert(containerAction.has_value());
        assert(containerAction->kind == ootmm::NativeGrantActionKind::AddHealthCapacity);
        assert(containerAction->amount == 16);

        assert(pieceAction->amount * 4 == containerAction->amount);
    }
}

void TestPortBridgeToNativeGrantActions() {
    ActionCollectingGrantAdapter operations(ootmm::Game::Mm, ootmm::NativePort::TwoShip2Harkinian);
    ootmm::ResolvingGameAdapter game(operations, ootmm::NativePort::TwoShip2Harkinian);
    FakeNetworkAdapter network;
    ootmm::PortBridge bridge(game, network);
    bridge.LoadSeed(ootmm::LoadSeedFromJson(kSeedJson));

    assert(bridge.PumpQueuedItems() == 2);
    assert(operations.actions.empty());

    bridge.OnRemoteItem(ootmm::NetworkEvent{
        .type = ootmm::NetworkEventType::GiveItem,
        .targetPlayer = 1,
        .sourcePlayer = 2,
        .checkId = "REMOTE_RED_RUPEE",
        .checkGame = ootmm::Game::Oot,
        .item = {
            .game = ootmm::Game::Mm,
            .id = "MM_RUPEE_RED",
            .name = "Red Rupee",
        },
        .deliveryKey = "remote-red-rupee",
    });
    bridge.OnRemoteItem(ootmm::NetworkEvent{
        .type = ootmm::NetworkEventType::GiveItem,
        .targetPlayer = 1,
        .sourcePlayer = 2,
        .checkId = "REMOTE_SOARING",
        .checkGame = ootmm::Game::Oot,
        .item = {
            .game = ootmm::Game::Mm,
            .id = "MM_SONG_SOARING",
            .name = "Song of Soaring",
        },
        .deliveryKey = "remote-soaring",
    });

    assert(bridge.PumpQueuedItems() == 2);
    assert(operations.actions.size() == 2);
    assert(operations.actions[0].kind == ootmm::NativeGrantActionKind::ChangeRupees);
    assert(operations.actions[0].functionName == "Rupees_ChangeBy");
    assert(operations.actions[0].amount == 20);
    assert(operations.actions[1].kind == ootmm::NativeGrantActionKind::SetQuestFlag);
    assert(operations.actions[1].functionName == "SET_QUEST_ITEM");
    assert(operations.actions[1].primarySymbol == "QUEST_SONG_SOARING");
}

void TestNativePortSession() {
    ActionCollectingGrantAdapter operations(ootmm::Game::Oot, ootmm::NativePort::ShipOfHarkinian);
    FakeNetworkAdapter network;
    ootmm::NativePortSession session(operations, network, ootmm::NativePort::ShipOfHarkinian);

    assert(!session.HasSeed());
    session.OnCheckCollected(ootmm::Game::Oot, "OOT_KF_MIDOS_TOP_LEFT");
    assert(network.sent.empty());
    assert(session.PumpQueuedItems() == 0);
    assert(!session.OnRemoteGiveItemJson("{}"));

    const ootmm::Seed seed = ootmm::LoadSeedFromJson(kSeedJson);
    session.LoadSeed(seed);
    assert(session.HasSeed());
    assert(session.GetRuntime().QueuedItemCount() == 2);
    const ootmm::EntranceMapping* entrance = session.FindEntranceOverride(ootmm::Game::Oot, 604);
    assert(entrance != nullptr);
    assert(entrance->toGame == ootmm::Game::Mm);
    assert(entrance->toNativeId.has_value());
    assert(*entrance->toNativeId == 49168);
    assert(session.FindEntranceOverride(ootmm::Game::Mm, 604) == nullptr);
    assert(session.PumpQueuedItems() == 2);
    assert(operations.actions.empty());

    session.OnCheckCollected(ootmm::Game::Oot, "OOT_KF_MIDOS_TOP_LEFT");
    assert(network.sent.size() == 1);
    assert(network.sent[0].targetPlayer == 1);
    // The collected OoT check holds an MM item; with OoT active it is held for MM
    // (not queued locally), so only the remote OoT arrows below get pumped here.

    const ootmm::NetworkEvent remoteEvent{
        .type = ootmm::NetworkEventType::GiveItem,
        .targetPlayer = 1,
        .sourcePlayer = 2,
        .checkId = "REMOTE_ARROWS",
        .checkGame = ootmm::Game::Mm,
        .item = {
            .game = ootmm::Game::Oot,
            .id = "OOT_ARROWS_30",
            .name = "Arrows",
        },
        .deliveryKey = "remote-arrows",
    };
    const std::string packet = ootmm::anchor::BuildGiveItemPacket(seed, remoteEvent);
    assert(session.OnRemoteGiveItemJson(packet));
    assert(!session.OnRemoteGiveItemJson(packet));

    assert(session.PumpQueuedItems() == 1);
    assert(operations.actions.size() == 1);
    assert(operations.actions.back().kind == ootmm::NativeGrantActionKind::ChangeAmmo);
    assert(operations.actions.back().functionName == "Inventory_ChangeAmmo");
    assert(operations.actions.back().primarySymbol == "ITEM_BOW");
    assert(operations.actions.back().amount == 30);
}

void TestCrossGamePendingDelivery() {
    // A self-owned OoT check holding an MM item must survive a save-and-relaunch
    // cross-game handoff: collected in OoT (held, not delivered), then delivered
    // exactly once when MM boots, and never re-delivered on subsequent MM boots.
    ootmm::Runtime oot;
    oot.LoadSeed(ootmm::LoadSeedFromJson(kSeedJson));
    (void)oot.PopQueuedItem(); // drain the 2 starting deku sticks
    (void)oot.PopQueuedItem();

    (void)oot.CompleteCheck(ootmm::Game::Oot, "OOT_KF_MIDOS_TOP_LEFT");
    assert(oot.QueuedItemCount() == 0);           // MM item not delivered in OoT
    // Booting OoT again never surfaces it (wrong engine).
    assert(oot.RegrantCrossGamePendingItems(ootmm::Game::Oot) == 0);

    const std::string handoffState = oot.SerializeState();

    // --- relaunch as MM ---
    ootmm::Runtime mm;
    mm.LoadSeed(ootmm::LoadSeedFromJson(kSeedJson));
    mm.RestoreState(handoffState);
    assert(mm.IsCheckCompleted(ootmm::Game::Oot, "OOT_KF_MIDOS_TOP_LEFT"));
    assert(mm.RegrantCrossGamePendingItems(ootmm::Game::Mm) == 1);
    assert(mm.PopQueuedItem()->item.id == "MM_BOW");
    assert(mm.RegrantCrossGamePendingItems(ootmm::Game::Mm) == 0); // idempotent in-session

    // --- relaunch as MM a second time: the delivery key persisted, so no re-grant ---
    const std::string secondState = mm.SerializeState();
    ootmm::Runtime mm2;
    mm2.LoadSeed(ootmm::LoadSeedFromJson(kSeedJson));
    mm2.RestoreState(secondState);
    assert(mm2.RegrantCrossGamePendingItems(ootmm::Game::Mm) == 0);
    assert(mm2.QueuedItemCount() == 0);
}

void TestFilePacketBridge() {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "ootmm_core_file_packet_bridge_test.jsonl";
    std::filesystem::remove(path);

    assert(ootmm::AppendPacketLines(path, {}) == 0);
    assert(ootmm::AppendPacketLines(path, { "{\"type\":\"A\"}", "", "{\"type\":\"B\"}" }) == 2);

    auto firstRead = ootmm::ReadPacketLines(path, 0);
    assert(firstRead.packets.size() == 2);
    assert(firstRead.packets[0] == "{\"type\":\"A\"}");
    assert(firstRead.packets[1] == "{\"type\":\"B\"}");
    assert(firstRead.nextOffset > 0);

    assert(ootmm::ReadPacketLines(path, firstRead.nextOffset).packets.empty());
    assert(ootmm::AppendPacketLines(path, { "{\"type\":\"C\"}" }) == 1);

    auto secondRead = ootmm::ReadPacketLines(path, firstRead.nextOffset);
    assert(secondRead.packets.size() == 1);
    assert(secondRead.packets[0] == "{\"type\":\"C\"}");
    assert(secondRead.nextOffset > firstRead.nextOffset);

    std::filesystem::remove(path);
    assert(ootmm::AppendPacketLines(path, { "{\"type\":\"D\"}" }) == 1);
    auto resetRead = ootmm::ReadPacketLines(path, secondRead.nextOffset);
    assert(resetRead.packets.size() == 1);
    assert(resetRead.packets[0] == "{\"type\":\"D\"}");

    std::filesystem::remove(path);
}

void TestCrossGameAndState() {
    using namespace ootmm;

    // Explicit startingGame field.
    {
        Seed seed = LoadSeedFromJson(R"json({"format":"ootmm.pc.seed.v1","seedId":"s","startingGame":"mm"})json");
        assert(seed.startingGame.has_value() && *seed.startingGame == Game::Mm);
        assert(seed.ResolveStartingGame() == Game::Mm);
    }

    // Inferred starting game: all-MM starting items => Mm.
    {
        Seed seed = LoadSeedFromJson(
            R"json({"format":"ootmm.pc.seed.v1","seedId":"s","startingItems":[{"itemGame":"mm","itemId":"MM_KOKIRI_SWORD","count":1}]})json");
        assert(!seed.startingGame.has_value());
        assert(seed.ResolveStartingGame() == Game::Mm);
    }

    // Default starting game: kSeedJson has an OoT starting item and no startingGame => Oot.
    // Its single entrance is a cross-game (oot -> mm) mapping.
    {
        Seed seed = LoadSeedFromJson(kSeedJson);
        assert(seed.ResolveStartingGame() == Game::Oot);
        const EntranceMapping* entrance = seed.FindEntrance(Game::Oot, static_cast<uint32_t>(604));
        assert(entrance != nullptr);
        assert(entrance->IsCrossGame());
        assert(entrance->toGame == Game::Mm);
        assert(entrance->toNativeId.has_value() && *entrance->toNativeId == 49168u);
    }

    // Cross-game transition packet round trip.
    {
        Seed seed = LoadSeedFromJson(kSeedJson);
        CrossGameTransition transition{ .fromGame = Game::Oot,
                                        .toGame = Game::Mm,
                                        .toEntrance = "MM_CLOCK_TOWER",
                                        .toNativeId = 49168u };
        const std::string packet = anchor::BuildCrossGameTransitionPacket(seed, transition);
        const auto parsed = anchor::ParseCrossGameTransitionPacket(packet);
        assert(parsed.has_value());
        assert(parsed->fromGame == Game::Oot && parsed->toGame == Game::Mm);
        assert(parsed->toEntrance == "MM_CLOCK_TOWER");
        assert(parsed->toNativeId.has_value() && *parsed->toNativeId == 49168u);
        // A different packet type must not parse as a transition.
        assert(!anchor::ParseCrossGameTransitionPacket("{\"type\":\"OOTMM_GIVE_ITEM\"}").has_value());
    }

    // Runtime progress state survives a serialize/restore (simulating a handoff).
    {
        Seed seed = LoadSeedFromJson(kSeedJson);
        Runtime source;
        source.LoadSeed(seed);
        assert(source.QueuedItemCount() == 2); // two Deku Sticks from startingItems
        (void)source.CompleteCheck(Game::Oot, "OOT_KF_MIDOS_TOP_LEFT");
        assert(source.CompletedCheckCount() == 1);
        const std::string snapshot = source.SerializeState();

        Runtime restored;
        restored.LoadSeed(seed); // re-queues starting items
        restored.RestoreState(snapshot);
        assert(restored.CompletedCheckCount() == source.CompletedCheckCount());
        assert(restored.QueuedItemCount() == source.QueuedItemCount());
        assert(restored.IsCheckCompleted(Game::Oot, "OOT_KF_MIDOS_TOP_LEFT"));
        // Re-completing the restored check yields nothing (dedup state carried over).
        assert(restored.CompleteCheck(Game::Oot, "OOT_KF_MIDOS_TOP_LEFT").empty());
    }
}

// ResetProgress wipes the cross-game ledger back to a fresh playthrough (only the seed's starting
// items), so deleting a save and starting over does not inherit the persisted co-save state. This
// is what Sram_InitSave -> Ootmm_ResetCrossGameState relies on in the OoT port.
void TestResetProgress() {
    using namespace ootmm;

    Seed seed = LoadSeedFromJson(kSeedJson);
    Runtime runtime;
    runtime.LoadSeed(seed);
    assert(runtime.QueuedItemCount() == 2); // two starting Deku Sticks

    // Simulate a played session: complete a check, grant an item, drain the queue.
    (void)runtime.CompleteCheck(Game::Oot, "OOT_KF_MIDOS_TOP_LEFT");
    assert(runtime.CompletedCheckCount() == 1);
    runtime.MarkItemGranted("starting:OOT_DEKU_STICK:0");
    assert(runtime.IsItemGranted("starting:OOT_DEKU_STICK:0"));
    while (runtime.HasQueuedItem()) {
        (void)runtime.PopQueuedItem();
    }
    assert(runtime.QueuedItemCount() == 0);

    // Reset (new save file): completed checks / granted keys cleared, starting items re-queued.
    runtime.ResetProgress();
    assert(runtime.CompletedCheckCount() == 0);
    assert(!runtime.IsCheckCompleted(Game::Oot, "OOT_KF_MIDOS_TOP_LEFT"));
    assert(!runtime.IsItemGranted("starting:OOT_DEKU_STICK:0"));
    assert(runtime.QueuedItemCount() == 2); // starting items requeued for redelivery

    // The serialized co-save written after the reset carries no prior progress.
    const std::string fresh = runtime.SerializeState();
    assert(fresh.find("OOT_KF_MIDOS_TOP_LEFT") == std::string::npos);

    // The seed itself is untouched: it can still be re-completed.
    assert(!runtime.CompleteCheck(Game::Oot, "OOT_KF_MIDOS_TOP_LEFT").empty());
}

} // namespace

// Verifies the loader captures every scalar setting kind (bool/string/number), the
// resolved worldFlags (none/all/explicit list), and the nested specialConds tree — the
// data the ports translate into native rando options. Before this, only booleans were
// kept and every enum/number/world-flag/special-cond was silently dropped.
void TestSettingsParsing() {
    const char* json = R"json(
    {
      "format": "ootmm.pc.seed.v1",
      "seedId": "settings-seed",
      "playerId": 1,
      "playerCount": 1,
      "settings": {
        "dekuTree": "open",
        "doorOfTime": "closed",
        "rainbowBridge": "medallions",
        "skipZelda": true,
        "fillWallets": false,
        "triforcePieces": 30,
        "triforceGoal": 20,
        "specialConds": {
          "BRIDGE": { "count": 3, "medallions": true, "stones": false, "remains": true },
          "MOON":   { "count": 0, "remains": false }
        }
      },
      "worldFlags": {
        "ganonTrials": "none",
        "openDungeonsOot": ["JJ", "BotW"],
        "mqDungeons": "all"
      }
    }
    )json";
    const ootmm::Seed seed = ootmm::LoadSeedFromJson(json);

    // Scalar settings by kind.
    assert(seed.GetStringSetting("dekuTree") == "open");
    assert(seed.GetStringSetting("doorOfTime", "open") == "closed");
    assert(seed.GetStringSetting("missing", "fallback") == "fallback");
    assert(seed.GetBoolSetting("skipZelda") == true);
    assert(seed.GetBoolSetting("fillWallets") == false);
    assert(seed.GetIntSetting("triforcePieces") == 30);
    assert(seed.GetIntSetting("triforceGoal", 1) == 20);
    assert(seed.GetIntSetting("absent", 7) == 7);
    // Enum strings must NOT leak into the bool map (and vice versa).
    assert(seed.GetBoolSetting("dekuTree", false) == false);

    // worldFlags: none -> empty, explicit list -> membership, all -> contains anything.
    assert(seed.WorldFlagContains("ganonTrials", "Fire") == false);
    assert(seed.WorldFlagCount("ganonTrials", 6) == 0);
    assert(seed.WorldFlagContains("openDungeonsOot", "JJ") == true);
    assert(seed.WorldFlagContains("openDungeonsOot", "DC") == false);
    assert(seed.WorldFlagCount("openDungeonsOot", 99) == 2);
    assert(seed.WorldFlagContains("mqDungeons", "anything") == true); // "all"
    assert(seed.WorldFlagCount("mqDungeons", 12) == 12);              // all -> totalIfAll
    assert(seed.GetWorldFlag("absentFlag") == nullptr);

    // specialConds: threshold + per-category booleans.
    const ootmm::Seed::SpecialCond* bridge = seed.GetSpecialCond("BRIDGE");
    assert(bridge != nullptr);
    assert(bridge->count == 3);
    assert(bridge->Field("medallions") == true);
    assert(bridge->Field("remains") == true);
    assert(bridge->Field("stones") == false);
    assert(bridge->Field("absentCategory") == false);
    const ootmm::Seed::SpecialCond* moon = seed.GetSpecialCond("MOON");
    assert(moon != nullptr && moon->count == 0);
    assert(seed.GetSpecialCond("ABSENT") == nullptr);
}

void TestCamcCategories() {
    using ootmm::CamcCategory;
    using ootmm::Game;

    // Raw table classification (no seed): intrinsic ITT type, not the per-seed major flag.
    assert(ootmm::CamcRawCategoryForItem("OOT_HOOKSHOT") == CamcCategory::Major);
    assert(ootmm::CamcRawCategoryForItem("OOT_SHIELD_HYLIAN") == CamcCategory::Normal); // shield != major
    assert(ootmm::CamcRawCategoryForItem("OOT_RUPEE_SILVER_DC") == CamcCategory::Key);  // silver rupee = key
    assert(ootmm::CamcRawCategoryForItem("OOT_BOSS_KEY_FOREST") == CamcCategory::BossKey);
    assert(ootmm::CamcRawCategoryForItem("OOT_HEART_PIECE") == CamcCategory::Heart);
    assert(ootmm::CamcRawCategoryForItem("OOT_SOUL_NPC_BOMBCHU_SHOPKEEPER") == CamcCategory::Soul);
    assert(ootmm::CamcRawCategoryForItem("OOT_STONE_OF_AGONY") == CamcCategory::Major);
    assert(ootmm::CamcRawCategoryForItem("NOTHING") == CamcCategory::Normal);
    assert(ootmm::CamcRawCategoryForItem("NOT_A_REAL_ID") == CamcCategory::Normal);

    // csmcHearts / csmcMapCompass downgrade to Normal when off (default on keeps the category).
    const ootmm::Seed defaults = ootmm::LoadSeedFromJson(
        R"json({"format":"ootmm.pc.seed.v1","seedId":"s"})json");
    assert(ootmm::CamcCategoryForItem("OOT_HEART_PIECE", Game::Oot, defaults) == CamcCategory::Heart);

    const ootmm::Seed heartsOff = ootmm::LoadSeedFromJson(
        R"json({"format":"ootmm.pc.seed.v1","seedId":"s","settings":{"csmcHearts":false}})json");
    assert(ootmm::CamcCategoryForItem("OOT_HEART_PIECE", Game::Oot, heartsOff) == CamcCategory::Normal);

    // Bombchu special-case: a raw pickup is Normal by default, Major when bombchuBehavior=bagFirst.
    assert(ootmm::CamcCategoryForItem("OOT_BOMBCHU_10", Game::Oot, defaults) == CamcCategory::Normal);
    const ootmm::Seed bagFirst = ootmm::LoadSeedFromJson(
        R"json({"format":"ootmm.pc.seed.v1","seedId":"s","settings":{"bombchuBehaviorOot":"bagFirst"}})json");
    assert(ootmm::CamcCategoryForItem("OOT_BOMBCHU_10", Game::Oot, bagFirst) == CamcCategory::Major);
    // ...but only for the matching game's setting.
    assert(ootmm::CamcCategoryForItem("MM_BOMBCHU_10", Game::Mm, bagFirst) == CamcCategory::Normal);

    // csmc gating: always -> on; never -> off; agony -> only with the stone.
    assert(ootmm::CamcEnabled(defaults, false) == true);  // default is "always"
    const ootmm::Seed never = ootmm::LoadSeedFromJson(
        R"json({"format":"ootmm.pc.seed.v1","seedId":"s","settings":{"csmc":"never"}})json");
    assert(ootmm::CamcEnabled(never, true) == false);
    const ootmm::Seed agony = ootmm::LoadSeedFromJson(
        R"json({"format":"ootmm.pc.seed.v1","seedId":"s","settings":{"csmc":"agony"}})json");
    assert(ootmm::CamcEnabled(agony, false) == false);
    assert(ootmm::CamcEnabled(agony, true) == true);

    // Skulltula/Cow sub-gates default off, and require the master gate.
    assert(ootmm::CamcEnabledSkulltula(defaults, false) == false);
    const ootmm::Seed skullOn = ootmm::LoadSeedFromJson(
        R"json({"format":"ootmm.pc.seed.v1","seedId":"s","settings":{"csmcSkulltula":true}})json");
    assert(ootmm::CamcEnabledSkulltula(skullOn, false) == true);
    assert(ootmm::CamcEnabledSkulltula(never, true) == false); // master off -> sub off

    // Palette matches OoTMM csmc_util.c.
    assert(ootmm::CamcTypeColor(CamcCategory::Major).r == 0xFF &&
           ootmm::CamcTypeColor(CamcCategory::Major).g == 0xFF &&
           ootmm::CamcTypeColor(CamcCategory::Major).b == 0x00);
    assert(ootmm::CamcTypeColor(CamcCategory::Key).r == 0x44);
    assert(ootmm::CamcTypeColor(CamcCategory::Normal).r == 0x29 &&
           ootmm::CamcTypeColor(CamcCategory::Normal).g == 0x14 &&
           ootmm::CamcTypeColor(CamcCategory::Normal).b == 0x0A);
}

void TestHasObtainedAgony() {
    // Stone obtained from a completed, locally-owned check (cross-game aware).
    const char* placedJson = R"json(
    {
      "format": "ootmm.pc.seed.v1",
      "seedId": "agony-placed",
      "playerId": 1,
      "playerCount": 1,
      "placements": [
        { "checkGame": "oot", "checkId": "OOT_KF_KOKIRI_SWORD_CHEST",
          "itemGame": "oot", "itemId": "OOT_STONE_OF_AGONY", "ownerPlayer": 1, "sourcePlayer": 1 }
      ]
    }
    )json";
    ootmm::Runtime placed;
    placed.LoadSeed(ootmm::LoadSeedFromJson(placedJson));
    assert(placed.HasObtainedAgony() == false); // not yet collected
    (void)placed.CompleteCheck(ootmm::Game::Oot, "OOT_KF_KOKIRI_SWORD_CHEST");
    assert(placed.HasObtainedAgony() == true); // collected

    // Stone as a starting item is immediately obtained.
    ootmm::Runtime starting;
    starting.LoadSeed(ootmm::LoadSeedFromJson(
        R"json({"format":"ootmm.pc.seed.v1","seedId":"agony-start",
                "startingItems":[{"itemGame":"oot","itemId":"OOT_STONE_OF_AGONY","count":1}]})json"));
    assert(starting.HasObtainedAgony() == true);

    // A seed with no agony stone never reports it.
    ootmm::Runtime none;
    none.LoadSeed(ootmm::LoadSeedFromJson(kSeedJson));
    assert(none.HasObtainedAgony() == false);

    // Broadcast-owned (ownerPlayer 0) stone IS delivered locally, so it must be detected.
    ootmm::Runtime broadcast;
    broadcast.LoadSeed(ootmm::LoadSeedFromJson(
        R"json({"format":"ootmm.pc.seed.v1","seedId":"agony-bcast","playerId":1,"playerCount":2,
                "placements":[{"checkGame":"oot","checkId":"OOT_KF_KOKIRI_SWORD_CHEST",
                "itemGame":"oot","itemId":"OOT_STONE_OF_AGONY","ownerPlayer":0,"sourcePlayer":1}]})json"));
    (void)broadcast.CompleteCheck(ootmm::Game::Oot, "OOT_KF_KOKIRI_SWORD_CHEST");
    assert(broadcast.HasObtainedAgony() == true);

    // A stone owned by ANOTHER player is not delivered locally, so it must NOT be reported.
    ootmm::Runtime other;
    other.LoadSeed(ootmm::LoadSeedFromJson(
        R"json({"format":"ootmm.pc.seed.v1","seedId":"agony-other","playerId":1,"playerCount":2,
                "placements":[{"checkGame":"oot","checkId":"OOT_KF_KOKIRI_SWORD_CHEST",
                "itemGame":"oot","itemId":"OOT_STONE_OF_AGONY","ownerPlayer":2,"sourcePlayer":1}]})json"));
    (void)other.CompleteCheck(ootmm::Game::Oot, "OOT_KF_KOKIRI_SWORD_CHEST");
    assert(other.HasObtainedAgony() == false);

    // The MM-side stone, collected from an MM check, is detected (cross-game aware).
    ootmm::Runtime mm;
    mm.LoadSeed(ootmm::LoadSeedFromJson(
        R"json({"format":"ootmm.pc.seed.v1","seedId":"agony-mm","playerId":1,"playerCount":1,
                "placements":[{"checkGame":"mm","checkId":"MM_CLOCK_TOWN_CHEST",
                "itemGame":"mm","itemId":"MM_STONE_OF_AGONY","ownerPlayer":1,"sourcePlayer":1}]})json"));
    assert(mm.HasObtainedAgony() == false);
    (void)mm.CompleteCheck(ootmm::Game::Mm, "MM_CLOCK_TOWN_CHEST");
    assert(mm.HasObtainedAgony() == true);

    // obtainedItems survives a serialize/restore handoff.
    const std::string snapshot = mm.SerializeState();
    ootmm::Runtime restored;
    restored.LoadSeed(ootmm::LoadSeedFromJson(
        R"json({"format":"ootmm.pc.seed.v1","seedId":"agony-mm","playerId":1,"playerCount":1,
                "placements":[{"checkGame":"mm","checkId":"MM_CLOCK_TOWN_CHEST",
                "itemGame":"mm","itemId":"MM_STONE_OF_AGONY","ownerPlayer":1,"sourcePlayer":1}]})json"));
    assert(restored.HasObtainedAgony() == false);
    restored.RestoreState(snapshot);
    assert(restored.HasObtainedAgony() == true);
}

int main() {
    TestSeedLoad();
    TestSettingsParsing();
    TestCamcCategories();
    TestHasObtainedAgony();
    TestRuntimeLocalAndRemoteChecks();
    TestAnchorGiveItemRoundTrip();
    TestAnchorEventPacketRouting();
    TestPortBridgeAdapters();
    TestGrantOperationResolver();
    TestNativeItemSymbols();
    TestResolvingGameAdapter();
    TestNativeGrantActions();
    TestPortBridgeToNativeGrantActions();
    TestNativePortSession();
    TestCrossGamePendingDelivery();
    TestFilePacketBridge();
    TestCrossGameAndState();
    TestResetProgress();
    std::cout << "ootmm_core_tests passed\n";
    return 0;
}
