#pragma once

// Launcher-side check tracker. Model built from the seed's placements (check location + item
// placed); completed set reconciled from state.json ("completedChecks": ["oot:CHECK", ...]); live
// collections stream in as OOTMM_CHECK_COMPLETE packets and also feed the chronological check log.

#include "ootmm/Seed.hpp"
#include "ootmm/Types.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace ootmm::launcher {

struct TrackerCheck {
    Game game = Game::Oot;
    std::string checkId;
    std::string name;     // human-readable when the seed provides one, else the id
    std::string type;     // "chest", "collectible", "gs", "npc", ...
    std::string itemId;   // item placed here (this world's placement)
    std::string itemName;
    uint16_t ownerPlayer = 1;
    bool collected = false;
    bool inLogic = false; // reachable with the current items, per the seed's logic graph
};

struct CheckLogEntry {
    std::string time; // local wall-clock "HH:MM:SS"
    Game game = Game::Oot;
    std::string checkName;
    std::string itemName;
    uint16_t targetPlayer = 1;
    bool sentToRemote = false;
};

class Tracker {
  public:
    void LoadSeed(const Seed& seed);

    // Marks a check collected (idempotent). When `logIt` is set (live packet), a
    // check-log entry is appended. Unknown checks are tracked in the log anyway so
    // nothing silently disappears.
    void MarkCollected(Game game, const std::string& checkId, const std::string& itemName,
                       uint16_t targetPlayer, uint16_t localPlayer, bool logIt);

    // Reconciles the collected set from state.json's completedChecks array.
    void SyncFromStateFile(const std::filesystem::path& statePath);

    // Records a multiworld item delivered TO the local player from another world
    // (deduplicated by delivery key, mirroring the runtime's own dedup). Returns true
    // when the receipt was new.
    bool AddRemoteReceipt(const std::string& itemId, const std::string& deliveryKey);

    // The local player's current item counts, reconstructed from durable sources:
    // seed starting items + collected checks whose placement the local player owns +
    // recorded multiworld receipts. This is what the logic solver evaluates against.
    [[nodiscard]] std::unordered_map<std::string, int> ItemCounts(const Seed& seed) const;

    [[nodiscard]] std::vector<TrackerCheck>& Checks() { return checks_; }
    [[nodiscard]] const std::vector<TrackerCheck>& Checks() const { return checks_; }
    [[nodiscard]] const std::vector<CheckLogEntry>& Log() const { return log_; }
    [[nodiscard]] int CollectedCount() const { return collectedCount_; }
    [[nodiscard]] int TotalCount() const { return static_cast<int>(checks_.size()); }
    // Bumped on every collection/receipt so the solver knows when to re-run.
    [[nodiscard]] uint64_t Revision() const { return revision_; }

  private:
    std::vector<TrackerCheck> checks_;
    std::unordered_map<std::string, size_t> byKey_; // "oot:CHECKID" -> index
    std::vector<CheckLogEntry> log_;
    std::unordered_map<std::string, int> remoteCounts_;   // itemId -> received count
    std::unordered_map<std::string, bool> remoteReceipts_; // deliveryKey -> seen
    int collectedCount_ = 0;
    uint64_t revision_ = 1;
};

} // namespace ootmm::launcher
