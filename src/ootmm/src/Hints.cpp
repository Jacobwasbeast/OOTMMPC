// og OoTMM hint text builder. String tables are verbatim ports of the generator's
// src/common/text/text.c / text_dungeon.c / hint.c (colors as @-markup, see Hints.hpp).

#include "ootmm/Hints.hpp"

#include <cctype>
#include <map>

namespace ootmm::hints {

namespace {

struct RegionName {
    const char* prepos;
    const char* name;
};

// text.c kRegionNamesOot (index = regionId - 1).
const RegionName kRegionNamesOot[38] = {
    { "in", "the @YSacred Realm" },
    { "inside", "the @GDeku Tree" },
    { "inside", "@RDodongo's Cavern" },
    { "inside", "@BJabu-Jabu" },
    { "in", "the @GForest Temple" },
    { "in", "the @RFire Temple" },
    { "in", "the @BWater Temple" },
    { "in", "the @OSpirit Temple" },
    { "in", "the @PShadow Temple" },
    { "at", "the @PBottom of the Well" },
    { "in", "the @TIce Cavern" },
    { "in", "@YGerudo Training Grounds" },
    { "in", "the @YThieve's Hideout" },
    { "inside", "@RGanon's Castle" },
    { "in", "the @GKokiri Forest" },
    { "in", "@YHyrule Field" },
    { "in", "@YHyrule's Market" },
    { "in", "@OLon Lon Ranch" },
    { "in", "@YHyrule's Castle" },
    { "in", "@RGanon's Castle Exterior" },
    { "in", "the @GLost Woods" },
    { "in", "the @GSacred Meadow" },
    { "in", "@RKakariko" },
    { "inside", "@PKakariko's Graveyard" },
    { "on", "@RDeath Mountain's Trail" },
    { "in", "@RDeath Mountain's Crater" },
    { "in", "@RGoron City" },
    { "in", "@BZora's River" },
    { "in", "@BZora's Domain" },
    { "in", "@BZora's Fountain" },
    { "around", "@BLake Hylia" },
    { "in", "the @YTemple of Time" },
    { "in", "@OGerudo Valley" },
    { "around", "@OGerudo Fortress" },
    { "in", "the @YHaunted Wastelands" },
    { "around", "the @ODesert Colossus" },
    { "inside", "an @YEgg" },
    { "inside", "@RGanon's Castle Tower" },
};

// text.c kRegionNamesMm (index = (regionId & 0x7f) - 1).
const RegionName kRegionNamesMm[46] = {
    { "in", "@GWoodfall Temple" },
    { "in", "@TSnowhead Temple" },
    { "in", "@BGreat Bay Temple" },
    { "in", "@OStone Tower Temple" },
    { "in", "@YSouth Clock Town" },
    { "in", "@YNorth Clock Town" },
    { "in", "@YEast Clock Town" },
    { "in", "@YWest Clock Town" },
    { "in", "the @YLaundry Pool" },
    { "inside", "the @PGiant's Dream" },
    { "on", "@YClock Tower Roof" },
    { "in", "the @RStock Pot Inn" },
    { "in", "@RTermina Field" },
    { "on", "the @GRoad to the Swamp" },
    { "in", "the @GSouthern Swamp" },
    { "in", "@GDeku Palace" },
    { "in", "@GWoodfall" },
    { "on", "the @RPath to Mountain Village" },
    { "in", "the @RMountain Village" },
    { "on", "the @RPath to Snowhead" },
    { "around", "@RTwin Islands" },
    { "around", "@RGoron Village" },
    { "in", "@RSnowhead" },
    { "around", "the @PMilk Road" },
    { "around", "@ORomani Ranch" },
    { "around", "@BGreat Bay Coast" },
    { "in", "the @BPirate's Fortress Exterior" },
    { "in", "the @BPirate's Fortress Sewers" },
    { "in", "the @BPirate's Fortress Interior" },
    { "around", "@BZora Cape" },
    { "in", "@BZora Hall" },
    { "in", "@BPinnacle Rock" },
    { "on", "the @ORoad to Ikana" },
    { "in", "@PIkana's Graveyard" },
    { "in", "@OIkana Canyon" },
    { "in", "the @OAncient Castle of Ikana" },
    { "somewhere", "@PBeneath The Well" },
    { "in", "a @YSecret Shrine" },
    { "on", "the @OStone Tower" },
    { "on", "the @RMoon" },
    { "in", "the @GSwamp Spider House" },
    { "in", "the @BOcean Spider House" },
    { "from", "@GTingle" },
    { "in", "@OInverted Stone Tower Temple" },
    { "inside", "the @GButler Race" },
    { "inside", "the @RGoron Racetrack" },
};

const RegionName kRegionNowhere = { nullptr, "@Pnowhere" };
const RegionName kRegionLinkPocket = { "inside", "@YLink's Pocket" };
const RegionName kRegionNamelessPlace = { "in", "a @PNameless Place" };

struct CheckName {
    const char* name;
    bool plural;
};

// text.c kCheckNamesOot (index = checkId - 1).
const CheckName kCheckNamesOot[25] = {
    { "the @BFrogs Ocarina Game", false },
    { "@BFishing", false },
    { "a @PRavaged Village", false },
    { "@BKing Zora", false },
    { "the @RGreat Fairy outside of Ganon's Castle", false },
    { "the @RFire Temple Hammer Chest", false },
    { "the @RFire Temple Scarecrow Chest", false },
    { "the @YGerudo Training Grounds Water Room", false },
    { "the @OHaunted Wastelands Chest", false },
    { "the @YGerudo Archery", false },
    { "the @GCow in Link's house", false },
    { "@RBiggoron", false },
    { "the @TIce Cavern Final Chest", false },
    { "the @YMarket Treasure Game", false },
    { "@RShooting at the Sun", false },
    { "the @GFloormaster in the Forest Temple", false },
    { "@Pbombing a fiery skull pot", false },
    { "a @PStalfos duel near spikes", false },
    { "the @BWater Temple River chest", false },
    { "@Gtrading a bird and a mixture in the Lost Woods", false },
    { "@BStingers in Jabu-Jabu's Belly", false },
    { "playing @Ya symphony in Spirit Temple", false },
    { "a @Gchest hidden by a time block in Deku Tree", false },
    { "a @Rsingular scrub in Death Mountain Crater", false },
    { "a @Gspider deep within Deku Tree", false },
};

// text.c kCheckNamesMm (index = (checkId & 0x7f) - 1).
const CheckName kCheckNamesMm[30] = {
    { "the @ORanch Defense", false },
    { "the @GButler Race", false },
    { "@PAnju and Kafei", false },
    { "@BDon Gero's Choir", false },
    { "the @RGoron Race", false },
    { "the @PBeneath the Graveyard Night 3 Chest", false },
    { "the @PTermina Field Musical Stones", true },
    { "the @TBank's Final Reward", false },
    { "the @TMilk Bar Performance", false },
    { "the @GBoat Archery", false },
    { "the @BOcean Spider House Chest", false },
    { "the @BPinnacle Rock Seahorses", false },
    { "the @BFisherman's Game", false },
    { "@OIgos du Ikana", false },
    { "the @YSecret Shrine Wart and Final Chest", false },
    { "the @PCow Beneath The Well", false },
    { "the @RBlacksmith", false },
    { "the @PMidnight Meeting", false },
    { "@BMadame Aroma in the Bar", false },
    { "@YMarching for Cuccos", false },
    { "@PFinding Kafei", false },
    { "an @PInvisible Soldier", false },
    { "the @BGreat Bay Temple Wart", false },
    { "the @Tsecond Snowhead Wizzrobe", false },
    { "the @GWoodfall Temple Gekko", false },
    { "@Ydefeating Gomess", false },
    { "@Tfeeding a freezing Goron", false },
    { "@Phealing Kamaro", false },
    { "the @GWoodfall Temple Dark Room", false },
    { "winning the @TLottery", false },
};

// hint.c kJunkHints (30).
const char* const kJunkHints[30] = {
    "THEW ORLD ISSQ UARE",
    "SWITCH targeting is the superior option",
    "getting 100 coins gives you a star",
    "the main character is not actually called Zelda",
    "nothing is forever",
    "the yes needs the no, to win against the no",
    "a winner is you",
    "your princess is in another castle",
    "it's a secret to everybody",
    "Ocarina of Time is the better game",
    "Majora's Mask is the better game",
    "the cake is a lie",
    "I am Error",
    "plundering Area 51 is a foolish choice",
    "there are 118 known elements",
    "Nax's House is on the Way of the Hero",
    "this is not the hint you are looking for",
    "you can find Luigi in Super Mario 64",
    "using the Fishing Pole in Twilight Princess is WoTH",
    "23 is number 1",
    "keys are always in the last place you look in",
    "a famous code is UUDD LRLR BAS",
    "there's only 151 Pokemons",
    "you can unlock Sonic and Tails in Super Smash Bros Melee",
    "one should not forget to check the chests in Mido's House",
    "Gossip Stones tell lies",
    "it's dangerous to go alone!",
    "they're never gonna give you up, never gonna let you down",
    "your seed got 10 percents better",
    "your seed got 10 percents worse",
};

// hint.c appendPathName tables.
const char* const kPathTriforceNames[3] = { "@RPath of Power", "@GPath of Courage", "@BPath of Wisdom" };
const char* const kPathEndBossNames[2] = { "@RPath to Ganon", "@PPath to Majora" };
const char* const kPathEventNames[4] = { "@TPath to Time Travel", "@YPath to Rainbow Bridge", "@PPath to Termina",
                                         "@RPath to Moon" };

struct ColoredName {
    const char* color;
    const char* name;
};

// text_dungeon.c kDungeonNameDefs (index = og dungeon id).
const ColoredName kDungeonNames[26] = {
    { "@G", "Deku Tree" },
    { "@R", "Dodongo's Cavern" },
    { "@B", "Jabu-Jabu" },
    { "@G", "Forest Temple" },
    { "@R", "Fire Temple" },
    { "@B", "Water Temple" },
    { "@P", "Shadow Temple" },
    { "@Y", "Spirit Temple" },
    { "@G", "Woodfall Temple" },
    { "@R", "Snowhead Temple" },
    { "@B", "Great Bay Temple" },
    { "@Y", "Stone Tower Temple (Inverted)" },
    { "@Y", "Stone Tower Temple" },
    { "@G", "Swamp Spider House" },
    { "@B", "Ocean Spider House" },
    { "@P", "Bottom of the Well" },
    { "@B", "Ice Cavern" },
    { "@O", "Gerudo Training Grounds" },
    { "@P", "Beneath The Well" },
    { "@O", "Ikana Castle" },
    { "@Y", "Secret Shrine" },
    { "@P", "Beneath The Well (End)" },
    { "@B", "Pirate Fortress" },
    { "@R", "Ganon's Castle" },
    { "@R", "Ganon's Tower" },
    { "@Y", "Clock Tower Roof" },
};

// text_dungeon.c kBossColors + kBossNames (index = og boss id).
const ColoredName kBossNames[12] = {
    { "@G", "Gohma" },
    { "@R", "King Dodongo" },
    { "@B", "Barinade" },
    { "@G", "Phantom Ganon" },
    { "@R", "Volvagia" },
    { "@B", "Morpha" },
    { "@P", "Bongo Bongo" },
    { "@O", "Twinrova" },
    { "@G", "Odolwa" },
    { "@R", "Goht" },
    { "@B", "Gyorg" },
    { "@O", "Twinmold" },
};

// text.c kImportanceStrs.
const char* const kImportanceStrs[4] = { "@Runreachable", "@Pnot required", "@Tsometimes required", "@Yrequired" };

constexpr int TF_PREPOS = 1;
constexpr int TF_CAPITALIZE = 2;

const RegionName* RegionEntry(int regionId) {
    if (regionId == 0xff) return &kRegionLinkPocket;
    if (regionId == 0xfe) return &kRegionNamelessPlace;
    if (regionId == 0x00) return &kRegionNowhere;
    const int idx = (regionId & 0x7f) - 1;
    if (regionId & 0x80) {
        return (idx >= 0 && idx < 46) ? &kRegionNamesMm[idx] : &kRegionNamelessPlace;
    }
    return (idx >= 0 && idx < 38) ? &kRegionNamesOot[idx] : &kRegionNamelessPlace;
}

// og comboTextAppendRegionName: optional preposition, colored name, clear, world suffix
// (multiworld only), TF_CAPITALIZE uppercases the first printable character appended.
void AppendRegionName(std::string& out, const Seed& seed, const Seed::HintRegion& region, int flags) {
    const RegionName* entry = RegionEntry(region.regionId);
    const size_t start = out.size();
    if ((flags & TF_PREPOS) && entry->prepos != nullptr) {
        out += entry->prepos;
        out += ' ';
    }
    out += entry->name;
    out += "@0";
    if (seed.playerCount > 1 && region.world + 1 != seed.playerId) {
        out += " in @YWorld " + std::to_string(region.world + 1) + "@0";
    }
    if (flags & TF_CAPITALIZE) {
        for (size_t i = start; i < out.size(); ++i) {
            if (out[i] == '@') {
                ++i; // skip the tag letter
                continue;
            }
            if (std::isalpha(static_cast<unsigned char>(out[i]))) {
                out[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[i])));
                break;
            }
        }
    }
}

// og comboTextAppendCheckName: colored name + clear; returns the plural flag.
bool AppendCheckName(std::string& out, int checkId) {
    const int idx = (checkId & 0x7f) - 1;
    const CheckName* entry = nullptr;
    if (checkId & 0x80) {
        if (idx >= 0 && idx < 30) entry = &kCheckNamesMm[idx];
    } else {
        if (idx >= 0 && idx < 25) entry = &kCheckNamesOot[idx];
    }
    if (entry == nullptr) {
        out += "a @PNameless Place@0";
        return false;
    }
    out += entry->name;
    out += "@0";
    return entry->plural;
}

// og appendCorrectItemName: item name (og embeds the article + a color per item; the
// pc-seed carries the plain name, rendered as og's most common form "the @R<name>@0"),
// then the importance tag, then the multiworld player suffix.
void AppendHintItem(std::string& out, const Seed& seed, const Seed::HintItem& item) {
    out += "the @R";
    out += item.itemName.empty() ? item.itemId : item.itemName;
    out += "@0";
    if (seed.GetBoolSetting("hintImportance") && item.importance >= 0 && item.importance <= 3) {
        out += " (";
        out += kImportanceStrs[item.importance];
        out += "@0)";
    }
    if (item.player != 0 && item.player != seed.playerId) {
        out += " for @YPlayer " + std::to_string(item.player) + "@0";
    }
}

// og hint.c appendPathName.
void AppendPathName(std::string& out, int pathId, int subId) {
    switch (pathId) {
        case 0:
            out += "@YWay of the Hero";
            break;
        case 1:
            out += (subId >= 0 && subId < 3) ? kPathTriforceNames[subId] : "@YWay of the Hero";
            break;
        case 2:
            if (subId >= 0 && subId < 26) {
                out += kDungeonNames[subId].color;
                out += "Path to ";
                out += kDungeonNames[subId].name;
            }
            break;
        case 3:
            if (subId >= 0 && subId < 12) {
                out += kBossNames[subId].color;
                out += "Path to ";
                out += kBossNames[subId].name;
            }
            break;
        case 4:
            out += (subId >= 0 && subId < 2) ? kPathEndBossNames[subId] : "@RPath to Ganon";
            break;
        case 5:
            out += (subId >= 0 && subId < 4) ? kPathEventNames[subId] : "@YWay of the Hero";
            break;
        default:
            out += "@YWay of the Hero";
            break;
    }
    out += "@0";
}

std::string JunkText(int id) {
    std::string out = "They say that ";
    out += kJunkHints[((id % 30) + 30) % 30];
    out += ".";
    return out;
}

const ItemPlacement* FindPlacementByName(const Seed& seed, Game game, const std::string& name) {
    for (const ItemPlacement& p : seed.placements) {
        if (p.check.game != game) {
            continue;
        }
        // Match id and aliases too: pc-seed exports region-qualified names, so a bare-name compare misses.
        if (p.check.name == name || p.check.id == name) {
            return &p;
        }
        for (const std::string& alias : p.check.aliases) {
            if (alias == name) {
                return &p;
            }
        }
    }
    return nullptr;
}

// The child-altar "reuniting the Spiritual Stones leads to ..." rewards: what the
// Ocarina of Time / Song of Time checks hold (og comboTextAppendNpcReward).
void AppendNpcReward(std::string& out, const Seed& seed, const std::string& checkName, int importance) {
    const ItemPlacement* p = FindPlacementByName(seed, Game::Oot, checkName);
    if (p == nullptr) {
        out += "@Psomething@0";
        return;
    }
    Seed::HintItem item;
    item.itemId = p->item.id;
    item.itemName = p->item.name;
    item.player = p->ownerPlayer;
    item.importance = importance;
    AppendHintItem(out, seed, item);
}

int StaticImportance(const Seed& seed, size_t index) {
    return index < seed.hints.staticImportances.size() ? seed.hints.staticImportances[index] : -1;
}

} // namespace

bool HasHints(const Seed& seed) {
    return !seed.hints.gossip.empty() || !seed.hints.dungeonRewards.empty();
}

std::string GossipText(const Seed& seed, Game stoneGame, int stoneKey) {
    const Seed::GossipHint* hint = seed.hints.FindGossip(stoneGame, stoneKey);
    if (hint == nullptr) {
        // og Hint_Display fallback: no record for this stone -> deterministic junk.
        return JunkText(stoneKey);
    }
    if (hint->type == "junk") {
        return JunkText(hint->junkId);
    }

    std::string out = "They say that ";
    if (hint->type == "path") {
        AppendRegionName(out, seed, hint->region, 0);
        out += " is on the ";
        AppendPathName(out, hint->pathId, hint->pathSubId);
        if (hint->pathPlayer != 0 && hint->pathPlayer != seed.playerId) {
            out += " for @YPlayer " + std::to_string(hint->pathPlayer) + "@0";
        }
    } else if (hint->type == "foolish") {
        out += "plundering ";
        AppendRegionName(out, seed, hint->region, 0);
        out += " is a @Pfoolish choice@0";
    } else if (hint->type == "item-exact") {
        const bool plural = AppendCheckName(out, hint->checkId);
        out += plural ? " give " : " gives ";
        // og iterates items[0], then items[2], then items[1] (reversed tail walk).
        const auto& items = hint->items;
        if (!items.empty()) {
            AppendHintItem(out, seed, items[0]);
            if (items.size() > 2) {
                out += ", ";
                AppendHintItem(out, seed, items[2]);
            }
            if (items.size() > 1) {
                out += " and ";
                AppendHintItem(out, seed, items[1]);
            }
        }
    } else if (hint->type == "item-region") {
        if (!hint->items.empty()) {
            AppendHintItem(out, seed, hint->items[0]);
        }
        out += " can be found ";
        AppendRegionName(out, seed, hint->region, TF_PREPOS);
    } else {
        return JunkText(stoneKey);
    }
    out += ".";
    return out;
}

std::string AltarChildText(const Seed& seed) {
    std::string out;
    for (size_t i = 0; i < 3 && i < seed.hints.dungeonRewards.size(); ++i) {
        if (i > 0) out += "@b";
        AppendRegionName(out, seed, seed.hints.dungeonRewards[i], TF_PREPOS | TF_CAPITALIZE);
        out += "...";
    }
    out += "@b";
    out += "It is also written that reuniting the @YSpiritual Stones @0leads to ";
    // pc-seed exports these checks under OOT_HYRULE_FIELD_ ids (alias-aware lookup resolves either form).
    AppendNpcReward(out, seed, "OOT_HYRULE_FIELD_OCARINA_OF_TIME", StaticImportance(seed, 5));
    out += " and ";
    AppendNpcReward(out, seed, "OOT_HYRULE_FIELD_SONG_OF_TIME", StaticImportance(seed, 6));
    out += ".";
    return out;
}

std::string AltarAdultText(const Seed& seed) {
    std::string out;
    bool first = true;
    for (size_t i = 3; i < 9 && i < seed.hints.dungeonRewards.size(); ++i) {
        if (!first) out += "@b";
        first = false;
        AppendRegionName(out, seed, seed.hints.dungeonRewards[i], TF_PREPOS | TF_CAPITALIZE);
        out += "...";
    }
    // og gates the Ganon BK line on CFG_OOT_GANON_BOSS_KEY_HINT (ganonBossKey === 'anywhere')
    // and renders it exactly like a dungeon-reward line: boss-key icon + region + "..."
    // (En_Wonder_Talk.c hintDungeons), no prose.
    if (seed.GetStringSetting("ganonBossKey") == "anywhere" && seed.hints.ganonBossKey.name != "NONE" &&
        !seed.hints.ganonBossKey.name.empty()) {
        out += "@b";
        AppendRegionName(out, seed, seed.hints.ganonBossKey, TF_PREPOS | TF_CAPITALIZE);
        out += "...";
    }
    return out;
}

std::string RemainsHintText(const Seed& seed) {
    // og comboTextHijackDungeonRewardHints (text.c:1206-1220): dungeonRewards[9..12], one line per remains.
    std::string out;
    bool first = true;
    for (size_t i = 9; i < 13 && i < seed.hints.dungeonRewards.size(); ++i) {
        if (!first) {
            out += "@b";
        }
        first = false;
        AppendRegionName(out, seed, seed.hints.dungeonRewards[i], TF_PREPOS | TF_CAPITALIZE);
        out += "...";
    }
    return out;
}

std::string LightArrowsText(const Seed& seed) {
    std::string out = "Have you found the @YLight Arrows @0";
    AppendRegionName(out, seed, seed.hints.lightArrow, TF_PREPOS);
    out += "?";
    return out;
}

std::string OathToOrderText(const Seed& seed) {
    const auto& regions = seed.hints.oathToOrder;
    // Count the notes per distinct non-NONE region; a single distinct region (or a
    // single entry) is og's non-notes mode.
    std::vector<std::pair<const Seed::HintRegion*, int>> grouped;
    for (const Seed::HintRegion& r : regions) {
        if (r.name == "NONE" || r.name.empty()) continue;
        bool found = false;
        for (auto& g : grouped) {
            if (g.first->regionId == r.regionId && g.first->world == r.world) {
                ++g.second;
                found = true;
                break;
            }
        }
        if (!found) grouped.push_back({ &r, 1 });
    }
    if (grouped.empty()) {
        return "Have you found the @POath to Order@0?";
    }
    if (grouped.size() == 1 && grouped[0].second <= 1) {
        std::string out = "Have you found the @POath to Order @0";
        AppendRegionName(out, seed, *grouped[0].first, TF_PREPOS);
        out += "?";
        return out;
    }
    std::string out = "Have you found the @POath to Order Notes@0?";
    for (const auto& g : grouped) {
        out += "\n";
        out += std::to_string(g.second);
        out += " ";
        AppendRegionName(out, seed, *g.first, TF_PREPOS);
    }
    return out;
}

} // namespace ootmm::hints
