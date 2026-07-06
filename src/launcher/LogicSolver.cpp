#include "LogicSolver.hpp"

#include "SimpleJson.hpp"

#include <algorithm>
#include <deque>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>

namespace ootmm::launcher {

namespace {

// Mirrors OOT_TIME / MM_TIME_SLICES from the generator's logic/expr.ts.
constexpr uint32_t kOotTimeAll = 0x7; // DAY | NIGHT | DUSK
constexpr uint32_t kOotTimeDay = 1u << 0;
constexpr uint32_t kOotTimeNight = 1u << 1;
constexpr int kMmSliceCount = 46;
constexpr uint32_t kMaskMmTime = 0xffffffffu;
constexpr uint32_t kMaskMmTime2 = (1u << (kMmSliceCount - 32)) - 1u; // 0x3fff

struct AreaData {
    uint32_t ootTime = 0;
    uint32_t mmTime = 0;
    uint32_t mmTime2 = 0;
    uint32_t flagsOn = 0;
    uint32_t flagsOff = 0;
};

AreaData MergeAreaData(const AreaData& a, const AreaData& b) {
    return AreaData{
        a.ootTime | b.ootTime, a.mmTime | b.mmTime, a.mmTime2 | b.mmTime2,
        a.flagsOn & b.flagsOn, a.flagsOff & b.flagsOff,
    };
}

bool CoveringAreaData(const AreaData& a, const AreaData& b) {
    if ((a.ootTime | b.ootTime) != a.ootTime) return false;
    if ((a.mmTime | b.mmTime) != a.mmTime) return false;
    if ((a.mmTime2 | b.mmTime2) != a.mmTime2) return false;
    if ((a.flagsOn & b.flagsOn) != a.flagsOn) return false;
    if ((a.flagsOff & b.flagsOff) != a.flagsOff) return false;
    return true;
}

// ExprRestrictions: NEGATION masks. AND of restrictions = bitwise OR; OR = bitwise AND.
struct Restrictions {
    uint32_t ootTime = 0;
    uint32_t mmTime = 0;
    uint32_t mmTime2 = 0;
    uint32_t flagsOn = 0;
    uint32_t flagsOff = 0;
};

bool IsDefaultRestrictions(const Restrictions& r) {
    return r.ootTime == 0 && r.mmTime == 0 && r.mmTime2 == 0 && r.flagsOn == 0 && r.flagsOff == 0;
}

bool IsRestrictionImpossible(const Restrictions& r) {
    if (r.ootTime == kOotTimeAll) return true;
    if (r.mmTime == 0xffffffffu && r.mmTime2 == 0xffffffffu) return true;
    if (r.flagsOn & r.flagsOff) return true;
    return false;
}

struct EvalResult {
    bool result = false;
    bool restricted = false;
    Restrictions restrictions;
};

} // namespace

int32_t LogicSolver::InternItem(const std::string& id) {
    const auto it = itemIndex_.find(id);
    if (it != itemIndex_.end()) {
        return it->second;
    }
    const int32_t index = static_cast<int32_t>(itemNames_.size());
    itemNames_.push_back(id);
    itemIndex_.emplace(id, index);
    return index;
}

int32_t LogicSolver::InternEvent(const std::string& id) {
    const auto it = eventIndex_.find(id);
    if (it != eventIndex_.end()) {
        return it->second;
    }
    const int32_t index = static_cast<int32_t>(eventNames_.size());
    eventNames_.push_back(id);
    eventIndex_.emplace(id, index);
    return index;
}

bool LogicSolver::LoadFromSeedFile(const std::filesystem::path& seedPath) {
    loaded_ = false;
    nodes_.clear();
    areas_.clear();
    locationNames_.clear();
    itemNames_.clear();
    eventNames_.clear();
    itemIndex_.clear();
    eventIndex_.clear();
    specials_.clear();
    masksRegular_.clear();
    prices_.clear();
    songEvents_.clear();
    renewableLocations_.clear();
    licenseLocations_.clear();
    spawnArea_ = -1;

    std::ifstream input(seedPath, std::ios::binary);
    if (!input) {
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();

    try {
        const json::Value root = json::Parse(buffer.str());
        const json::Value* logic = root.IsObject() ? root.Find("logic") : nullptr;
        if (logic == nullptr || !logic->IsObject()) {
            return false;
        }

        if (const json::Value* mode = logic->Find("logicMode"); mode != nullptr && mode->IsString()) {
            logicMode_ = mode->AsString();
        }

        std::unordered_map<std::string, int32_t> specialIndex;
        const json::Value* specials = logic->Find("specials");
        if (specials != nullptr && specials->IsObject()) {
            for (const auto& [name, value] : specials->AsObject()) {
                Special special;
                if (const json::Value* items = value.Find("items"); items != nullptr && items->IsArray()) {
                    for (const json::Value& item : items->AsArray()) {
                        special.items.push_back(InternItem(item.AsString()));
                    }
                }
                if (const json::Value* items = value.Find("itemsUnique"); items != nullptr && items->IsArray()) {
                    for (const json::Value& item : items->AsArray()) {
                        special.itemsUnique.push_back(InternItem(item.AsString()));
                    }
                }
                if (const json::Value* count = value.Find("count"); count != nullptr && count->IsNumber()) {
                    special.count = static_cast<int32_t>(count->AsNumber());
                }
                specialIndex.emplace(name, static_cast<int32_t>(specials_.size()));
                specials_.push_back(std::move(special));
            }
        }

        if (const json::Value* masks = logic->Find("masksRegular"); masks != nullptr && masks->IsArray()) {
            for (const json::Value& item : masks->AsArray()) {
                masksRegular_.push_back(InternItem(item.AsString()));
            }
        }
        if (const json::Value* prices = logic->Find("prices"); prices != nullptr && prices->IsArray()) {
            for (const json::Value& price : prices->AsArray()) {
                prices_.push_back(price.IsNumber() ? static_cast<int32_t>(price.AsNumber()) : 0);
            }
        }
        if (const json::Value* songs = logic->Find("songEvents"); songs != nullptr && songs->IsArray()) {
            for (const json::Value& song : songs->AsArray()) {
                songEvents_.push_back(song.IsNumber() ? static_cast<int32_t>(song.AsNumber()) : 0);
            }
        }
        if (const json::Value* locs = logic->Find("renewableLocations"); locs != nullptr && locs->IsArray()) {
            for (const json::Value& loc : locs->AsArray()) {
                renewableLocations_.insert(loc.AsString());
            }
        }
        if (const json::Value* locs = logic->Find("licenseLocations"); locs != nullptr && locs->IsArray()) {
            for (const json::Value& loc : locs->AsArray()) {
                licenseLocations_.insert(loc.AsString());
            }
        }

        // --- Expression pool -------------------------------------------------
        const json::Value* exprs = logic->Find("exprs");
        if (exprs == nullptr || !exprs->IsArray()) {
            return false;
        }
        nodes_.reserve(exprs->AsArray().size());
        for (const json::Value& raw : exprs->AsArray()) {
            const auto& arr = raw.AsArray();
            const std::string& op = arr.at(0).AsString();
            Node node;
            if (op == "t") {
                node.op = Op::True;
            } else if (op == "f") {
                node.op = Op::False;
            } else if (op == "and" || op == "or") {
                node.op = op == "and" ? Op::And : Op::Or;
                for (const json::Value& kid : arr.at(1).AsArray()) {
                    node.kids.push_back(static_cast<int32_t>(kid.AsNumber()));
                }
            } else if (op == "age") {
                node.op = Op::Age;
                node.a = static_cast<int32_t>(arr.at(1).AsNumber());
            } else if (op == "has") {
                node.op = Op::Has;
                node.item = InternItem(arr.at(1).AsString());
                node.a = static_cast<int32_t>(arr.at(2).AsNumber());
            } else if (op == "renew") {
                node.op = Op::Renewable;
                node.item = InternItem(arr.at(1).AsString());
            } else if (op == "lic") {
                node.op = Op::License;
                node.item = InternItem(arr.at(1).AsString());
            } else if (op == "ev") {
                node.op = Op::Event;
                node.event = InternEvent(arr.at(1).AsString());
            } else if (op == "masks") {
                node.op = Op::Masks;
                node.a = static_cast<int32_t>(arr.at(2 - 1).AsNumber());
            } else if (op == "special") {
                node.op = Op::Special;
                const auto it = specialIndex.find(arr.at(1).AsString());
                if (it == specialIndex.end()) {
                    return false; // seed inconsistent
                }
                node.special = it->second;
            } else if (op == "oottime") {
                node.op = Op::TimeOot;
                node.u1 = static_cast<uint32_t>(arr.at(1).AsNumber());
            } else if (op == "mmtime") {
                node.op = Op::TimeMm;
                node.u1 = static_cast<uint32_t>(arr.at(1).AsNumber());
                node.u2 = static_cast<uint32_t>(arr.at(2).AsNumber());
            } else if (op == "price") {
                node.op = Op::Price;
                node.a = static_cast<int32_t>(arr.at(1).AsNumber());
                node.b = static_cast<int32_t>(arr.at(2).AsNumber());
            } else if (op == "songev") {
                node.op = Op::SongEvent;
                node.a = static_cast<int32_t>(arr.at(1).AsNumber());
                node.b = static_cast<int32_t>(arr.at(2).AsNumber());
            } else if (op == "flagon") {
                node.op = Op::FlagOn;
                node.u1 = static_cast<uint32_t>(arr.at(1).AsNumber());
            } else if (op == "flagoff") {
                node.op = Op::FlagOff;
                node.u1 = static_cast<uint32_t>(arr.at(1).AsNumber());
            } else {
                return false; // unknown node type: refuse rather than silently mis-solve
            }
            nodes_.push_back(std::move(node));
        }

        // --- Areas ------------------------------------------------------------
        const json::Value* areas = logic->Find("areas");
        if (areas == nullptr || !areas->IsObject()) {
            return false;
        }
        std::unordered_map<std::string, int32_t> areaIndex;
        std::unordered_map<std::string, int32_t> locationIndex;
        for (const auto& [name, value] : areas->AsObject()) {
            areaIndex.emplace(name, static_cast<int32_t>(areaIndex.size()));
        }
        areas_.resize(areaIndex.size());
        for (const auto& [name, value] : areas->AsObject()) {
            Area& area = areas_[areaIndex.at(name)];
            const auto num = [&](const char* key) -> int32_t {
                const json::Value* v = value.Find(key);
                return (v != nullptr && v->IsNumber()) ? static_cast<int32_t>(v->AsNumber()) : 0;
            };
            area.mm = num("g") == 1;
            area.time = num("t");
            area.ageChange = num("a") == 1;

            if (const json::Value* exits = value.Find("x"); exits != nullptr && exits->IsObject()) {
                for (const auto& [target, expr] : exits->AsObject()) {
                    const auto it = areaIndex.find(target);
                    if (it == areaIndex.end()) {
                        continue; // exit into an area the export skipped — unreachable edge
                    }
                    area.exits.emplace_back(it->second, static_cast<int32_t>(expr.AsNumber()));
                }
            }
            if (const json::Value* locations = value.Find("l"); locations != nullptr && locations->IsObject()) {
                for (const auto& [locName, expr] : locations->AsObject()) {
                    auto it = locationIndex.find(locName);
                    if (it == locationIndex.end()) {
                        it = locationIndex.emplace(locName, static_cast<int32_t>(locationNames_.size())).first;
                        locationNames_.push_back(locName);
                    }
                    area.locations.emplace_back(it->second, static_cast<int32_t>(expr.AsNumber()));
                }
            }
            if (const json::Value* events = value.Find("e"); events != nullptr && events->IsObject()) {
                for (const auto& [eventName, expr] : events->AsObject()) {
                    area.events.emplace_back(InternEvent(eventName), static_cast<int32_t>(expr.AsNumber()));
                }
            }
            if (const json::Value* stay = value.Find("s"); stay != nullptr && stay->IsArray()) {
                for (const json::Value& expr : stay->AsArray()) {
                    area.stay.push_back(static_cast<int32_t>(expr.AsNumber()));
                }
            }
        }

        const auto spawn = areaIndex.find("OOT SPAWN");
        if (spawn == areaIndex.end()) {
            return false;
        }
        spawnArea_ = spawn->second;
        timeTravelEvent_ = InternEvent("OOT_TIME_TRAVEL_AT_WILL");
        loaded_ = true;
        return true;
    } catch (...) {
        return false;
    }
}

bool LogicSolver::IsRenewableLocation(const std::string& locationId) const {
    return renewableLocations_.contains(locationId);
}

bool LogicSolver::IsLicenseLocation(const std::string& locationId) const {
    return licenseLocations_.contains(locationId);
}

namespace {

struct SolveState {
    std::vector<int> items;      // by item index
    std::vector<int> renewables; // by item index
    std::vector<int> licenses;   // by item index
    std::vector<uint8_t> events; // by event index
    int age = 0;
    const AreaData* areaData = nullptr;
};

} // namespace

std::unordered_set<std::string> LogicSolver::Solve(const Input& input,
                                                   std::unordered_set<std::string>* outEvents) const {
    std::unordered_set<std::string> reachable;
    if (!loaded_) {
        return reachable;
    }
    if (logicMode_ == "none") {
        reachable.insert(locationNames_.begin(), locationNames_.end());
        return reachable;
    }

    SolveState st;
    st.items.assign(itemNames_.size(), 0);
    st.renewables.assign(itemNames_.size(), 0);
    st.licenses.assign(itemNames_.size(), 0);
    st.events.assign(eventNames_.size(), 0);
    const auto fill = [&](const std::unordered_map<std::string, int>& src, std::vector<int>& dst) {
        for (const auto& [id, count] : src) {
            const auto it = itemIndex_.find(id);
            if (it != itemIndex_.end()) {
                dst[static_cast<size_t>(it->second)] += count;
            }
        }
    };
    fill(input.items, st.items);
    fill(input.renewables, st.renewables);
    fill(input.licenses, st.licenses);

    // Recursive expression evaluation (depth is small: the generator's ASTs are shallow).
    const std::function<EvalResult(int32_t)> evalExpr = [&](int32_t index) -> EvalResult {
        const Node& node = nodes_[static_cast<size_t>(index)];
        switch (node.op) {
        case Op::True:
            return { true, false, {} };
        case Op::False:
            return { false, false, {} };
        case Op::And: {
            Restrictions r; // AND of restrictions == bitwise OR (they are negations)
            bool restricted = false;
            for (const int32_t kid : node.kids) {
                const EvalResult kr = evalExpr(kid);
                if (!kr.result) {
                    return { false, false, {} };
                }
                if (kr.restricted) {
                    restricted = true;
                    r.ootTime |= kr.restrictions.ootTime;
                    r.mmTime |= kr.restrictions.mmTime;
                    r.mmTime2 |= kr.restrictions.mmTime2;
                    r.flagsOn |= kr.restrictions.flagsOn;
                    r.flagsOff |= kr.restrictions.flagsOff;
                }
            }
            if (restricted && IsRestrictionImpossible(r)) {
                return { false, false, {} };
            }
            if (!restricted || IsDefaultRestrictions(r)) {
                return { true, false, {} };
            }
            return { true, true, r };
        }
        case Op::Or: {
            Restrictions r{ kOotTimeAll, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu };
            bool any = false;
            for (const int32_t kid : node.kids) {
                const EvalResult kr = evalExpr(kid);
                if (!kr.result) {
                    continue;
                }
                if (!kr.restricted) {
                    return { true, false, {} }; // unrestricted true short-circuits
                }
                any = true;
                r.ootTime &= kr.restrictions.ootTime;
                r.mmTime &= kr.restrictions.mmTime;
                r.mmTime2 &= kr.restrictions.mmTime2;
                r.flagsOn &= kr.restrictions.flagsOn;
                r.flagsOff &= kr.restrictions.flagsOff;
            }
            if (!any) {
                return { false, false, {} };
            }
            if (IsDefaultRestrictions(r)) {
                return { true, false, {} };
            }
            return { true, true, r };
        }
        case Op::Age:
            return { st.age == node.a, false, {} };
        case Op::Has:
            return { st.items[static_cast<size_t>(node.item)] >= node.a, false, {} };
        case Op::Renewable:
            return { st.renewables[static_cast<size_t>(node.item)] > 0, false, {} };
        case Op::License:
            return { st.licenses[static_cast<size_t>(node.item)] > 0, false, {} };
        case Op::Event:
            return { st.events[static_cast<size_t>(node.event)] != 0, false, {} };
        case Op::Masks: {
            int sum = 0;
            for (const int32_t item : masksRegular_) {
                sum += st.items[static_cast<size_t>(item)];
            }
            return { sum >= node.a, false, {} };
        }
        case Op::Special: {
            const Special& special = specials_[static_cast<size_t>(node.special)];
            int sum = 0;
            for (const int32_t item : special.items) {
                sum += st.items[static_cast<size_t>(item)];
            }
            for (const int32_t item : special.itemsUnique) {
                if (st.items[static_cast<size_t>(item)] > 0) {
                    ++sum;
                }
            }
            return { sum >= special.count, false, {} };
        }
        case Op::TimeOot:
            if (st.areaData->ootTime & node.u1) {
                Restrictions r;
                r.ootTime = kOotTimeAll & ~node.u1;
                return { true, true, r };
            }
            return { false, false, {} };
        case Op::TimeMm:
            if ((st.areaData->mmTime & node.u1) || (st.areaData->mmTime2 & node.u2)) {
                Restrictions r;
                r.mmTime = ~node.u1;
                r.mmTime2 = ~node.u2;
                return { true, true, r };
            }
            return { false, false, {} };
        case Op::Price: {
            const int32_t price =
                node.a >= 0 && node.a < static_cast<int32_t>(prices_.size()) ? prices_[static_cast<size_t>(node.a)] : 0;
            return { price <= node.b, false, {} };
        }
        case Op::SongEvent: {
            const int32_t value = node.a >= 0 && node.a < static_cast<int32_t>(songEvents_.size())
                                      ? songEvents_[static_cast<size_t>(node.a)]
                                      : 0;
            return { value == node.b, false, {} };
        }
        case Op::FlagOn:
            if (st.areaData->flagsOff & node.u1) {
                return { false, false, {} };
            }
            {
                Restrictions r;
                r.flagsOn = node.u1;
                return { true, true, r };
            }
        case Op::FlagOff:
            if (st.areaData->flagsOn & node.u1) {
                return { false, false, {} };
            }
            {
                Restrictions r;
                r.flagsOff = node.u1;
                return { true, true, r };
            }
        }
        return { false, false, {} };
    };

    std::vector<uint8_t> reachedLoc(locationNames_.size(), 0);

    struct QueueEntry {
        int age;
        int32_t area;
        AreaData data;
        int32_t fromArea;
    };

    // Events grow monotonically; each pass re-explores with the accumulated event set until no new
    // event appears (bounded by the longest event-dependency chain, small).
    bool eventsChanged = true;
    int guard = 0;
    while (eventsChanged && guard++ < 512) {
        eventsChanged = false;
        std::fill(reachedLoc.begin(), reachedLoc.end(), 0);
        std::vector<std::optional<AreaData>> reached[2];
        reached[0].assign(areas_.size(), std::nullopt);
        reached[1].assign(areas_.size(), std::nullopt);

        std::deque<QueueEntry> queue;
        AreaData init;
        init.mmTime = 1; // day 1, 6 AM
        queue.push_back({ 0, spawnArea_, init, spawnArea_ });
        queue.push_back({ 1, spawnArea_, init, spawnArea_ });

        while (!queue.empty()) {
            QueueEntry entry = queue.front();
            queue.pop_front();
            const Area& area = areas_[static_cast<size_t>(entry.area)];
            std::optional<AreaData>& slot = reached[entry.age][static_cast<size_t>(entry.area)];

            AreaData newData = slot.has_value() ? MergeAreaData(*slot, entry.data) : entry.data;

            if (!area.mm) {
                switch (area.time) {
                case 1:
                    newData.ootTime |= kOotTimeDay;
                    break;
                case 2:
                    newData.ootTime |= kOotTimeNight;
                    break;
                case 3:
                    newData.ootTime |= kOotTimeAll;
                    break;
                default:
                    break;
                }
            } else {
                // MM: unreachable without a time slice; otherwise expand forward (waiting).
                if (newData.mmTime == 0 && newData.mmTime2 == 0) {
                    continue;
                }
                if (area.stay.empty()) {
                    uint32_t mmTime;
                    uint32_t mmTime2;
                    if (newData.mmTime) {
                        mmTime = newData.mmTime;
                        mmTime |= mmTime << 1;
                        mmTime |= mmTime << 2;
                        mmTime |= mmTime << 4;
                        mmTime |= mmTime << 8;
                        mmTime |= mmTime << 16;
                        mmTime2 = 0xffffffffu;
                    } else {
                        mmTime2 = newData.mmTime2;
                        mmTime2 |= mmTime2 << 1;
                        mmTime2 |= mmTime2 << 2;
                        mmTime2 |= mmTime2 << 4;
                        mmTime2 |= mmTime2 << 8;
                        mmTime2 |= mmTime2 << 16;
                        mmTime = 0;
                    }
                    newData.mmTime = mmTime & kMaskMmTime;
                    newData.mmTime2 = mmTime2 & kMaskMmTime2;
                } else {
                    // Conditional waiting: extend slice by slice where `stay` holds. Stay expressions
                    // evaluate against the PREVIOUS area data (mirroring the generator's not-yet-updated state).
                    static const AreaData kZeroAreaData{};
                    const AreaData& stayData = slot.has_value() ? *slot : kZeroAreaData;
                    st.age = entry.age;
                    st.areaData = &stayData;
                    bool waitMode = false;
                    for (int i = 0; i < kMmSliceCount && i < static_cast<int>(area.stay.size()); ++i) {
                        const uint32_t mask1 = i < 32 ? (1u << i) : 0;
                        const uint32_t mask2 = i < 32 ? 0 : (1u << (i - 32));
                        if ((newData.mmTime & mask1) || (newData.mmTime2 & mask2)) {
                            waitMode = true;
                        } else if (waitMode) {
                            const EvalResult stay = evalExpr(area.stay[static_cast<size_t>(i)]);
                            if (stay.result) {
                                newData.mmTime |= mask1;
                                newData.mmTime2 |= mask2;
                            } else {
                                waitMode = false;
                            }
                        }
                    }
                }
            }

            // Age swap (time travel at will), before the covering check like the generator.
            if (timeTravelEvent_ >= 0 && st.events[static_cast<size_t>(timeTravelEvent_)] && area.ageChange &&
                entry.area != entry.fromArea) {
                queue.push_back({ entry.age ^ 1, entry.area, newData, entry.area });
            }

            if (slot.has_value() && CoveringAreaData(*slot, newData)) {
                continue;
            }
            slot = newData;

            st.age = entry.age;
            st.areaData = &*slot;

            for (const auto& [locIndex, exprIndex] : area.locations) {
                if (!reachedLoc[static_cast<size_t>(locIndex)] && evalExpr(exprIndex).result) {
                    reachedLoc[static_cast<size_t>(locIndex)] = 1;
                }
            }
            for (const auto& [eventIndex, exprIndex] : area.events) {
                if (!st.events[static_cast<size_t>(eventIndex)] && evalExpr(exprIndex).result) {
                    st.events[static_cast<size_t>(eventIndex)] = 1;
                    eventsChanged = true;
                }
            }
            for (const auto& [targetArea, exprIndex] : area.exits) {
                const EvalResult exit = evalExpr(exprIndex);
                if (!exit.result) {
                    continue;
                }
                AreaData exitData = *slot;
                if (exit.restricted) {
                    exitData.ootTime &= ~exit.restrictions.ootTime;
                    exitData.mmTime &= ~exit.restrictions.mmTime;
                    exitData.mmTime2 &= ~exit.restrictions.mmTime2;
                    exitData.flagsOn |= exit.restrictions.flagsOn;
                    exitData.flagsOff |= exit.restrictions.flagsOff;
                }
                // OoT -> MM edge: the Song of Time resets to day 1, 6 AM.
                if (!area.mm && areas_[static_cast<size_t>(targetArea)].mm) {
                    exitData.mmTime |= 1;
                }
                const std::optional<AreaData>& targetSlot = reached[entry.age][static_cast<size_t>(targetArea)];
                if (targetSlot.has_value() && CoveringAreaData(*targetSlot, exitData) &&
                    areas_[static_cast<size_t>(targetArea)].stay.empty()) {
                    continue; // nothing new to propagate
                }
                queue.push_back({ entry.age, targetArea, exitData, entry.area });
            }
        }
    }

    for (size_t i = 0; i < reachedLoc.size(); ++i) {
        if (reachedLoc[i]) {
            reachable.insert(locationNames_[i]);
        }
    }
    if (outEvents != nullptr) {
        for (size_t i = 0; i < st.events.size(); ++i) {
            if (st.events[i]) {
                outEvents->insert(eventNames_[i]);
            }
        }
    }
    return reachable;
}

} // namespace ootmm::launcher
