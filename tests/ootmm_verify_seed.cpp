// Headless seed completability verifier (structural + multiworld): drives the real ootmm_core
// pipeline (Runtime/PortBridge/regrant/Anchor routing) to full completion for every player world.
// Core-undeliverable ids are WARNINGS only (ports carry extra delivery tables core does not).
// Usage: ootmm_verify_seed <player1.ootmm.json> [player2 ...]; exit 0 = PASS, 1 = FAIL, 2 = load error.

#include "ootmm/AnchorBridge.hpp"
#include "ootmm/GrantApplier.hpp"
#include "ootmm/NativeGrantAction.hpp"
#include "ootmm/NativeItem.hpp"
#include "ootmm/PortBridge.hpp"
#include "ootmm/Runtime.hpp"
#include "ootmm/Seed.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

using ootmm::Game;

const char* GameName(Game g) {
    return g == Game::Oot ? "oot" : "mm";
}

ootmm::NativePort PortFor(Game g) {
    return g == Game::Oot ? ootmm::NativePort::ShipOfHarkinian : ootmm::NativePort::TwoShip2Harkinian;
}

// Records every ResolvedGrant, flagging bare RawItems with no native symbol/grant action (core-undeliverable).
struct RecordingAdapter final : public ootmm::IGrantOperationAdapter {
    Game game;
    std::size_t delivered = 0;
    std::set<std::string> coreUndeliverable;

    explicit RecordingAdapter(Game g) : game(g) {}
    Game ActiveGame() const override {
        return game;
    }
    void ApplyGrant(const ootmm::ResolvedGrant& grant) override {
        ++delivered;
        if (grant.operation.kind != ootmm::GrantOperationKind::RawItem || grant.nativeSymbol.has_value()) {
            return;
        }
        if (ootmm::ResolveNativeGrantAction(grant, PortFor(game)).has_value()) {
            return;
        }
        coreUndeliverable.insert(grant.operation.itemId);
    }
};

// Captures a world's outbound packets so the verifier can route them to other players.
struct CapturingNetwork final : public ootmm::INetworkAdapter {
    const ootmm::Seed* seed = nullptr;
    std::vector<std::string>* out = nullptr;
    void SendEvent(const ootmm::NetworkEvent& event) override {
        if (seed != nullptr && out != nullptr) {
            for (const std::string& pkt : ootmm::anchor::BuildPacketsForEvent(*seed, event)) {
                out->push_back(pkt);
            }
        }
    }
};

struct World {
    ootmm::Seed seed;
    std::string state;                 // serialized ledger, carried between game sessions
    std::vector<std::string> inbox;    // GiveItem packets routed here from other worlds
    std::set<std::string> coreUndeliverable;
    std::size_t delivered = 0;
};

// One game session: restore ledger, deliver inbound, collect this game's checks, regrant, drain.
void RunSession(World& w, Game game, std::vector<std::string>& outbound) {
    RecordingAdapter rec(game);
    ootmm::ResolvingGameAdapter resolver(rec, PortFor(game));
    CapturingNetwork net;
    net.seed = &w.seed;
    net.out = &outbound;
    ootmm::PortBridge bridge(resolver, net);
    bridge.LoadSeed(w.seed); // fresh copy; state is authoritative below
    if (!w.state.empty()) {
        bridge.GetRuntime().RestoreState(w.state);
    }

    // Deliver items routed here from other worlds (CheckComplete is own-history only; GiveItems only).
    for (const std::string& pkt : w.inbox) {
        if (const auto ev = ootmm::anchor::ParseGiveItemPacket(pkt); ev.has_value()) {
            bridge.OnRemoteItem(*ev);
        }
    }
    w.inbox.clear();

    // Collect every not-yet-complete check for THIS game only, mirroring a real engine session.
    for (const ootmm::ItemPlacement& p : w.seed.placements) {
        if (p.check.game != game) {
            continue;
        }
        if (!bridge.GetRuntime().IsCheckCompleted(p.check.game, p.check.id)) {
            bridge.OnCheckCollected(game, p.check.id);
        }
    }

    // Cross-game pending items + shared-item mirrors, exactly as the coordinator boot does.
    bridge.GetRuntime().RegrantCrossGamePendingItems(game);
    bridge.GetRuntime().RegrantSharedPersistentItems(game);

    // Drain fully (no throttle — this is offline).
    while (bridge.GetRuntime().QueuedItemCount() > 0) {
        if (bridge.PumpQueuedItems(256) == 0) {
            break;
        }
    }

    w.delivered += rec.delivered;
    w.coreUndeliverable.insert(rec.coreUndeliverable.begin(), rec.coreUndeliverable.end());
    w.state = bridge.GetRuntime().SerializeState();
}

// Route each outbound GiveItem to the addressed player's inbox; returns count routed (0 = fixpoint).
std::size_t RouteBetweenWorlds(std::vector<World>& worlds, std::vector<std::string>& outbound) {
    std::size_t routed = 0;
    for (const std::string& pkt : outbound) {
        const auto ev = ootmm::anchor::ParseGiveItemPacket(pkt);
        if (!ev.has_value()) {
            continue; // CheckComplete etc.: no cross-world delivery
        }
        for (World& w : worlds) {
            if (w.seed.playerId == ev->targetPlayer && ev->targetPlayer != ev->sourcePlayer) {
                w.inbox.push_back(pkt);
                ++routed;
            }
        }
    }
    outbound.clear();
    return routed;
}

bool VerifyGoal(const World& w, const ootmm::Runtime& oot, const ootmm::Runtime& mm) {
    const std::string goal = w.seed.GetStringSetting("goal", "both");
    if (goal == "triforce") {
        const int need = w.seed.GetIntSetting("triforceGoal", 20);
        const std::size_t owned = oot.CountGrantedItemIdOnce("OOT_TRIFORCE") +
                                  mm.CountGrantedItemIdOnce("MM_TRIFORCE") +
                                  oot.CountGrantedItemIdOnce("SHARED_TRIFORCE") +
                                  mm.CountGrantedItemIdOnce("SHARED_TRIFORCE");
        if (owned < static_cast<std::size_t>(need > 0 ? need : 1)) {
            std::cerr << "  [goal] triforce: only " << owned << "/" << need << " pieces obtainable\n";
            return false;
        }
    }
    // Boss-kill goals and triforce3 are generator-guaranteed once every item delivers.
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: ootmm_verify_seed <player1.ootmm.json> [player2 ...]\n";
        return 2;
    }

    std::vector<World> worlds;
    for (int i = 1; i < argc; ++i) {
        World w;
        try {
            w.seed = ootmm::LoadSeedFromFile(argv[i]);
        } catch (const std::exception& e) {
            std::cerr << "[verify] could not load " << argv[i] << ": " << e.what() << "\n";
            return 2;
        }
        worlds.push_back(std::move(w));
    }
    std::cerr << "[verify] " << worlds.size() << " world(s); seed " << worlds.front().seed.seedId << "\n";

    // Alternate OoT/MM sessions, routing cross-world items between rounds, to a bounded fixpoint.
    std::vector<std::string> outbound;
    for (int round = 0; round < 8; ++round) {
        for (World& w : worlds) {
            RunSession(w, Game::Oot, outbound);
            RunSession(w, Game::Mm, outbound);
        }
        const std::size_t routed = RouteBetweenWorlds(worlds, outbound);
        if (routed == 0 && round > 0) {
            break;
        }
    }

    bool allPass = true;
    for (World& w : worlds) {
        const uint16_t me = w.seed.playerId;
        ootmm::Runtime oot;
        oot.LoadSeed(w.seed);
        oot.RestoreState(w.state);
        ootmm::Runtime mm;
        mm.LoadSeed(w.seed);
        mm.RestoreState(w.state);

        int incomplete = 0;
        std::string firstMissing;
        for (const ootmm::ItemPlacement& p : w.seed.placements) {
            if (!oot.IsCheckCompleted(p.check.game, p.check.id)) {
                ++incomplete;
                if (firstMissing.empty()) {
                    firstMissing = std::string(GameName(p.check.game)) + ":" + p.check.id;
                }
            }
        }
        const bool goalOk = VerifyGoal(w, oot, mm);
        const bool pass = incomplete == 0 && goalOk;
        allPass = allPass && pass;

        std::cerr << "[verify] player " << me << ": " << (w.seed.placements.size() - incomplete) << "/"
                  << w.seed.placements.size() << " checks complete, " << w.delivered << " grants delivered, goal("
                  << w.seed.GetStringSetting("goal", "both") << ")=" << (goalOk ? "ok" : "UNREACHABLE") << " — "
                  << (pass ? "PASS" : "FAIL") << "\n";
        if (incomplete != 0) {
            std::cerr << "  [incomplete] " << incomplete << " checks never completed; first: " << firstMissing << "\n";
        }
        if (!w.coreUndeliverable.empty()) {
            std::cerr << "  [warn] " << w.coreUndeliverable.size()
                      << " item id(s) undeliverable at the CORE level (may still deliver via a port table; the "
                         "in-engine delivery-failure counter is authoritative):\n";
            int shown = 0;
            for (const std::string& id : w.coreUndeliverable) {
                if (shown++ >= 40) {
                    std::cerr << "    ... and " << (w.coreUndeliverable.size() - 40) << " more\n";
                    break;
                }
                std::cerr << "    " << id << "\n";
            }
        }
    }

    std::cerr << "[verify] SEED VERDICT: " << (allPass ? "PASS — every world structurally completable"
                                                       : "FAIL — see above")
              << "\n";
    return allPass ? 0 : 1;
}
