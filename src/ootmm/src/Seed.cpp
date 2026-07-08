#include "ootmm/Seed.hpp"

#include "SimpleJson.hpp"

#include <fstream>
#include <sstream>

namespace ootmm {

std::string ToString(Game game) {
    return game == Game::Oot ? "oot" : "mm";
}

std::optional<Game> GameFromString(const std::string& value) {
    if (value == "oot" || value == "OoT" || value == "OOT") {
        return Game::Oot;
    }
    if (value == "mm" || value == "MM") {
        return Game::Mm;
    }
    return std::nullopt;
}

std::string ItemPlacement::DeliveryKey() const {
    return ToString(check.game) + ":" + check.id + "->p" + std::to_string(ownerPlayer) + ":" + item.id;
}

bool Seed::GetBoolSetting(const std::string& key, bool fallback) const {
    const auto it = boolSettings.find(key);
    return it != boolSettings.end() ? it->second : fallback;
}

std::string Seed::GetStringSetting(const std::string& key, const std::string& fallback) const {
    const auto it = stringSettings.find(key);
    return it != stringSettings.end() ? it->second : fallback;
}

int Seed::GetIntSetting(const std::string& key, int fallback) const {
    const auto it = numberSettings.find(key);
    return it != numberSettings.end() ? static_cast<int>(it->second) : fallback;
}

std::vector<std::string> Seed::UnsupportedEnabledSettings() const {
    std::vector<std::string> out;
    // og-payload custom ability items: ids deliver but the engine lacks the ability implementation.
    static constexpr const char* kUnsupportedBools[] = {
        "spellFireMm",  "spellWindMm",   "spellLoveMm",    "hammerMm",       "bootsIronMm",
        "bootsHoverMm", "tunicGoronMm",  "tunicZoraMm",    "sunSongMm",      "fairyOcarinaMm",
        "shortHookshotMm", "blastMaskOot", "stoneMaskOot", "elegyOot",       "spinUpgradeOot",
        // ER behavior modes with actor-level requirements beyond the entrance override table.
        "erWallmasters", "erOneWaysWaterVoids", "erOneWaysWoods", "alterLostWoodsExits",
        // Cross-game warp/ability triggers.
        "crossWarpOot", "crossWarpMm", "crossGameFw",
        // Shared consumable/upgrade pools (persistent capabilities DO mirror; live pools don't).
        "sharedWallets", "sharedBows", "sharedBombBags", "sharedMagic", "sharedHealth",
        "sharedBottles",
        // Economy extensions.
        "colossalWallets", "bottomlessWallets", "rupeeScaling", "coins",
        // Misc unhonored behaviors.
        "voidWarpMm", "swordlessAdult", "restoreBrokenActors", "preCompletedDungeons",
        "songEventsShuffleOot", "iceArrowPlatformsOot", "kegStrength3",
    };
    for (const char* key : kUnsupportedBools) {
        if (GetBoolSetting(key, false)) {
            out.emplace_back(key);
        }
    }
    if (GetStringSetting("songs", "songLocations") == "notes") {
        out.emplace_back("songs=notes");
    }
    if (GetStringSetting("ageChange", "none") != "none") {
        out.emplace_back("ageChange=" + GetStringSetting("ageChange"));
    }
    if (GetStringSetting("clockSpeed", "default") != "default") {
        out.emplace_back("clockSpeed=" + GetStringSetting("clockSpeed"));
    }
    if (GetStringSetting("autoInvert", "never") != "never") {
        out.emplace_back("autoInvert=" + GetStringSetting("autoInvert"));
    }
    if (GetStringSetting("erBoss", "none") != "none") {
        out.emplace_back("erBoss=" + GetStringSetting("erBoss"));
    }
    if (GetStringSetting("majoraChild", "none") == "custom") {
        out.emplace_back("majoraChild=custom");
    }
    if (GetStringSetting("mode", "single") == "coop") {
        out.emplace_back("mode=coop");
    }
    for (const char* priceKey : { "priceOotShops", "priceMmShops", "priceOotScrubs", "priceOotMerchants",
                                  "priceMmTingle" }) {
        if (GetStringSetting(priceKey, "vanilla") != "vanilla") {
            out.emplace_back(std::string(priceKey) + "=" + GetStringSetting(priceKey));
        }
    }
    return out;
}

const Seed::WorldFlag* Seed::GetWorldFlag(const std::string& key) const {
    const auto it = worldFlags.find(key);
    return it != worldFlags.end() ? &it->second : nullptr;
}

bool Seed::WorldFlagContains(const std::string& key, const std::string& member) const {
    const WorldFlag* flag = GetWorldFlag(key);
    if (flag == nullptr) {
        return false;
    }
    if (flag->all) {
        return true;
    }
    for (const std::string& entry : flag->members) {
        if (entry == member) {
            return true;
        }
    }
    return false;
}

int Seed::WorldFlagCount(const std::string& key, int totalIfAll) const {
    const WorldFlag* flag = GetWorldFlag(key);
    if (flag == nullptr) {
        return 0;
    }
    return flag->all ? totalIfAll : static_cast<int>(flag->members.size());
}

bool Seed::SpecialCond::Field(const std::string& name) const {
    const auto it = fields.find(name);
    return it != fields.end() && it->second;
}

const Seed::SpecialCond* Seed::GetSpecialCond(const std::string& name) const {
    const auto it = specialConds.find(name);
    return it != specialConds.end() ? &it->second : nullptr;
}

const ItemPlacement* Seed::FindPlacement(Game game, const std::string& checkId) const {
    for (const ItemPlacement& placement : placements) {
        if (placement.check.game != game) {
            continue;
        }
        if (placement.check.id == checkId || placement.check.name == checkId) {
            return &placement;
        }
        for (const std::string& alias : placement.check.aliases) {
            if (alias == checkId) {
                return &placement;
            }
        }
    }
    return nullptr;
}

const EntranceMapping* Seed::FindEntrance(Game fromGame, const std::string& from) const {
    for (const EntranceMapping& entrance : entrances) {
        if (entrance.fromGame == fromGame && entrance.from == from) {
            return &entrance;
        }
    }
    return nullptr;
}

const EntranceMapping* Seed::FindEntrance(Game fromGame, uint32_t fromNativeId) const {
    for (const EntranceMapping& entrance : entrances) {
        if (entrance.fromGame == fromGame && entrance.fromNativeId.has_value() && *entrance.fromNativeId == fromNativeId) {
            return &entrance;
        }
    }
    return nullptr;
}

Game Seed::ResolveStartingGame() const {
    if (startingGame.has_value()) {
        return *startingGame;
    }
    if (!startingItems.empty()) {
        bool allMm = true;
        for (const StartingItem& startingItem : startingItems) {
            if (startingItem.item.game != Game::Mm) {
                allMm = false;
                break;
            }
        }
        if (allMm) {
            return Game::Mm;
        }
    }
    return Game::Oot;
}

const Seed::GossipHint* Seed::SeedHints::FindGossip(Game game, int stoneKey) const {
    for (const GossipHint& hint : gossip) {
        if (hint.stoneGame == game && hint.stoneKey == stoneKey) {
            return &hint;
        }
    }
    return nullptr;
}

bool Seed::IsCompatible() const {
    return format == CurrentFormat && !seedId.empty() && playerId > 0 && playerCount > 0 && playerId <= playerCount;
}

SeedLoadError::SeedLoadError(const std::string& message) : std::runtime_error(message) {}

namespace {

const json::Value& Required(const json::Value& object, const std::string& key) {
    try {
        return object.At(key);
    } catch (const std::out_of_range&) {
        throw SeedLoadError("Missing required seed key: " + key);
    }
}

std::string RequiredString(const json::Value& object, const std::string& key) {
    const json::Value& value = Required(object, key);
    if (!value.IsString()) {
        throw SeedLoadError("Seed key must be a string: " + key);
    }
    return value.AsString();
}

std::string OptionalString(const json::Value& object, const std::string& key, const std::string& fallback) {
    const json::Value* value = object.Find(key);
    if (value == nullptr || value->IsNull()) {
        return fallback;
    }
    if (!value->IsString()) {
        throw SeedLoadError("Seed key must be a string: " + key);
    }
    return value->AsString();
}

uint16_t OptionalU16(const json::Value& object, const std::string& key, uint16_t fallback) {
    const json::Value* value = object.Find(key);
    if (value == nullptr || value->IsNull()) {
        return fallback;
    }
    if (!value->IsNumber()) {
        throw SeedLoadError("Seed key must be a number: " + key);
    }
    const double raw = value->AsNumber();
    if (raw < 0 || raw > 65535 || raw != static_cast<uint16_t>(raw)) {
        throw SeedLoadError("Seed key is outside uint16 range: " + key);
    }
    return static_cast<uint16_t>(raw);
}

std::optional<uint32_t> OptionalU32(const json::Value& object, const std::string& key) {
    const json::Value* value = object.Find(key);
    if (value == nullptr || value->IsNull()) {
        return std::nullopt;
    }
    if (!value->IsNumber()) {
        throw SeedLoadError("Seed key must be a number: " + key);
    }
    const double raw = value->AsNumber();
    if (raw < 0 || raw > 4294967295.0 || raw != static_cast<uint32_t>(raw)) {
        throw SeedLoadError("Seed key is outside uint32 range: " + key);
    }
    return static_cast<uint32_t>(raw);
}

int OptionalInt(const json::Value& object, const std::string& key, int fallback) {
    const json::Value* value = object.Find(key);
    if (value == nullptr || value->IsNull()) {
        return fallback;
    }
    if (!value->IsNumber()) {
        throw SeedLoadError("Seed key must be a number: " + key);
    }
    return static_cast<int>(value->AsNumber());
}

bool OptionalBool(const json::Value& object, const std::string& key, bool fallback) {
    const json::Value* value = object.Find(key);
    if (value == nullptr || value->IsNull()) {
        return fallback;
    }
    if (!value->IsBool()) {
        throw SeedLoadError("Seed key must be a boolean: " + key);
    }
    return value->AsBool();
}

std::vector<std::string> OptionalStringArray(const json::Value& object, const std::string& key) {
    const json::Value* value = object.Find(key);
    if (value == nullptr || value->IsNull()) {
        return {};
    }
    if (!value->IsArray()) {
        throw SeedLoadError("Seed key must be an array: " + key);
    }

    std::vector<std::string> result;
    for (const json::Value& entry : value->AsArray()) {
        if (!entry.IsString()) {
            throw SeedLoadError("Seed array entries must be strings: " + key);
        }
        result.push_back(entry.AsString());
    }
    return result;
}

Game RequiredGame(const json::Value& object, const std::string& key) {
    const auto parsed = GameFromString(RequiredString(object, key));
    if (!parsed.has_value()) {
        throw SeedLoadError("Invalid game value for seed key: " + key);
    }
    return *parsed;
}

std::optional<Game> OptionalGame(const json::Value& object, const std::string& key) {
    const json::Value* value = object.Find(key);
    if (value == nullptr || value->IsNull()) {
        return std::nullopt;
    }
    if (!value->IsString()) {
        throw SeedLoadError("Seed key must be a string: " + key);
    }
    const auto parsed = GameFromString(value->AsString());
    if (!parsed.has_value()) {
        throw SeedLoadError("Invalid game value for seed key: " + key);
    }
    return parsed;
}

ItemRef ParseItemRef(const json::Value& object, const std::string& gameKey, const std::string& idKey,
                     const std::string& nameKey) {
    return {
        .game = RequiredGame(object, gameKey),
        .id = RequiredString(object, idKey),
        .name = OptionalString(object, nameKey, ""),
    };
}

} // namespace

Seed LoadSeedFromJson(const std::string& jsonText) {
    json::Value root;
    try {
        root = json::Parse(jsonText);
    } catch (const json::ParseError& error) {
        throw SeedLoadError(error.what());
    }

    if (!root.IsObject()) {
        throw SeedLoadError("Seed root must be a JSON object");
    }

    Seed seed;
    seed.format = RequiredString(root, "format");
    seed.seedId = RequiredString(root, "seedId");
    seed.settingsHash = OptionalString(root, "settingsHash", "");
    seed.playerId = OptionalU16(root, "playerId", 1);
    seed.playerCount = OptionalU16(root, "playerCount", 1);
    seed.teamId = OptionalString(root, "teamId", "default");
    seed.startingGame = OptionalGame(root, "startingGame");

    // Capture the OoTMM settings so the ports can honor gameplay options. The settings
    // object mixes booleans (open forest toggles, QoL), enum strings (dekuTree,
    // doorOfTime, rainbowBridge...), and numbers (triforce counts, coin counts). Capture
    // each scalar kind into its own map, and parse the nested specialConds tree.
    if (const json::Value* settings = root.Find("settings"); settings != nullptr && settings->IsObject()) {
        for (const auto& [key, value] : settings->AsObject()) {
            if (value.IsBool()) {
                seed.boolSettings[key] = value.AsBool();
            } else if (value.IsString()) {
                seed.stringSettings[key] = value.AsString();
            } else if (value.IsNumber()) {
                seed.numberSettings[key] = value.AsNumber();
            }
        }

        // specialConds: { BRIDGE:{count, stones, medallions, remains, ...}, MOON:{...}, ... }
        if (const json::Value* conds = settings->Find("specialConds"); conds != nullptr && conds->IsObject()) {
            for (const auto& [condName, body] : conds->AsObject()) {
                if (!body.IsObject()) {
                    continue;
                }
                Seed::SpecialCond cond;
                if (const json::Value* count = body.Find("count"); count != nullptr && count->IsNumber()) {
                    cond.count = static_cast<int>(count->AsNumber());
                }
                for (const auto& [fieldName, fieldValue] : body.AsObject()) {
                    if (fieldValue.IsBool()) {
                        cond.fields[fieldName] = fieldValue.AsBool();
                    }
                }
                seed.specialConds[condName] = std::move(cond);
            }
        }
    }

    // Top-level worldFlags: each value is "none", "all", or an explicit member array.
    if (const json::Value* flags = root.Find("worldFlags"); flags != nullptr && flags->IsObject()) {
        for (const auto& [key, value] : flags->AsObject()) {
            Seed::WorldFlag flag;
            if (value.IsString()) {
                const std::string& resolved = value.AsString();
                flag.all = (resolved == "all");
                // "none" (and any other scalar) resolves to an empty member set.
            } else if (value.IsArray()) {
                for (const json::Value& member : value.AsArray()) {
                    if (member.IsString()) {
                        flag.members.push_back(member.AsString());
                    }
                }
            }
            seed.worldFlags[key] = std::move(flag);
        }
    }

    if (const json::Value* placements = root.Find("placements")) {
        if (!placements->IsArray()) {
            throw SeedLoadError("Seed placements must be an array");
        }
        for (const json::Value& entry : placements->AsArray()) {
            if (!entry.IsObject()) {
                throw SeedLoadError("Seed placement entries must be objects");
            }
            ItemPlacement placement;
            placement.check = {
                .game = RequiredGame(entry, "checkGame"),
                .id = RequiredString(entry, "checkId"),
                .name = OptionalString(entry, "checkName", ""),
                .aliases = OptionalStringArray(entry, "checkAliases"),
                .sceneId = OptionalU32(entry, "checkScene"),
                .type = OptionalString(entry, "checkType", ""),
                .flag = OptionalU32(entry, "checkFlag"),
            };
            placement.item = ParseItemRef(entry, "itemGame", "itemId", "itemName");
            placement.ownerPlayer = OptionalU16(entry, "ownerPlayer", seed.playerId);
            placement.sourcePlayer = OptionalU16(entry, "sourcePlayer", seed.playerId);
            placement.major = OptionalBool(entry, "major", false);
            seed.placements.push_back(std::move(placement));
        }
    }

    if (const json::Value* entrances = root.Find("entrances")) {
        if (!entrances->IsArray()) {
            throw SeedLoadError("Seed entrances must be an array");
        }
        for (const json::Value& entry : entrances->AsArray()) {
            if (!entry.IsObject()) {
                throw SeedLoadError("Seed entrance entries must be objects");
            }
            seed.entrances.push_back({
                .fromGame = RequiredGame(entry, "fromGame"),
                .from = RequiredString(entry, "from"),
                .fromNativeId = OptionalU32(entry, "fromNativeId"),
                .toGame = RequiredGame(entry, "toGame"),
                .to = RequiredString(entry, "to"),
                .toNativeId = OptionalU32(entry, "toNativeId"),
            });
        }
    }

    if (const json::Value* startingItems = root.Find("startingItems")) {
        if (!startingItems->IsArray()) {
            throw SeedLoadError("Seed startingItems must be an array");
        }
        for (const json::Value& entry : startingItems->AsArray()) {
            if (!entry.IsObject()) {
                throw SeedLoadError("Seed starting item entries must be objects");
            }
            seed.startingItems.push_back({
                .item = ParseItemRef(entry, "itemGame", "itemId", "itemName"),
                .count = OptionalU16(entry, "count", 1),
            });
        }
    }

    // Optional og hints section (older seeds don't carry it; everything degrades to
    // the deterministic junk fallback in the text builder).
    if (const json::Value* hints = root.Find("hints"); hints != nullptr && hints->IsObject()) {
        const auto parseRegion = [](const json::Value* value) {
            Seed::HintRegion region;
            region.name = "NONE";
            if (value != nullptr && value->IsObject()) {
                region.name = OptionalString(*value, "name", "NONE");
                region.regionId = OptionalInt(*value, "regionId", 0);
                region.world = OptionalInt(*value, "world", 0);
            }
            return region;
        };
        const auto parseRegionArray = [&parseRegion](const json::Value* value) {
            std::vector<Seed::HintRegion> regions;
            if (value != nullptr && value->IsArray()) {
                for (const json::Value& entry : value->AsArray()) {
                    regions.push_back(parseRegion(&entry));
                }
            }
            return regions;
        };

        if (const json::Value* gossip = hints->Find("gossip"); gossip != nullptr && gossip->IsArray()) {
            for (const json::Value& entry : gossip->AsArray()) {
                if (!entry.IsObject()) {
                    continue;
                }
                Seed::GossipHint hint;
                hint.stone = OptionalString(entry, "stone", "");
                if (const auto game = OptionalGame(entry, "stoneGame"); game.has_value()) {
                    hint.stoneGame = *game;
                }
                hint.stoneKey = OptionalInt(entry, "stoneKey", 0);
                hint.type = OptionalString(entry, "type", "junk");
                hint.region = parseRegion(entry.Find("region"));
                hint.pathId = OptionalInt(entry, "pathId", -1);
                hint.pathSubId = OptionalInt(entry, "pathSubId", -1);
                hint.pathPlayer = OptionalInt(entry, "pathPlayer", 0);
                hint.checkId = OptionalInt(entry, "checkId", 0);
                hint.checkName = OptionalString(entry, "checkName", "");
                hint.checkWorld = OptionalInt(entry, "checkWorld", 0);
                hint.junkId = OptionalInt(entry, "junkId", 0);
                if (const json::Value* items = entry.Find("items"); items != nullptr && items->IsArray()) {
                    for (const json::Value& itemEntry : items->AsArray()) {
                        if (!itemEntry.IsObject()) {
                            continue;
                        }
                        Seed::HintItem item;
                        item.itemId = OptionalString(itemEntry, "itemId", "");
                        item.itemName = OptionalString(itemEntry, "itemName", "");
                        item.player = OptionalInt(itemEntry, "player", 0);
                        item.importance = OptionalInt(itemEntry, "importance", -1);
                        hint.items.push_back(std::move(item));
                    }
                }
                seed.hints.gossip.push_back(std::move(hint));
            }
        }
        seed.hints.dungeonRewards = parseRegionArray(hints->Find("dungeonRewards"));
        seed.hints.lightArrow = parseRegion(hints->Find("lightArrow"));
        seed.hints.oathToOrder = parseRegionArray(hints->Find("oathToOrder"));
        seed.hints.ganonBossKey = parseRegion(hints->Find("ganonBossKey"));
        if (const json::Value* imps = hints->Find("staticImportances"); imps != nullptr && imps->IsArray()) {
            for (const json::Value& entry : imps->AsArray()) {
                if (entry.IsNumber()) {
                    seed.hints.staticImportances.push_back(static_cast<int>(entry.AsNumber()));
                }
            }
        }
    }

    if (!seed.IsCompatible()) {
        throw SeedLoadError("Seed is not compatible with " + std::string(Seed::CurrentFormat));
    }

    return seed;
}

Seed LoadSeedFromFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw SeedLoadError("Unable to open seed file: " + path.string());
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return LoadSeedFromJson(buffer.str());
}

} // namespace ootmm
