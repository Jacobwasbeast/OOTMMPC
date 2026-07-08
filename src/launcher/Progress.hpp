#pragma once

// Progress tab view over Tracker+Seed; pool_[item] = placements owned by seed.playerId (multiworld-correct, no hand max tables)

#include "Tracker.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ootmm::launcher {

enum class SoulSub { Enemy, Boss, Npc, Animal, Misc };
constexpr int kSoulSubCount = 5;

struct DungeonProgress {
    const char* name;       // presentation label (ported from og kDungeonDefs)
    ootmm::Game game;
    std::string smallKeyId; // e.g. "OOT_SMALL_KEY_FOREST"; empty when the dungeon has none
    int keysHave = 0;
    int keysTotal = 0;      // pool_ for the small-key id (individual-key shuffle)
    bool hasKeyRing = false, keyRing = false; // key-ring shuffle for this dungeon (pool_/have_)
    bool skeletonOwned = false;               // this game's skeleton key owned (opens every door)
    bool hasMap = false, hasCompass = false, hasBossKey = false; // pool_>0 for that id
    bool map = false, compass = false, bossKey = false;          // have_>0
    std::string fairyId;    // MM stray-fairy id; empty for OoT / non-temple MM
    int fairiesHave = 0;
    int fairiesTotal = 0;
    bool hasFairies = false;
};

struct ItemProgress {
    std::string id;
    std::string name;
    int have = 0;
    int total = 0;          // pool_ (owned-by-local placements); drives "(not shuffled)" when 0
    bool owned = false;     // have > 0
};

struct ChecksSummary {
    int have = 0, total = 0;
    int ootHave = 0, ootTotal = 0;
    int mmHave = 0, mmTotal = 0;
};

struct GoalProgress {
    const char* name;       // BRIDGE / MOON / LACS / GANON_BK / MAJORA
    int owned = 0;
    int threshold = 0;
    bool active = false;    // threshold > 0
    bool satisfied = false; // owned >= threshold
};

class Progress {
  public:
    // Rebuilds only when tracker.Revision() or seed.seedId changed. O(placements + checks).
    void BuildIfDirty(const ootmm::Seed& seed, const Tracker& tracker);

    [[nodiscard]] const ChecksSummary& Checks() const { return checks_; }
    [[nodiscard]] const std::vector<DungeonProgress>& Dungeons() const { return dungeons_; }
    [[nodiscard]] const std::vector<ItemProgress>& Souls(ootmm::Game game, SoulSub sub) const;
    [[nodiscard]] const std::vector<ItemProgress>& Songs(ootmm::Game game) const {
        return game == ootmm::Game::Oot ? songsOot_ : songsMm_;
    }
    [[nodiscard]] const std::vector<ItemProgress>& KeyItems(ootmm::Game game) const {
        return game == ootmm::Game::Oot ? keyOot_ : keyMm_;
    }
    [[nodiscard]] const std::vector<GoalProgress>& Goals() const { return goals_; }
    [[nodiscard]] int ItemsOwned() const { return itemsOwned_; }

  private:
    void Build(const ootmm::Seed& seed, const Tracker& tracker);
    int Have(const std::string& id) const;
    int Pool(const std::string& id) const;
    const std::string& NameOf(const std::string& id);

    uint64_t builtRevision_ = 0;
    std::string builtSeedId_;

    std::unordered_map<std::string, int> have_;
    std::unordered_map<std::string, int> pool_;
    std::unordered_map<std::string, std::string> names_;
    std::unordered_map<std::string, std::string> fallbackNames_; // title-cased id

    ChecksSummary checks_;
    std::vector<DungeonProgress> dungeons_;
    std::vector<ItemProgress> soulsOot_[kSoulSubCount];
    std::vector<ItemProgress> soulsMm_[kSoulSubCount];
    std::vector<ItemProgress> songsOot_, songsMm_;
    std::vector<ItemProgress> keyOot_, keyMm_;
    std::vector<GoalProgress> goals_;
    int itemsOwned_ = 0;
};

} // namespace ootmm::launcher
