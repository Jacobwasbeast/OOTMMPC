#pragma once

#include "ootmm/Seed.hpp"

#include <deque>
#include <map>
#include <set>

namespace ootmm {

class Runtime {
  public:
    void LoadSeed(Seed seed);
    [[nodiscard]] const Seed* ActiveSeed() const;

    // Reset the cross-game progress ledger to a fresh state (clears completed checks / delivered /
    // granted / obtained / queue, then re-queues only the seed's starting items). Called when a port
    // creates a new save file so a fresh playthrough does not inherit the persisted co-save state.
    void ResetProgress();

    [[nodiscard]] std::vector<NetworkEvent> CompleteCheck(Game game, const std::string& checkId);
    [[nodiscard]] bool IsCheckCompleted(Game game, const std::string& checkId) const;

    // Applies a check completion received on REMOTE authority — another player's live
    // completion, or the relay's join replay of this room's durable history (the server is the
    // source of truth for check state, so a fresh native save catches up without recollecting).
    // Marks the check completed with no announce and no outbound event. When the completion is
    // OUR OWN history (sourcePlayer == local player) and the placement is locally owned, the
    // item is also re-queued — QueueLocalItem dedups by delivery key, so on a resumed save this
    // is a no-op; after a new-save ResetProgress it restores the playthrough's items. Items for
    // OTHER players travel in their own GIVE_ITEM packets and are never queued here; cross-game
    // items re-derive from completedChecks_ via RegrantCrossGamePendingItems on the destination
    // game's boot; traps never replay. Returns true if the check was newly marked.
    bool ApplyRemoteCheckComplete(const NetworkEvent& event, Game activeGame);

    // Whether the local player has obtained the Stone of Agony (OoT or MM variant), used to gate
    // CAMC ("Container Appearance Matches Content") in `agony` mode. True if it is a starting item,
    // or if a completed check the local player owns holds it (cross-game aware: the check may live
    // in either game). The OoT port also has SoH's native per-frame QUEST_STONE_OF_AGONY gate;
    // this cross-game signal is primarily for the MM port, which has no native agony stone.
    [[nodiscard]] bool HasObtainedAgony() const;
    [[nodiscard]] const EntranceMapping* FindEntrance(Game fromGame, const std::string& from) const;
    [[nodiscard]] const EntranceMapping* FindEntrance(Game fromGame, uint32_t fromNativeId) const;

    [[nodiscard]] bool ReceiveRemoteItem(const NetworkEvent& event);
    [[nodiscard]] bool HasQueuedItem() const;
    [[nodiscard]] std::optional<ReceivedItem> PopQueuedItem();

    // Cross-game shared inventory parity. When OoTMM "shared" items are enabled,
    // a shared persistent item (equipment, capacity upgrade, song, heart) collected
    // in one game must also exist in the OTHER game in its native form. Each item is
    // delivered in its own game during normal collection, so this re-queues the
    // shared persistent items belonging to the *other* game(s) for the currently
    // running game, in that game's form, exactly once per game (idempotent across
    // reboots and handoffs). Consumable shared pools (rupees, ammo, recovery,
    // sticks, nuts) are intentionally NOT re-granted to avoid duplicating a pool
    // that is conceptually shared. Returns the number of items queued. Call on boot
    // after RestoreState so completed checks are known.
    std::size_t RegrantSharedPersistentItems(Game activeGame);

    // The core OoTMM combo mechanic: a check in one game may hold the OTHER game's
    // item (e.g. an OoT chest containing MM's Bow). In the save-and-relaunch model
    // only one game runs at a time, so such an item cannot be delivered when its
    // check is collected (the wrong engine is active). CompleteCheck records the
    // check as completed but does NOT queue the foreign item locally; instead, on
    // boot of the destination game, this scans completed checks for concrete items
    // belonging to activeGame whose check lives in the other game, and queues them
    // for delivery in their own engine. Idempotent across reboots/handoffs (each is
    // delivered exactly once, namespaced by activeGame in deliveredItems_). Honors
    // ownership (self / broadcast owner 0). Call on boot after RestoreState.
    // Returns the number of items queued.
    std::size_t RegrantCrossGamePendingItems(Game activeGame);

    [[nodiscard]] uint16_t LocalPlayerId() const;
    [[nodiscard]] std::size_t CompletedCheckCount() const;
    [[nodiscard]] std::size_t QueuedItemCount() const;

    // Serializes the mutable progress state (completed checks, delivered items,
    // and the pending item queue) to JSON so a coordinator can carry it across a
    // save-and-relaunch cross-game handoff. RestoreState replaces the live state
    // with a previously serialized snapshot; it does NOT touch the loaded seed,
    // so call it after LoadSeed (which seeds the queue with starting items).
    [[nodiscard]] std::string SerializeState() const;
    void RestoreState(const std::string& jsonText);

    // Grant-time idempotence: a delivery key is recorded only when its item is actually
    // GRANTED (not merely queued). The persisted item queue is replayed on every boot/handoff;
    // without this guard, undrained queue entries re-grant on the next launch and progressive
    // upgrades (wallet/scale/hearts) climb a tier each round trip. Serialized alongside the
    // other ledgers so the guard survives reboots.
    [[nodiscard]] bool IsItemGranted(const std::string& deliveryKey) const;
    void MarkItemGranted(const std::string& deliveryKey);

    // How many copies of `itemId` have been durably granted to this game. Every delivery key —
    // local ("<game>:<check>->p<N>:<item>"), cross-game ("xgame:<game>:...") and remote — ends
    // with ":<itemId>", and each placement grants exactly once per game, so counting granted
    // keys by that suffix yields the owned copy count. Used for count-gated puzzles whose state
    // must survive reboots (e.g. the OoT silver-rupee doors).
    [[nodiscard]] std::size_t CountGrantedItemId(const std::string& itemId) const;

    // Save-consistency guard. An item is GRANTED into the engine's in-memory save the instant it is
    // pumped, but the native .sav is a separate file written later. If the progress ledger is persisted
    // while the .sav is not (e.g. a cross-game handoff that forgot to flush the save), the ledger claims
    // the item was granted while the .sav never stored it -- and IsItemGranted then blocks re-granting it
    // forever = permanent loss. To make that impossible: RecordGrant marks a grant UNCONFIRMED; a native
    // save calls ConfirmGrants to clear the unconfirmed set (those items are now durably in the .sav);
    // and on load ReconcileUnconfirmedGrants replays any grant that is still unconfirmed (its item never
    // reached a saved .sav), clearing its granted key so it is delivered again. In steady state every
    // ledger persist is preceded by a save+confirm, so nothing replays; the replay only fires when a
    // persist slipped past a save, which is exactly the loss bug. Serialized so it survives reboots.
    void RecordGrant(const ReceivedItem& item);
    std::size_t ConfirmGrants();
    std::size_t ReconcileUnconfirmedGrants();

    // Small persisted key/value store for port-side session context that must survive a
    // save+relaunch (e.g. the OoT grotto Link was inside when the save was written, so the
    // grotto exit can still resolve after a reload). Serialized with the runtime state.
    void SetSessionValue(const std::string& key, const std::string& value);
    void EraseSessionValue(const std::string& key);
    [[nodiscard]] std::optional<std::string> GetSessionValue(const std::string& key) const;

  private:
    Seed seed_;
    bool hasSeed_ = false;
    std::set<std::string> completedChecks_;
    std::set<std::string> deliveredItems_;
    // Delivery keys whose item has actually been GRANTED to the local engine (see
    // IsItemGranted/MarkItemGranted). Distinct from deliveredItems_, which is recorded at
    // QUEUE time and so cannot stop a persisted-queue replay from re-granting.
    std::set<std::string> grantedKeys_;
    // Item ids actually delivered to the LOCAL player (starting items + every queued grant:
    // local-check, cross-game regrant, shared regrant, and remote). Recorded at delivery, so it
    // captures "what the player has obtained" regardless of ownership/source. Serialized so it
    // survives reboots/handoffs. HasObtainedAgony reads it.
    std::set<std::string> obtainedItemIds_;
    std::deque<ReceivedItem> itemQueue_;
    // Grants applied to the in-memory save but not yet confirmed durable by a native save (see
    // RecordGrant/ConfirmGrants/ReconcileUnconfirmedGrants). Serialized so an unconfirmed grant that
    // was persisted without a save is replayed on the next load rather than lost.
    std::deque<ReceivedItem> pendingConfirmation_;
    // See SetSessionValue.
    std::map<std::string, std::string> sessionValues_;

    [[nodiscard]] std::string CheckKey(Game game, const std::string& checkId) const;
    void QueueLocalItem(const ItemPlacement& placement, bool fromRemote);
};

} // namespace ootmm
