#pragma once

// Reverse-logic solver: evaluates the seed's exported logic graph (generator's pc-seed-logic.ts)
// against the player's CURRENT items and answers which checks are reachable in logic. Faithful
// port of the generator's Pathfinder semantics (logic/pathfind.ts + logic/expr.ts):
//   * areas explored per age (child/adult) with an AreaData mask {ootTime, mmTime, mmTime2, flagsOn, flagsOff},
//   * MM's 3-day clock as a 46-slice bitmask over two u32s, expanded forward on entry (waiting)
//     unless the area has per-slice `stay` conditions,
//   * expression restrictions (negation masks) AND-ed as bitwise OR / OR-ed as bitwise AND,
//     subtracted from the AreaData when traversing an exit,
//   * events grow monotonically; the walk restarts until the event set is stable.
// Item counts are FIXED inputs (what the player holds), not assumed collections — a tracker, not a solver.

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ootmm::launcher {

class LogicSolver {
  public:
    // Parses the "logic" object out of a PC seed file. Returns false (and leaves the
    // solver unloaded) when the seed predates the logic export or fails to parse.
    bool LoadFromSeedFile(const std::filesystem::path& seedPath);

    [[nodiscard]] bool Loaded() const { return loaded_; }
    [[nodiscard]] const std::string& LogicMode() const { return logicMode_; }

    // True when collecting this location (a placement's checkName) grants renewable / license credit.
    [[nodiscard]] bool IsRenewableLocation(const std::string& locationId) const;
    [[nodiscard]] bool IsLicenseLocation(const std::string& locationId) const;

    struct Input {
        std::unordered_map<std::string, int> items;      // itemId -> count owned
        std::unordered_map<std::string, int> renewables; // itemId -> renewable-source count
        std::unordered_map<std::string, int> licenses;   // itemId -> license count
    };

    // Location ids (placement checkNames) reachable in logic. logicMode "none" returns every known
    // location. outEvents (optional) receives the reached event set (diagnostics).
    [[nodiscard]] std::unordered_set<std::string> Solve(const Input& input,
                                                        std::unordered_set<std::string>* outEvents = nullptr) const;

  private:
    enum class Op : uint8_t {
        True,
        False,
        And,
        Or,
        Age,
        Has,
        Renewable,
        License,
        Event,
        Masks,
        Special,
        TimeOot,
        TimeMm,
        Price,
        SongEvent,
        FlagOn,
        FlagOff,
    };

    struct Node {
        Op op = Op::False;
        int32_t a = 0;      // Age: age / Has: count / Masks: count / Price: slot / SongEvent: id
        int32_t b = 0;      // Price: max / SongEvent: cmp
        uint32_t u1 = 0;    // TimeOot: flag / TimeMm: value / FlagOn|Off: bit
        uint32_t u2 = 0;    // TimeMm: value2
        int32_t item = -1;  // Has/Renewable/License: item index
        int32_t event = -1; // Event: event index
        int32_t special = -1;
        std::vector<int32_t> kids; // And/Or
    };

    struct Special {
        std::vector<int32_t> items;
        std::vector<int32_t> itemsUnique;
        int32_t count = 0;
    };

    struct Area {
        bool mm = false;
        int32_t time = 0; // 0 still, 1 day, 2 night, 3 flow
        bool ageChange = false;
        std::vector<std::pair<int32_t, int32_t>> exits;     // (area index, expr index)
        std::vector<std::pair<int32_t, int32_t>> locations; // (location index, expr index)
        std::vector<std::pair<int32_t, int32_t>> events;    // (event index, expr index)
        std::vector<int32_t> stay;                          // per-slice expr, empty = none
    };

    int32_t InternItem(const std::string& id);
    int32_t InternEvent(const std::string& id);

    bool loaded_ = false;
    std::string logicMode_;
    std::vector<Node> nodes_;
    std::vector<Area> areas_;
    std::vector<std::string> locationNames_;
    std::vector<std::string> itemNames_;
    std::vector<std::string> eventNames_;
    std::unordered_map<std::string, int32_t> itemIndex_;
    std::unordered_map<std::string, int32_t> eventIndex_;
    std::vector<Special> specials_;
    std::vector<int32_t> masksRegular_; // item indices
    std::vector<int32_t> prices_;
    std::vector<int32_t> songEvents_;
    std::unordered_set<std::string> renewableLocations_;
    std::unordered_set<std::string> licenseLocations_;
    int32_t spawnArea_ = -1;
    int32_t timeTravelEvent_ = -1; // OOT_TIME_TRAVEL_AT_WILL
};

} // namespace ootmm::launcher
