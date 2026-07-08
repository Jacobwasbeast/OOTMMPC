#include "Progress.hpp"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <unordered_set>

namespace ootmm::launcher {

// ---------------------------------------------------------------------------
// Static presentation tables (all counts derived from have_/pool_)
// ---------------------------------------------------------------------------

// og kDungeonDefs (menu.c) minus ROM ids; token = seed item-id suffix for this dungeon
struct DungeonDef {
    const char* name;
    ootmm::Game game;
    const char* token;
};
static const DungeonDef kDungeons[] = {
    { "Deku Tree", ootmm::Game::Oot, "DT" },
    { "Dodongo's Cavern", ootmm::Game::Oot, "DC" },
    { "Jabu Jabu", ootmm::Game::Oot, "JJ" },
    { "Forest Temple", ootmm::Game::Oot, "FOREST" },
    { "Fire Temple", ootmm::Game::Oot, "FIRE" },
    { "Water Temple", ootmm::Game::Oot, "WATER" },
    { "Spirit Temple", ootmm::Game::Oot, "SPIRIT" },
    { "Shadow Temple", ootmm::Game::Oot, "SHADOW" },
    { "Bottom of the Well", ootmm::Game::Oot, "BOTW" },
    { "Ice Cavern", ootmm::Game::Oot, "IC" },
    { "Gerudo Fortress", ootmm::Game::Oot, "GF" },
    { "Gerudo Training", ootmm::Game::Oot, "GTG" },
    { "Chest Game", ootmm::Game::Oot, "TCG" },
    { "Ganon's Castle", ootmm::Game::Oot, "GANON" },
    { "Woodfall Temple", ootmm::Game::Mm, "WF" },
    { "Snowhead Temple", ootmm::Game::Mm, "SH" },
    { "Great Bay Temple", ootmm::Game::Mm, "GB" },
    { "Stone Tower Temple", ootmm::Game::Mm, "ST" },
    { "Clock Town", ootmm::Game::Mm, "TOWN" },
};

// mirrors Ootmm2s2hMoonConditionSatisfied field set; masks/triforce intentionally excluded
struct GoalField {
    const char* field;
    std::initializer_list<const char*> ids;
};
static const GoalField kGoalFields[] = {
    { "stones", { "OOT_STONE_EMERALD", "OOT_STONE_RUBY", "OOT_STONE_SAPPHIRE" } },
    { "medallions", { "OOT_MEDALLION_FOREST", "OOT_MEDALLION_FIRE", "OOT_MEDALLION_WATER",
                      "OOT_MEDALLION_SPIRIT", "OOT_MEDALLION_SHADOW", "OOT_MEDALLION_LIGHT" } },
    { "remains", { "MM_REMAINS_ODOLWA", "MM_REMAINS_GOHT", "MM_REMAINS_GYORG", "MM_REMAINS_TWINMOLD" } },
    { "skullsGold", { "OOT_GS_TOKEN" } },
    { "skullsSwamp", { "MM_GS_TOKEN_SWAMP" } },
    { "skullsOcean", { "MM_GS_TOKEN_OCEAN" } },
    { "fairiesWF", { "MM_STRAY_FAIRY_WF" } },
    { "fairiesSH", { "MM_STRAY_FAIRY_SH" } },
    { "fairiesGB", { "MM_STRAY_FAIRY_GB" } },
    { "fairiesST", { "MM_STRAY_FAIRY_ST" } },
    { "fairyTown", { "MM_STRAY_FAIRY_TOWN" } },
};
static const char* kGoalNames[] = { "BRIDGE", "MOON", "LACS", "GANON_BK", "MAJORA" };

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

int Progress::Have(const std::string& id) const {
    const auto it = have_.find(id);
    return it != have_.end() ? it->second : 0;
}

int Progress::Pool(const std::string& id) const {
    const auto it = pool_.find(id);
    return it != pool_.end() ? it->second : 0;
}

const std::string& Progress::NameOf(const std::string& id) {
    const auto it = names_.find(id);
    if (it != names_.end()) {
        return it->second;
    }
    // Fallback: underscores -> spaces. Cached so the returned reference is stable.
    std::string& fb = fallbackNames_[id];
    if (fb.empty()) {
        fb.reserve(id.size());
        for (char c : id) {
            fb.push_back(c == '_' ? ' ' : c);
        }
    }
    return fb;
}

static bool Starts(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

static bool ClassifySoul(const std::string& id, ootmm::Game& game, SoulSub& sub) {
    const char* subs[] = { "SOUL_ENEMY_", "SOUL_BOSS_", "SOUL_NPC_", "SOUL_ANIMAL_", "SOUL_MISC_" };
    const ootmm::Game games[2] = { ootmm::Game::Oot, ootmm::Game::Mm };
    const char* prefixes[2] = { "OOT_", "MM_" };
    for (int g = 0; g < 2; ++g) {
        std::string p = std::string(prefixes[g]) + subs[0];
        if (!Starts(id, p.c_str())) {
            // still might be another sub under this game
            bool any = false;
            for (int s = 0; s < kSoulSubCount; ++s) {
                if (Starts(id, (std::string(prefixes[g]) + subs[s]).c_str())) {
                    any = true;
                    break;
                }
            }
            if (!any) {
                continue;
            }
        }
        for (int s = 0; s < kSoulSubCount; ++s) {
            if (Starts(id, (std::string(prefixes[g]) + subs[s]).c_str())) {
                game = games[g];
                sub = static_cast<SoulSub>(s);
                return true;
            }
        }
    }
    return false;
}

static ootmm::Game GameOf(const std::string& id) {
    return Starts(id, "OOT_") ? ootmm::Game::Oot : ootmm::Game::Mm;
}

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

void Progress::BuildIfDirty(const ootmm::Seed& seed, const Tracker& tracker) {
    if (builtRevision_ == tracker.Revision() && builtSeedId_ == seed.seedId) {
        return;
    }
    Build(seed, tracker);
    builtRevision_ = tracker.Revision();
    builtSeedId_ = seed.seedId;
}

void Progress::Build(const ootmm::Seed& seed, const Tracker& tracker) {
    // Current owned counts + the local player's collectible pool + display names + major set.
    have_ = tracker.ItemCounts(seed);
    pool_.clear();
    names_.clear();
    fallbackNames_.clear();
    std::unordered_set<std::string> majorIds;
    for (const ootmm::ItemPlacement& p : seed.placements) {
        if (p.ownerPlayer != seed.playerId || p.item.id.empty()) {
            continue;
        }
        pool_[p.item.id]++;
        if (names_.find(p.item.id) == names_.end() && !p.item.name.empty()) {
            names_[p.item.id] = p.item.name;
        }
        if (p.major) {
            majorIds.insert(p.item.id);
        }
    }

    itemsOwned_ = 0;
    for (const auto& [id, n] : have_) {
        itemsOwned_ += n;
    }

    // Checks summary (+ per-game split).
    checks_ = {};
    checks_.have = tracker.CollectedCount();
    checks_.total = tracker.TotalCount();
    for (const TrackerCheck& c : tracker.Checks()) {
        if (c.game == ootmm::Game::Oot) {
            checks_.ootTotal++;
            if (c.collected) {
                checks_.ootHave++;
            }
        } else {
            checks_.mmTotal++;
            if (c.collected) {
                checks_.mmHave++;
            }
        }
    }

    // Dungeons: one row per roster entry; every field derived from have_/pool_.
    dungeons_.clear();
    for (const DungeonDef& d : kDungeons) {
        DungeonProgress dp{};
        dp.name = d.name;
        dp.game = d.game;
        const bool oot = d.game == ootmm::Game::Oot;
        const std::string pre = oot ? "OOT_" : "MM_";
        dp.smallKeyId = pre + "SMALL_KEY_" + d.token;
        const std::string bossId = pre + "BOSS_KEY_" + d.token;
        const std::string mapId = pre + "MAP_" + d.token;
        const std::string compassId = oot ? ("OOT_COMPASS_" + std::string(d.token)) : std::string{};
        const std::string fairyId = oot ? std::string{} : ("MM_STRAY_FAIRY_" + std::string(d.token));

        dp.keysTotal = Pool(dp.smallKeyId);
        dp.keysHave = Have(dp.smallKeyId);
        const std::string ringId = pre + "KEY_RING_" + d.token;
        dp.hasKeyRing = Pool(ringId) > 0;
        dp.keyRing = Have(ringId) > 0;
        dp.skeletonOwned = Have(oot ? "OOT_SKELETON_KEY" : "MM_SKELETON_KEY") > 0;
        dp.hasBossKey = Pool(bossId) > 0;
        dp.bossKey = Have(bossId) > 0;
        dp.hasMap = Pool(mapId) > 0;
        dp.map = Have(mapId) > 0;
        if (oot) {
            dp.hasCompass = Pool(compassId) > 0;
            dp.compass = Have(compassId) > 0;
        } else {
            dp.fairyId = fairyId;
            dp.hasFairies = Pool(fairyId) > 0;
            dp.fairiesTotal = Pool(fairyId);
            dp.fairiesHave = Have(fairyId);
        }
        // Hide dungeons that contribute nothing in this seed (e.g. a token with no items at all).
        const bool anything = dp.keysTotal > 0 || dp.hasKeyRing || dp.hasBossKey || dp.hasMap ||
                              dp.hasCompass || dp.hasFairies;
        if (anything) {
            dungeons_.push_back(std::move(dp));
        }
    }

    // Souls: classify the local pool's soul ids by game + sub-category.
    for (auto& v : soulsOot_) {
        v.clear();
    }
    for (auto& v : soulsMm_) {
        v.clear();
    }
    for (const auto& [id, total] : pool_) {
        ootmm::Game game{};
        SoulSub sub{};
        if (!ClassifySoul(id, game, sub)) {
            continue;
        }
        const int have = Have(id);
        ItemProgress ip{};
        ip.id = id;
        ip.name = NameOf(id);
        ip.have = have;
        ip.total = total;
        ip.owned = have > 0;
        (game == ootmm::Game::Oot ? soulsOot_ : soulsMm_)[static_cast<int>(sub)].push_back(std::move(ip));
    }

    // Songs (exclude the _NOTE_ shards from the song list).
    songsOot_.clear();
    songsMm_.clear();
    for (const auto& [id, total] : pool_) {
        const bool oot = Starts(id, "OOT_SONG_");
        const bool mm = Starts(id, "MM_SONG_");
        if (!oot && !mm) {
            continue;
        }
        if (id.find("_SONG_NOTE_") != std::string::npos) {
            continue;
        }
        const int have = Have(id);
        ItemProgress ip{};
        ip.id = id;
        ip.name = NameOf(id);
        ip.have = have;
        ip.total = total;
        ip.owned = have > 0;
        (oot ? songsOot_ : songsMm_).push_back(std::move(ip));
    }

    // Key items: distinct major items in the local pool, grouped by game.
    keyOot_.clear();
    keyMm_.clear();
    for (const auto& [id, total] : pool_) {
        if (!majorIds.count(id)) {
            continue;
        }
        const int have = Have(id);
        ItemProgress ip{};
        ip.id = id;
        ip.name = NameOf(id);
        ip.have = have;
        ip.total = total;
        ip.owned = have > 0;
        (GameOf(id) == ootmm::Game::Oot ? keyOot_ : keyMm_).push_back(std::move(ip));
    }

    // Goals: each special cond's owned sum vs threshold (ported field set).
    goals_.clear();
    for (const char* condName : kGoalNames) {
        const ootmm::Seed::SpecialCond* c = seed.GetSpecialCond(condName);
        if (c == nullptr) {
            continue;
        }
        GoalProgress gp{};
        gp.name = condName;
        gp.threshold = c->count;
        gp.active = c->count > 0;
        if (gp.active) {
            int owned = 0;
            for (const GoalField& gf : kGoalFields) {
                if (c->Field(gf.field)) {
                    for (const char* id : gf.ids) {
                        owned += Have(id);
                    }
                }
            }
            gp.owned = owned;
            gp.satisfied = owned >= c->count;
        }
        goals_.push_back(gp);
    }
}

const std::vector<ItemProgress>& Progress::Souls(ootmm::Game game, SoulSub sub) const {
    const int idx = static_cast<int>(sub);
    return game == ootmm::Game::Oot ? soulsOot_[idx] : soulsMm_[idx];
}

} // namespace ootmm::launcher
