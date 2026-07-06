#pragma once

#include "ootmm/Types.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ootmm {

struct Seed {
    static constexpr const char* CurrentFormat = "ootmm.pc.seed.v1";

    std::string format;
    std::string seedId;
    std::string settingsHash;
    uint16_t playerId = 1;
    uint16_t playerCount = 1;
    std::string teamId = "default";
    std::vector<ItemPlacement> placements;
    std::vector<EntranceMapping> entrances;
    std::vector<StartingItem> startingItems;

    // The OoTMM settings/world flags, so the ports can honor gameplay options
    // (open forest, door of time, dungeons, trials, goals, ...) — the same settings
    // the website used. Keyed by the OoTMM setting key. Three scalar maps cover the
    // three JSON value kinds the settings object uses (bool / enum-string / number).
    std::map<std::string, bool> boolSettings;
    std::map<std::string, std::string> stringSettings;
    std::map<std::string, double> numberSettings;
    [[nodiscard]] bool GetBoolSetting(const std::string& key, bool fallback = false) const;
    [[nodiscard]] std::string GetStringSetting(const std::string& key, const std::string& fallback = "") const;
    [[nodiscard]] int GetIntSetting(const std::string& key, int fallback = 0) const;

    // A resolved OoTMM "world flag" set (e.g. ganonTrials, openDungeonsOot). In the
    // seed JSON each is "none" (empty), "all" (every member), or an explicit member
    // list. `all` is preserved so membership/count queries stay correct without the
    // port needing to know each flag's full member roster.
    struct WorldFlag {
        bool all = false;
        std::vector<std::string> members;
    };
    std::map<std::string, WorldFlag> worldFlags;
    [[nodiscard]] const WorldFlag* GetWorldFlag(const std::string& key) const;
    [[nodiscard]] bool WorldFlagContains(const std::string& key, const std::string& member) const;
    // Number of selected members; when the flag resolved to "all", returns
    // `totalIfAll` (the flag's full roster size, which the caller knows).
    [[nodiscard]] int WorldFlagCount(const std::string& key, int totalIfAll) const;

    // An OoTMM "special condition" (BRIDGE / MOON / LACS / GANON_BK / MAJORA): a
    // configurable threshold `count` plus per-category booleans (stones, medallions,
    // remains, skulls*, fairies*, masks*, triforce, coins*) that select which owned
    // items are summed toward the threshold.
    struct SpecialCond {
        int count = 0;
        std::map<std::string, bool> fields;
        [[nodiscard]] bool Field(const std::string& name) const;
    };
    std::map<std::string, SpecialCond> specialConds;
    [[nodiscard]] const SpecialCond* GetSpecialCond(const std::string& name) const;

    // --- og OoTMM hints (seed `hints` section) --------------------------------------------
    // Numeric ids match og's ROM encoding (region id u8 with MM high bit; named-check id u8
    // with MM high bit; path ids 0..5), so the text builder is a verbatim port of hint.c.
    struct HintRegion {
        std::string name; // REGIONS key ('NONE' when absent)
        int regionId = 0; // og numeric region id
        int world = 0;    // 0-based
    };
    struct HintItem {
        std::string itemId;
        std::string itemName;
        int player = 0;      // 0 = all, else 1-based
        int importance = -1; // -1 none, 0 unreachable, 1 not required, 2 sometimes, 3 required
    };
    struct GossipHint {
        std::string stone;
        Game stoneGame = Game::Oot;
        int stoneKey = 0;      // raw id | 0x20 grotto | 0x40 moon (the actor's runtime key)
        std::string type;      // path / foolish / item-exact / item-region / junk
        HintRegion region;     // path / foolish / item-region
        int pathId = -1;       // path: 0 woth, 1 triforce, 2 dungeon, 3 boss, 4 end-boss, 5 event
        int pathSubId = -1;
        int pathPlayer = 0;    // 0 = all, else 1-based
        int checkId = 0;       // item-exact: og named-check id
        std::string checkName; // item-exact
        int checkWorld = 0;    // item-exact: 1-based
        std::vector<HintItem> items; // item-exact (1..3) / item-region (1)
        int junkId = 0;        // junk
    };
    struct SeedHints {
        std::vector<GossipHint> gossip;
        std::vector<HintRegion> dungeonRewards; // 13
        HintRegion lightArrow;
        std::vector<HintRegion> oathToOrder;
        HintRegion ganonBossKey; // name == "NONE" unless ganonBossKey === 'anywhere'
        std::vector<int> staticImportances;
        [[nodiscard]] const GossipHint* FindGossip(Game game, int stoneKey) const;
    };
    SeedHints hints;

    // Which game the player begins in. Optional in the seed: when absent it is
    // inferred from the starting items (all-MM => Mm, else Oot).
    std::optional<Game> startingGame;

    [[nodiscard]] const ItemPlacement* FindPlacement(Game game, const std::string& checkId) const;
    [[nodiscard]] const EntranceMapping* FindEntrance(Game fromGame, const std::string& from) const;
    [[nodiscard]] const EntranceMapping* FindEntrance(Game fromGame, uint32_t fromNativeId) const;
    // Resolves the concrete starting game: startingGame if present, else inferred
    // from startingItems (Mm only when at least one starting item exists and every
    // one is an MM item), defaulting to Oot.
    [[nodiscard]] Game ResolveStartingGame() const;
    [[nodiscard]] bool IsCompatible() const;
};

class SeedLoadError final : public std::runtime_error {
  public:
    explicit SeedLoadError(const std::string& message);
};

[[nodiscard]] Seed LoadSeedFromJson(const std::string& jsonText);
[[nodiscard]] Seed LoadSeedFromFile(const std::filesystem::path& path);

} // namespace ootmm
