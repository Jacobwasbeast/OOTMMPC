#include "ootmm/Runtime.hpp"

#include "SimpleJson.hpp"

#include <sstream>
#include <stdexcept>
#include <string_view>

namespace ootmm {
namespace {

// True when a SHARED_* item represents a persistent capability that must exist
// independently in each game (equipment, capacity upgrade, song, mask, heart),
// as opposed to a consumable that is conceptually a single shared pool (rupees,
// ammo, recovery, sticks/nuts ammo). Only persistent items are re-granted in the
// other game's native form on a cross-game handoff; re-granting a consumable
// pool per game would duplicate currency/ammo the player already pooled.
[[nodiscard]] bool IsSharedPersistentItem(std::string_view id) {
    if (id.rfind("SHARED_", 0) != 0) {
        return false;
    }
    // Bare consumable ammo (deku stick / deku nut counts) — single shared pool.
    if (id == "SHARED_STICK" || id == "SHARED_NUT") {
        return false;
    }
    // Consumable currency / ammo counts / recovery / magic refills — shared pool.
    // NOTE: BOMBCHU_<count> is consumable but BOMBCHU_BAG (capacity) is persistent;
    // BOMBS_<count> is consumable but BOMB_BAG (no trailing 'S_') is persistent.
    static constexpr std::string_view kConsumableNeedles[] = {
        "RUPEE",          "ARROWS_",        "BOMBS_",   "BOMBCHU_5",
        "BOMBCHU_10",     "BOMBCHU_20",     "BOMBCHU_30", "BOMBCHU_40",
        "NUTS_",          "STICKS_",        "SEEDS_",   "RECOVERY_HEART",
        "MAGIC_JAR",
    };
    for (const std::string_view needle : kConsumableNeedles) {
        if (id.find(needle) != std::string_view::npos) {
            return false;
        }
    }
    return true;
}

// The game a concrete OoTMM item belongs to, by id prefix. Shared items (SHARED_*)
// are not concrete to a single game — they resolve to the active game's form at
// delivery and are mirrored by RegrantSharedPersistentItems — so they return
// nullopt here and are excluded from cross-game pending routing.
[[nodiscard]] std::optional<Game> ConcreteItemGame(std::string_view id) {
    if (id.rfind("MM_", 0) == 0) {
        return Game::Mm;
    }
    if (id.rfind("OOT_", 0) == 0) {
        return Game::Oot;
    }
    return std::nullopt;
}

// Traps (OOT_TRAP_*) are game-LOCAL effects, not carried items: in OoTMM the shared trap counter is
// applied by whichever game is active, so a trap fires in the game where its check is collected — even
// though the item id nominally belongs to one game. They must therefore deliver locally on collection
// (never held for / re-granted to the other game), unlike every other cross-game item.
[[nodiscard]] bool IsTrapItem(std::string_view id) {
    return id.find("_TRAP_") != std::string_view::npos;
}

} // namespace

void Runtime::LoadSeed(Seed seed) {
    if (!seed.IsCompatible()) {
        throw SeedLoadError("Cannot activate incompatible seed");
    }
    seed_ = std::move(seed);
    hasSeed_ = true;
    ResetProgress();
}

void Runtime::ResetProgress() {
    // Wipe the cross-game progress ledger and re-queue ONLY the seed's starting items, so a fresh
    // save does not inherit a previous playthrough's state from the persisted co-save.
    completedChecks_.clear();
    deliveredItems_.clear();
    grantedKeys_.clear();
    obtainedItemIds_.clear();
    itemQueue_.clear();
    pendingConfirmation_.clear();
    sessionValues_.clear();

    for (const StartingItem& startingItem : seed_.startingItems) {
        for (uint16_t i = 0; i < startingItem.count; ++i) {
            const std::string key = "starting:" + startingItem.item.id + ":" + std::to_string(i);
            deliveredItems_.insert(key);
            obtainedItemIds_.insert(startingItem.item.id);
            itemQueue_.push_back({
                .item = startingItem.item,
                .sourcePlayer = seed_.playerId,
                .sourceCheckId = "starting",
                .deliveryKey = key,
                .fromRemote = false,
            });
        }
    }
}

const Seed* Runtime::ActiveSeed() const {
    return hasSeed_ ? &seed_ : nullptr;
}

std::vector<NetworkEvent> Runtime::CompleteCheck(Game game, const std::string& checkId) {
    if (!hasSeed_) {
        throw std::logic_error("Cannot complete check without an active seed");
    }

    const ItemPlacement* placement = seed_.FindPlacement(game, checkId);
    if (placement == nullptr) {
        throw std::out_of_range("Unknown OoTMM check: " + CheckKey(game, checkId));
    }

    const std::string checkKey = CheckKey(placement->check.game, placement->check.id);
    if (completedChecks_.contains(checkKey)) {
        return {};
    }

    completedChecks_.insert(checkKey);

    NetworkEvent event = {
        .type = NetworkEventType::CheckComplete,
        .targetPlayer = placement->ownerPlayer,
        .sourcePlayer = seed_.playerId,
        .checkId = placement->check.id,
        .checkGame = placement->check.game,
        .item = placement->item,
        .deliveryKey = placement->DeliveryKey(),
    };

    // Linked multiworld traps: a collected trap fires for EVERY player in the session, the
    // collector included, regardless of who nominally owns the placement. The collector's copy
    // queues locally (its prompt carries no sender); every other player receives a GiveItem
    // stamped with the collector so their prompt reads "... from Player N". Each remote copy's
    // delivery key is namespaced by the collector: two players' worlds can hold the identical
    // (check, owner, item) tuple, and the shared owner-keyed value would make the second trap
    // vanish in every receiver's dedup. The CheckComplete event is retargeted at the collector
    // so the packet bridge never emits an owner-addressed GiveItem alongside the broadcast
    // (the owner would be trapped twice).
    if (IsTrapItem(placement->item.id)) {
        QueueLocalItem(*placement, false);
        event.targetPlayer = seed_.playerId;
        std::vector<NetworkEvent> events{ event };
        for (uint16_t pid = 1; pid <= seed_.playerCount; ++pid) {
            if (pid == seed_.playerId) {
                continue;
            }
            NetworkEvent give = event;
            give.type = NetworkEventType::GiveItem;
            give.targetPlayer = pid;
            give.deliveryKey = placement->DeliveryKey() + ":linked:p" + std::to_string(seed_.playerId);
            events.push_back(give);
        }
        return events;
    }

    if (placement->ownerPlayer == seed_.playerId || placement->ownerPlayer == 0) {
        // Deliver locally only if the item belongs to the game currently running.
        // A check in this game may hold the OTHER game's item (the core OoTMM combo
        // mechanic); that item is held until the destination game next boots, where
        // RegrantCrossGamePendingItems queues it. Shared items (SHARED_*, nullopt
        // here) resolve to the active game's form at delivery and are mirrored to the
        // other game by RegrantSharedPersistentItems, so they deliver locally as usual.
        const std::optional<Game> itemGame = ConcreteItemGame(placement->item.id);
        if (!itemGame.has_value() || *itemGame == game) {
            QueueLocalItem(*placement, false);
        }
    }

    return { event };
}

bool Runtime::ApplyRemoteCheckComplete(const NetworkEvent& event, Game activeGame) {
    if (!hasSeed_) {
        return false;
    }

    const ItemPlacement* placement = seed_.FindPlacement(event.checkGame, event.checkId);
    if (placement == nullptr) {
        return false; // tolerate checks this client's seed copy does not know
    }

    const bool newlyMarked = completedChecks_.insert(CheckKey(placement->check.game, placement->check.id)).second;

    // Replay of OUR OWN collection history: restore the locally-owned item too. On a resumed
    // save the delivery-key dedup makes this a no-op; after a new-save ResetProgress it is how
    // the playthrough's items come back. Foreign-game items are intentionally skipped —
    // RegrantCrossGamePendingItems re-derives them from completedChecks_ when their game boots.
    // Traps are a prank, not progression, so they never replay.
    if (event.sourcePlayer == seed_.playerId &&
        (placement->ownerPlayer == seed_.playerId || placement->ownerPlayer == 0) &&
        !IsTrapItem(placement->item.id)) {
        const std::optional<Game> itemGame = ConcreteItemGame(placement->item.id);
        if (!itemGame.has_value() || *itemGame == activeGame) {
            QueueLocalItem(*placement, false);
        }
    }
    return newlyMarked;
}

bool Runtime::IsCheckCompleted(Game game, const std::string& checkId) const {
    if (!hasSeed_) {
        return false;
    }

    const ItemPlacement* placement = seed_.FindPlacement(game, checkId);
    if (placement == nullptr) {
        return false;
    }

    return completedChecks_.contains(CheckKey(placement->check.game, placement->check.id));
}

bool Runtime::HasObtainedAgony() const {
    // "Obtained" is tracked at every delivery (obtainedItemIds_), so this is correct for the
    // OoT or MM stone regardless of how it arrived: a starting item, a locally-collected check
    // (incl. the broadcast owner 0), a cross-game regrant, or a remote multiworld delivery. A
    // count==0 starting item delivers nothing and is therefore never recorded.
    return obtainedItemIds_.contains("OOT_STONE_OF_AGONY") ||
           obtainedItemIds_.contains("MM_STONE_OF_AGONY");
}

const EntranceMapping* Runtime::FindEntrance(Game fromGame, const std::string& from) const {
    return hasSeed_ ? seed_.FindEntrance(fromGame, from) : nullptr;
}

const EntranceMapping* Runtime::FindEntrance(Game fromGame, uint32_t fromNativeId) const {
    return hasSeed_ ? seed_.FindEntrance(fromGame, fromNativeId) : nullptr;
}

bool Runtime::ReceiveRemoteItem(const NetworkEvent& event) {
    if (!hasSeed_) {
        throw std::logic_error("Cannot receive remote item without an active seed");
    }
    if (event.targetPlayer != seed_.playerId && event.targetPlayer != 0) {
        return false;
    }
    if (event.deliveryKey.empty() || deliveredItems_.contains(event.deliveryKey)) {
        return false;
    }

    deliveredItems_.insert(event.deliveryKey);
    obtainedItemIds_.insert(event.item.id);
    itemQueue_.push_back({
        .item = event.item,
        .sourcePlayer = event.sourcePlayer,
        .sourceCheckId = event.checkId,
        .deliveryKey = event.deliveryKey,
        .fromRemote = true,
    });
    return true;
}

bool Runtime::HasQueuedItem() const {
    return !itemQueue_.empty();
}

std::optional<ReceivedItem> Runtime::PopQueuedItem() {
    if (itemQueue_.empty()) {
        return std::nullopt;
    }
    ReceivedItem item = itemQueue_.front();
    itemQueue_.pop_front();
    return item;
}

std::size_t Runtime::RegrantSharedPersistentItems(Game activeGame) {
    if (!hasSeed_) {
        return 0;
    }

    std::size_t queued = 0;
    for (const ItemPlacement& placement : seed_.placements) {
        // Only shared persistent capabilities cross the game boundary this way.
        if (!IsSharedPersistentItem(placement.item.id)) {
            continue;
        }
        // The item was already delivered in its own game during normal collection;
        // we only synthesize the copy for the OTHER game(s).
        if (placement.check.game == activeGame) {
            continue;
        }
        // Honor ownership: in a multiworld only items routed to the local player
        // (or the broadcast owner 0) should materialize on this client.
        if (placement.ownerPlayer != seed_.playerId && placement.ownerPlayer != 0) {
            continue;
        }
        // Only items whose source check is actually collected exist yet.
        if (!completedChecks_.contains(CheckKey(placement.check.game, placement.check.id))) {
            continue;
        }

        // A delivery key namespaced by the active game so the re-grant happens
        // exactly once per game and survives reboots/handoffs via deliveredItems_.
        const std::string key = "regrant:" + ToString(activeGame) + ":" + placement.DeliveryKey();
        if (deliveredItems_.contains(key)) {
            continue;
        }
        deliveredItems_.insert(key);
        obtainedItemIds_.insert(placement.item.id);
        itemQueue_.push_back({
            .item = placement.item,
            .sourcePlayer = placement.sourcePlayer,
            .sourceCheckId = placement.check.id,
            .deliveryKey = key,
            .fromRemote = false,
        });
        ++queued;
    }
    return queued;
}

std::size_t Runtime::RegrantCrossGamePendingItems(Game activeGame) {
    if (!hasSeed_) {
        return 0;
    }

    std::size_t queued = 0;
    for (const ItemPlacement& placement : seed_.placements) {
        // Only concrete items belonging to the game now running.
        const std::optional<Game> itemGame = ConcreteItemGame(placement.item.id);
        if (!itemGame.has_value() || *itemGame != activeGame) {
            continue;
        }
        // Traps already fired locally in the game where they were collected (game-local effects),
        // so they are never carried to / re-applied in the other game.
        if (IsTrapItem(placement.item.id)) {
            continue;
        }
        // Only cross-game placements: the source check lives in the OTHER game.
        // (Same-game items were already delivered locally by CompleteCheck.)
        if (placement.check.game == activeGame) {
            continue;
        }
        // Honor ownership: only items routed to the local player or the broadcast
        // owner (0) materialize on this client.
        if (placement.ownerPlayer != seed_.playerId && placement.ownerPlayer != 0) {
            continue;
        }
        // Only items whose source check has actually been collected exist yet.
        if (!completedChecks_.contains(CheckKey(placement.check.game, placement.check.id))) {
            continue;
        }
        // A delivery key namespaced by the destination game so the foreign item is
        // granted exactly once per game and survives reboots/handoffs via
        // deliveredItems_ (which is serialized in the runtime state).
        const std::string key = "xgame:" + ToString(activeGame) + ":" + placement.DeliveryKey();
        if (deliveredItems_.contains(key)) {
            continue;
        }
        deliveredItems_.insert(key);
        obtainedItemIds_.insert(placement.item.id);
        itemQueue_.push_back({
            .item = placement.item,
            .sourcePlayer = placement.sourcePlayer,
            .sourceCheckId = placement.check.id,
            .deliveryKey = key,
            .fromRemote = false,
        });
        ++queued;
    }
    return queued;
}

uint16_t Runtime::LocalPlayerId() const {
    return hasSeed_ ? seed_.playerId : 0;
}

std::size_t Runtime::CompletedCheckCount() const {
    return completedChecks_.size();
}

std::size_t Runtime::QueuedItemCount() const {
    return itemQueue_.size();
}

std::string Runtime::SerializeState() const {
    std::ostringstream out;
    out << "{\"completedChecks\":[";
    bool first = true;
    for (const std::string& key : completedChecks_) {
        out << (first ? "" : ",") << json::EscapeString(key);
        first = false;
    }
    out << "],\"deliveredItems\":[";
    first = true;
    for (const std::string& key : deliveredItems_) {
        out << (first ? "" : ",") << json::EscapeString(key);
        first = false;
    }
    out << "],\"grantedKeys\":[";
    first = true;
    for (const std::string& key : grantedKeys_) {
        out << (first ? "" : ",") << json::EscapeString(key);
        first = false;
    }
    out << "],\"obtainedItems\":[";
    first = true;
    for (const std::string& id : obtainedItemIds_) {
        out << (first ? "" : ",") << json::EscapeString(id);
        first = false;
    }
    out << "]"; // close the obtainedItems array
    const auto writeItemArray = [&](const char* key, const std::deque<ReceivedItem>& items) {
        out << ",\"" << key << "\":[";
        bool firstItem = true;
        for (const ReceivedItem& item : items) {
            out << (firstItem ? "" : ",") << "{\"itemGame\":" << json::EscapeString(ToString(item.item.game))
                << ",\"itemId\":" << json::EscapeString(item.item.id)
                << ",\"itemName\":" << json::EscapeString(item.item.name)
                << ",\"sourcePlayer\":" << item.sourcePlayer
                << ",\"sourceCheckId\":" << json::EscapeString(item.sourceCheckId)
                << ",\"deliveryKey\":" << json::EscapeString(item.deliveryKey)
                << ",\"fromRemote\":" << (item.fromRemote ? "true" : "false") << "}";
            firstItem = false;
        }
        out << "]";
    };
    writeItemArray("itemQueue", itemQueue_);
    writeItemArray("pendingConfirmation", pendingConfirmation_);
    out << ",\"sessionValues\":{";
    first = true;
    for (const auto& [key, value] : sessionValues_) {
        out << (first ? "" : ",") << json::EscapeString(key) << ":" << json::EscapeString(value);
        first = false;
    }
    out << "}}";
    return out.str();
}

void Runtime::SetSessionValue(const std::string& key, const std::string& value) {
    sessionValues_[key] = value;
}

void Runtime::EraseSessionValue(const std::string& key) {
    sessionValues_.erase(key);
}

std::optional<std::string> Runtime::GetSessionValue(const std::string& key) const {
    const auto it = sessionValues_.find(key);
    if (it == sessionValues_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void Runtime::RestoreState(const std::string& jsonText) {
    json::Value root;
    try {
        root = json::Parse(jsonText);
    } catch (const json::ParseError& error) {
        throw std::runtime_error(std::string("Invalid runtime state JSON: ") + error.what());
    }
    if (!root.IsObject()) {
        throw std::runtime_error("Runtime state must be a JSON object");
    }

    completedChecks_.clear();
    deliveredItems_.clear();
    grantedKeys_.clear();
    obtainedItemIds_.clear();
    itemQueue_.clear();
    pendingConfirmation_.clear();
    sessionValues_.clear();

    if (const json::Value* values = root.Find("sessionValues"); values != nullptr && values->IsObject()) {
        for (const auto& [svKey, svValue] : values->AsObject()) {
            if (svValue.IsString()) {
                sessionValues_[svKey] = svValue.AsString();
            }
        }
    }

    const auto readStringArray = [&](const char* key, std::set<std::string>& target) {
        const json::Value* array = root.Find(key);
        if (array == nullptr || !array->IsArray()) {
            return;
        }
        for (const json::Value& entry : array->AsArray()) {
            if (entry.IsString()) {
                target.insert(entry.AsString());
            }
        }
    };
    readStringArray("completedChecks", completedChecks_);
    readStringArray("deliveredItems", deliveredItems_);
    readStringArray("grantedKeys", grantedKeys_);
    readStringArray("obtainedItems", obtainedItemIds_);

    const auto readItemArray = [&](const char* key, std::deque<ReceivedItem>& target) {
        const json::Value* array = root.Find(key);
        if (array == nullptr || !array->IsArray()) {
            return;
        }
        for (const json::Value& entry : array->AsArray()) {
            if (!entry.IsObject()) {
                continue;
            }
            // Not named `u16`/`str`: this file compiles inside each port's TU, where the OoT/MM
            // decomp defines `u16`-style aliases as macros that would mangle these declarations.
            const auto readStr = [&](const char* field) -> std::string {
                const json::Value* value = entry.Find(field);
                return (value != nullptr && value->IsString()) ? value->AsString() : std::string();
            };
            const auto readU16 = [&](const char* field) -> uint16_t {
                const json::Value* value = entry.Find(field);
                return (value != nullptr && value->IsNumber()) ? static_cast<uint16_t>(value->AsNumber()) : 0;
            };
            const json::Value* fromRemote = entry.Find("fromRemote");
            const auto itemGame = GameFromString(readStr("itemGame"));

            target.push_back(ReceivedItem{
                .item = { .game = itemGame.value_or(Game::Oot), .id = readStr("itemId"), .name = readStr("itemName") },
                .sourcePlayer = readU16("sourcePlayer"),
                .sourceCheckId = readStr("sourceCheckId"),
                .deliveryKey = readStr("deliveryKey"),
                .fromRemote = fromRemote != nullptr && fromRemote->IsBool() && fromRemote->AsBool(),
            });
        }
    };
    readItemArray("itemQueue", itemQueue_);
    readItemArray("pendingConfirmation", pendingConfirmation_);
}

std::string Runtime::CheckKey(Game game, const std::string& checkId) const {
    return ToString(game) + ":" + checkId;
}

void Runtime::QueueLocalItem(const ItemPlacement& placement, bool fromRemote) {
    const std::string key = placement.DeliveryKey();
    if (deliveredItems_.contains(key)) {
        return;
    }
    deliveredItems_.insert(key);
    obtainedItemIds_.insert(placement.item.id);
    itemQueue_.push_back({
        .item = placement.item,
        .sourcePlayer = placement.sourcePlayer,
        .sourceCheckId = placement.check.id,
        .deliveryKey = key,
        .fromRemote = fromRemote,
    });
}

bool Runtime::IsItemGranted(const std::string& deliveryKey) const {
    return grantedKeys_.contains(deliveryKey);
}

void Runtime::MarkItemGranted(const std::string& deliveryKey) {
    grantedKeys_.insert(deliveryKey);
}

std::size_t Runtime::CountGrantedItemId(const std::string& itemId) const {
    const std::string suffix = ":" + itemId;
    std::size_t count = 0;
    for (const std::string& key : grantedKeys_) {
        if (key.size() >= suffix.size() && key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0) {
            ++count;
        }
    }
    return count;
}

void Runtime::RecordGrant(const ReceivedItem& item) {
    grantedKeys_.insert(item.deliveryKey);
    // Unconfirmed until a native save persists it. Traps are instantaneous effects with no saved state,
    // so they must never be replayed (a replay would re-trigger the effect); exclude them.
    if (!IsTrapItem(item.item.id)) {
        pendingConfirmation_.push_back(item);
    }
}

std::size_t Runtime::ConfirmGrants() {
    const std::size_t n = pendingConfirmation_.size();
    pendingConfirmation_.clear();
    return n;
}

std::size_t Runtime::ReconcileUnconfirmedGrants() {
    // Every still-unconfirmed grant was applied to an in-memory save that was never durably written
    // (its ledger was persisted without a following save). Clear its granted key and re-queue it so it
    // is delivered again onto the freshly loaded save. Deduped by delivery key so a single item can't be
    // queued twice. Steady-state pending is empty (save+confirm precedes every persist), so this no-ops.
    std::set<std::string> seen;
    std::size_t requeued = 0;
    for (const ReceivedItem& item : pendingConfirmation_) {
        if (!seen.insert(item.deliveryKey).second) {
            continue;
        }
        grantedKeys_.erase(item.deliveryKey);
        itemQueue_.push_back(item);
        ++requeued;
    }
    pendingConfirmation_.clear();
    return requeued;
}

} // namespace ootmm
