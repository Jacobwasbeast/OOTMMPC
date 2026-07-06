// Co-op presence + player-model mod sync codec. Same newline-JSON dialect as AnchorBridge;
// the ephemeral game<->launcher channel uses atomic last-value files (write temp + rename)
// instead of the durable append-only inbox/outbox.

#include "ootmm/Presence.hpp"
#include "ootmm/Seed.hpp"

#include "SimpleJson.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace ootmm::presence {
namespace {

void AppendCommonSeedFields(std::ostringstream& out, const Seed& seed) {
    out << ",\"seedId\":" << json::EscapeString(seed.seedId);
    out << ",\"teamId\":" << json::EscapeString(seed.teamId);
}

void AppendPoseFields(std::ostringstream& out, const PlayerPose& pose) {
    out << ",\"sourcePlayer\":" << pose.playerId;
    out << ",\"clientName\":" << json::EscapeString(pose.clientName);
    out << ",\"game\":" << json::EscapeString(ToString(pose.game));
    out << ",\"sceneId\":" << pose.sceneId;
    out << ",\"roomId\":" << pose.roomId;
    out << ",\"pos\":[" << pose.pos[0] << "," << pose.pos[1] << "," << pose.pos[2] << "]";
    out << ",\"yaw\":" << pose.yaw;
    out << ",\"yOff\":" << pose.yOffset;
    out << ",\"mvf\":" << pose.moveFlags;
    out << ",\"form\":" << pose.form;
    out << ",\"seq\":" << pose.seq;
    out << ",\"modelHash\":" << json::EscapeString(pose.modelHash);
    // Equipment visuals (omitted when default so old payloads stay byte-identical).
    if (!pose.dlLeftHand.empty()) out << ",\"dlL\":" << json::EscapeString(pose.dlLeftHand);
    if (!pose.dlRightHand.empty()) out << ",\"dlR\":" << json::EscapeString(pose.dlRightHand);
    if (!pose.dlSheath.empty()) out << ",\"dlS\":" << json::EscapeString(pose.dlSheath);
    if (!pose.dlWaist.empty()) out << ",\"dlW\":" << json::EscapeString(pose.dlWaist);
    if (pose.leftHandType >= 0) out << ",\"lht\":" << pose.leftHandType;
    if (pose.rightHandType >= 0) out << ",\"rht\":" << pose.rightHandType;
    if (pose.tunic != 0) out << ",\"tun\":" << pose.tunic;
    if (pose.boots != 0) out << ",\"bts\":" << pose.boots;
    if (pose.strength != 0) out << ",\"str\":" << pose.strength;
    if (pose.mask != 0) out << ",\"msk\":" << pose.mask;
    out << ",\"joints\":[";
    for (size_t i = 0; i < pose.jointTable.size(); ++i) {
        if (i > 0) out << ",";
        out << pose.jointTable[i];
    }
    out << "]";
}

int ReadInt(const json::Value& object, const char* key, int fallback) {
    const json::Value* value = object.Find(key);
    return (value != nullptr && value->IsNumber()) ? static_cast<int>(value->AsNumber()) : fallback;
}

double ReadNumber(const json::Value& object, const char* key, double fallback) {
    const json::Value* value = object.Find(key);
    return (value != nullptr && value->IsNumber()) ? value->AsNumber() : fallback;
}

std::string ReadString(const json::Value& object, const char* key) {
    const json::Value* value = object.Find(key);
    return (value != nullptr && value->IsString()) ? value->AsString() : std::string();
}

std::optional<PlayerPose> ParsePoseObject(const json::Value& root) {
    PlayerPose pose;
    pose.playerId = static_cast<uint16_t>(ReadInt(root, "sourcePlayer", 0));
    pose.clientName = ReadString(root, "clientName");
    const auto game = GameFromString(ReadString(root, "game"));
    if (!game.has_value()) {
        return std::nullopt;
    }
    pose.game = *game;
    pose.sceneId = ReadInt(root, "sceneId", -1);
    pose.roomId = ReadInt(root, "roomId", 0);
    if (const json::Value* pos = root.Find("pos"); pos != nullptr && pos->IsArray() && pos->AsArray().size() >= 3) {
        for (int i = 0; i < 3; ++i) {
            const json::Value& c = pos->AsArray()[i];
            pose.pos[i] = c.IsNumber() ? static_cast<float>(c.AsNumber()) : 0.0f;
        }
    }
    pose.yaw = static_cast<int16_t>(ReadInt(root, "yaw", 0));
    pose.yOffset = static_cast<float>(ReadNumber(root, "yOff", 0.0));
    pose.moveFlags = ReadInt(root, "mvf", 0);
    pose.form = ReadInt(root, "form", 0);
    pose.seq = static_cast<uint32_t>(ReadNumber(root, "seq", 0));
    pose.modelHash = ReadString(root, "modelHash");
    pose.dlLeftHand = ReadString(root, "dlL");
    pose.dlRightHand = ReadString(root, "dlR");
    pose.dlSheath = ReadString(root, "dlS");
    pose.dlWaist = ReadString(root, "dlW");
    pose.leftHandType = ReadInt(root, "lht", -1);
    pose.rightHandType = ReadInt(root, "rht", -1);
    pose.tunic = ReadInt(root, "tun", 0);
    pose.boots = ReadInt(root, "bts", 0);
    pose.strength = ReadInt(root, "str", 0);
    pose.mask = ReadInt(root, "msk", 0);
    if (const json::Value* joints = root.Find("joints"); joints != nullptr && joints->IsArray()) {
        pose.jointTable.reserve(joints->AsArray().size());
        for (const json::Value& j : joints->AsArray()) {
            pose.jointTable.push_back(static_cast<int16_t>(j.IsNumber() ? j.AsNumber() : 0));
        }
    }
    return pose;
}

std::string SerializePoseObject(const PlayerPose& pose, const char* type, const Seed* seed) {
    std::ostringstream out;
    out << "{\"type\":" << json::EscapeString(type);
    if (seed != nullptr) {
        AppendCommonSeedFields(out, *seed);
    }
    AppendPoseFields(out, pose);
    out << "}";
    return out.str();
}

std::string SerializePvpHit(const PvpHit& hit, const Seed* seed) {
    std::ostringstream out;
    out << "{\"type\":" << json::EscapeString(PacketPvpHit);
    if (seed != nullptr) {
        AppendCommonSeedFields(out, *seed);
    }
    out << ",\"sourcePlayer\":" << hit.sourcePlayer;
    out << ",\"targetPlayer\":" << hit.targetPlayer;
    out << ",\"damage\":" << hit.damage;
    out << ",\"yaw\":" << hit.yaw;
    out << ",\"seq\":" << hit.seq;
    out << "}";
    return out.str();
}

// Atomic replace: write to <path>.tmp, then rename over the destination. Readers either see
// the previous complete file or the new complete file, never a torn write.
bool AtomicWriteFile(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out << text;
        if (!out) {
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        // Windows rename fails if the destination is momentarily open by the reader; retry via
        // remove+rename, and give up quietly otherwise (next tick rewrites anyway).
        std::filesystem::remove(path, ec);
        std::filesystem::rename(tmp, path, ec);
        return !ec;
    }
    return true;
}

std::optional<std::string> ReadWholeFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

constexpr char kBase64Chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int8_t Base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return static_cast<int8_t>(c - 'A');
    if (c >= 'a' && c <= 'z') return static_cast<int8_t>(c - 'a' + 26);
    if (c >= '0' && c <= '9') return static_cast<int8_t>(c - '0' + 52);
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

} // namespace

std::string BuildPlayerStatePacket(const Seed& seed, const PlayerPose& pose) {
    return SerializePoseObject(pose, PacketPlayerState, &seed);
}

std::optional<PlayerPose> ParsePlayerStatePacket(const std::string& jsonText) {
    try {
        const json::Value root = json::Parse(jsonText);
        if (!root.IsObject() || ReadString(root, "type") != PacketPlayerState) {
            return std::nullopt;
        }
        return ParsePoseObject(root);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool WritePoseFile(const std::filesystem::path& path, const PlayerPose& pose) {
    return AtomicWriteFile(path, SerializePoseObject(pose, PacketPlayerState, nullptr) + "\n");
}

std::optional<PlayerPose> ReadPoseFile(const std::filesystem::path& path) {
    const std::optional<std::string> text = ReadWholeFile(path);
    if (!text.has_value()) {
        return std::nullopt;
    }
    return ParsePlayerStatePacket(*text);
}

bool WriteRosterFile(const std::filesystem::path& path, const std::vector<PlayerPose>& roster,
                     const RosterMeta* meta, const std::vector<PvpHit>* hits,
                     const std::vector<DeathEvent>* deaths) {
    std::string text;
    if (meta != nullptr) {
        text += std::string("{\"type\":\"OOTMM_ROSTER_META\",\"showNames\":") +
                (meta->showNames ? "true" : "false") + ",\"pvp\":" + (meta->pvp ? "true" : "false") +
                ",\"deathLink\":" + (meta->deathLink ? "true" : "false") +
                ",\"speedPercent\":" + std::to_string(meta->speedPercent) +
                ",\"speedSmooth\":" + (meta->speedSmooth ? "true" : "false") + "}\n";
    }
    for (const PlayerPose& pose : roster) {
        text += SerializePoseObject(pose, PacketPlayerState, nullptr);
        text += "\n";
    }
    if (hits != nullptr) {
        for (const PvpHit& hit : *hits) {
            text += SerializePvpHit(hit, nullptr);
            text += "\n";
        }
    }
    if (deaths != nullptr) {
        for (const DeathEvent& death : *deaths) {
            text += std::string("{\"type\":\"") + PacketDeathLink +
                    "\",\"sourcePlayer\":" + std::to_string(death.sourcePlayer) +
                    ",\"seq\":" + std::to_string(death.seq) + "}\n";
        }
    }
    return AtomicWriteFile(path, text);
}

std::vector<PlayerPose> ReadRosterFile(const std::filesystem::path& path, RosterMeta* outMeta,
                                       std::vector<PvpHit>* outHits, std::vector<DeathEvent>* outDeaths) {
    std::vector<PlayerPose> roster;
    const std::optional<std::string> text = ReadWholeFile(path);
    if (!text.has_value()) {
        return roster;
    }
    size_t start = 0;
    while (start < text->size()) {
        size_t end = text->find('\n', start);
        if (end == std::string::npos) {
            end = text->size();
        }
        const std::string line = text->substr(start, end - start);
        if (!line.empty()) {
            if (auto pose = ParsePlayerStatePacket(line); pose.has_value()) {
                roster.push_back(std::move(*pose));
            } else if (auto hit = ParsePvpHitPacket(line); hit.has_value()) {
                if (outHits != nullptr) {
                    outHits->push_back(*hit);
                }
            } else if (auto death = ParseDeathLinkPacket(line); death.has_value()) {
                if (outDeaths != nullptr) {
                    outDeaths->push_back(*death);
                }
            } else if (outMeta != nullptr) {
                try {
                    const json::Value root = json::Parse(line);
                    if (root.IsObject() && ReadString(root, "type") == "OOTMM_ROSTER_META") {
                        const json::Value* show = root.Find("showNames");
                        outMeta->showNames = show == nullptr || !show->IsBool() || show->AsBool();
                        const json::Value* pvp = root.Find("pvp");
                        outMeta->pvp = pvp != nullptr && pvp->IsBool() && pvp->AsBool();
                        const json::Value* dl = root.Find("deathLink");
                        outMeta->deathLink = dl != nullptr && dl->IsBool() && dl->AsBool();
                        outMeta->speedPercent = ReadInt(root, "speedPercent", 100);
                        const json::Value* smooth = root.Find("speedSmooth");
                        outMeta->speedSmooth = smooth == nullptr || !smooth->IsBool() || smooth->AsBool();
                    }
                } catch (const std::exception&) {
                }
            }
        }
        start = end + 1;
    }
    return roster;
}

std::string BuildPvpHitPacket(const Seed& seed, const PvpHit& hit) {
    return SerializePvpHit(hit, &seed);
}

std::optional<PvpHit> ParsePvpHitPacket(const std::string& jsonText) {
    try {
        const json::Value root = json::Parse(jsonText);
        if (!root.IsObject() || ReadString(root, "type") != PacketPvpHit) {
            return std::nullopt;
        }
        PvpHit hit;
        hit.sourcePlayer = static_cast<uint16_t>(ReadInt(root, "sourcePlayer", 0));
        hit.targetPlayer = static_cast<uint16_t>(ReadInt(root, "targetPlayer", 0));
        hit.damage = ReadInt(root, "damage", 0);
        hit.yaw = static_cast<int16_t>(ReadInt(root, "yaw", 0));
        hit.seq = static_cast<uint32_t>(ReadNumber(root, "seq", 0));
        return hit;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string BuildSessionMetaPacket(const Seed& seed, const SessionMeta& meta) {
    std::ostringstream out;
    out << "{\"type\":" << json::EscapeString(PacketSessionMeta);
    AppendCommonSeedFields(out, seed);
    out << ",\"pvp\":" << (meta.pvp ? "true" : "false");
    out << ",\"deathLink\":" << (meta.deathLink ? "true" : "false");
    out << ",\"timerEnabled\":" << (meta.timerEnabled ? "true" : "false");
    out << ",\"timerMin\":" << meta.timerMin;
    out << ",\"timerMax\":" << meta.timerMax;
    out << "}";
    return out.str();
}

std::optional<SessionMeta> ParseSessionMetaPacket(const std::string& jsonText) {
    try {
        const json::Value root = json::Parse(jsonText);
        if (!root.IsObject() || ReadString(root, "type") != PacketSessionMeta) {
            return std::nullopt;
        }
        SessionMeta meta;
        const json::Value* pvp = root.Find("pvp");
        meta.pvp = pvp != nullptr && pvp->IsBool() && pvp->AsBool();
        const json::Value* dl = root.Find("deathLink");
        meta.deathLink = dl != nullptr && dl->IsBool() && dl->AsBool();
        const json::Value* timer = root.Find("timerEnabled");
        meta.timerEnabled = timer == nullptr || !timer->IsBool() || timer->AsBool();
        meta.timerMin = ReadInt(root, "timerMin", 0);
        meta.timerMax = ReadInt(root, "timerMax", 250);
        return meta;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string BuildDeathLinkPacket(const Seed& seed, const DeathEvent& death) {
    std::ostringstream out;
    out << "{\"type\":" << json::EscapeString(PacketDeathLink);
    AppendCommonSeedFields(out, seed);
    out << ",\"sourcePlayer\":" << death.sourcePlayer;
    out << ",\"seq\":" << death.seq;
    out << "}";
    return out.str();
}

std::optional<DeathEvent> ParseDeathLinkPacket(const std::string& jsonText) {
    try {
        const json::Value root = json::Parse(jsonText);
        if (!root.IsObject() || ReadString(root, "type") != PacketDeathLink) {
            return std::nullopt;
        }
        DeathEvent death;
        death.sourcePlayer = static_cast<uint16_t>(ReadInt(root, "sourcePlayer", 0));
        death.seq = static_cast<uint32_t>(ReadNumber(root, "seq", 0));
        return death;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string BuildModAnnouncePacket(const Seed& seed, const ModAnnounce& announce) {
    std::ostringstream out;
    out << "{\"type\":" << json::EscapeString(PacketModAnnounce);
    AppendCommonSeedFields(out, seed);
    out << ",\"sourcePlayer\":" << announce.playerId;
    out << ",\"game\":" << json::EscapeString(ToString(announce.game));
    out << ",\"name\":" << json::EscapeString(announce.name);
    out << ",\"hash\":" << json::EscapeString(announce.hash);
    out << ",\"size\":" << announce.size;
    out << "}";
    return out.str();
}

std::optional<ModAnnounce> ParseModAnnouncePacket(const std::string& jsonText) {
    try {
        const json::Value root = json::Parse(jsonText);
        if (!root.IsObject() || ReadString(root, "type") != PacketModAnnounce) {
            return std::nullopt;
        }
        ModAnnounce announce;
        announce.playerId = static_cast<uint16_t>(ReadInt(root, "sourcePlayer", 0));
        if (const auto game = GameFromString(ReadString(root, "game")); game.has_value()) {
            announce.game = *game;
        }
        announce.name = ReadString(root, "name");
        announce.hash = ReadString(root, "hash");
        announce.size = static_cast<uint64_t>(ReadNumber(root, "size", 0));
        return announce;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string BuildModRequestPacket(const Seed& seed, const ModRequest& request) {
    std::ostringstream out;
    out << "{\"type\":" << json::EscapeString(PacketModRequest);
    AppendCommonSeedFields(out, seed);
    out << ",\"sourcePlayer\":" << request.playerId;
    out << ",\"targetPlayer\":" << request.targetPlayer;
    out << ",\"hash\":" << json::EscapeString(request.hash);
    out << "}";
    return out.str();
}

std::optional<ModRequest> ParseModRequestPacket(const std::string& jsonText) {
    try {
        const json::Value root = json::Parse(jsonText);
        if (!root.IsObject() || ReadString(root, "type") != PacketModRequest) {
            return std::nullopt;
        }
        ModRequest request;
        request.playerId = static_cast<uint16_t>(ReadInt(root, "sourcePlayer", 0));
        request.targetPlayer = static_cast<uint16_t>(ReadInt(root, "targetPlayer", 0));
        request.hash = ReadString(root, "hash");
        return request;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string BuildModChunkPacket(const Seed& seed, const ModChunk& chunk) {
    std::ostringstream out;
    out << "{\"type\":" << json::EscapeString(PacketModChunk);
    AppendCommonSeedFields(out, seed);
    out << ",\"sourcePlayer\":" << chunk.playerId;
    out << ",\"hash\":" << json::EscapeString(chunk.hash);
    out << ",\"seq\":" << chunk.seq;
    out << ",\"total\":" << chunk.total;
    out << ",\"data\":" << json::EscapeString(chunk.dataB64);
    out << "}";
    return out.str();
}

std::optional<ModChunk> ParseModChunkPacket(const std::string& jsonText) {
    try {
        const json::Value root = json::Parse(jsonText);
        if (!root.IsObject() || ReadString(root, "type") != PacketModChunk) {
            return std::nullopt;
        }
        ModChunk chunk;
        chunk.playerId = static_cast<uint16_t>(ReadInt(root, "sourcePlayer", 0));
        chunk.hash = ReadString(root, "hash");
        chunk.seq = static_cast<uint32_t>(ReadNumber(root, "seq", 0));
        chunk.total = static_cast<uint32_t>(ReadNumber(root, "total", 0));
        chunk.dataB64 = ReadString(root, "data");
        return chunk;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string Base64Encode(const uint8_t* data, size_t size) {
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t b0 = data[i];
        const uint32_t b1 = (i + 1 < size) ? data[i + 1] : 0;
        const uint32_t b2 = (i + 2 < size) ? data[i + 2] : 0;
        const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        out += kBase64Chars[(triple >> 18) & 0x3F];
        out += kBase64Chars[(triple >> 12) & 0x3F];
        out += (i + 1 < size) ? kBase64Chars[(triple >> 6) & 0x3F] : '=';
        out += (i + 2 < size) ? kBase64Chars[triple & 0x3F] : '=';
    }
    return out;
}

std::vector<uint8_t> Base64Decode(const std::string& text) {
    std::vector<uint8_t> out;
    out.reserve((text.size() / 4) * 3);
    uint32_t accum = 0;
    int bits = 0;
    for (const char c : text) {
        if (c == '=' || c == '\n' || c == '\r') {
            continue;
        }
        const int8_t v = Base64Value(c);
        if (v < 0) {
            continue;
        }
        accum = (accum << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((accum >> bits) & 0xFF));
        }
    }
    return out;
}

std::string HashBytesHex(const uint8_t* data, size_t size) {
    uint64_t hash = 1469598103934665603ull; // FNV-1a 64 offset basis
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ull;
    }
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return buffer;
}

std::string HashFileHex(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return "";
    }
    uint64_t hash = 1469598103934665603ull;
    char buffer[65536];
    while (in.read(buffer, sizeof(buffer)) || in.gcount() > 0) {
        const std::streamsize n = in.gcount();
        for (std::streamsize i = 0; i < n; ++i) {
            hash ^= static_cast<uint8_t>(buffer[i]);
            hash *= 1099511628211ull;
        }
        if (n < static_cast<std::streamsize>(sizeof(buffer))) {
            break;
        }
    }
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));
    return hex;
}

} // namespace ootmm::presence
