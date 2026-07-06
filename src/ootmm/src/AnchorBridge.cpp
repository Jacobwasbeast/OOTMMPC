#include "ootmm/AnchorBridge.hpp"

#include "SimpleJson.hpp"

#include <sstream>

namespace ootmm::anchor {
namespace {

void AppendCommonSeedFields(std::ostringstream& out, const Seed& seed) {
    out << ",\"seedId\":" << json::EscapeString(seed.seedId);
    out << ",\"settingsHash\":" << json::EscapeString(seed.settingsHash);
    out << ",\"teamId\":" << json::EscapeString(seed.teamId);
    out << ",\"playerId\":" << seed.playerId;
    out << ",\"playerCount\":" << seed.playerCount;
}

std::string BuildEventPacket(const char* packetType, const Seed& seed, const NetworkEvent& event) {
    std::ostringstream out;
    out << "{\"type\":" << json::EscapeString(packetType);
    AppendCommonSeedFields(out, seed);
    out << ",\"sourcePlayer\":" << event.sourcePlayer;
    out << ",\"targetPlayer\":" << event.targetPlayer;
    out << ",\"checkGame\":" << json::EscapeString(ToString(event.checkGame));
    out << ",\"checkId\":" << json::EscapeString(event.checkId);
    out << ",\"itemGame\":" << json::EscapeString(ToString(event.item.game));
    out << ",\"itemId\":" << json::EscapeString(event.item.id);
    out << ",\"itemName\":" << json::EscapeString(event.item.name);
    out << ",\"deliveryKey\":" << json::EscapeString(event.deliveryKey);
    out << "}";
    return out.str();
}

uint16_t ReadU16(const json::Value& object, const std::string& key) {
    const json::Value& value = object.At(key);
    if (!value.IsNumber() || value.AsNumber() < 0 || value.AsNumber() > 65535) {
        throw std::runtime_error("Invalid Anchor packet number: " + key);
    }
    return static_cast<uint16_t>(value.AsNumber());
}

std::string ReadString(const json::Value& object, const std::string& key) {
    const json::Value& value = object.At(key);
    if (!value.IsString()) {
        throw std::runtime_error("Invalid Anchor packet string: " + key);
    }
    return value.AsString();
}

} // namespace

std::string BuildHandshakePacket(const Seed& seed, const std::string& clientName) {
    std::ostringstream out;
    out << "{\"type\":" << json::EscapeString(PacketHandshake);
    AppendCommonSeedFields(out, seed);
    out << ",\"clientName\":" << json::EscapeString(clientName);
    out << ",\"format\":" << json::EscapeString(Seed::CurrentFormat);
    out << "}";
    return out.str();
}

std::string BuildCheckCatalogPacket(const Seed& seed) {
    std::ostringstream out;
    out << "{\"type\":" << json::EscapeString(PacketCheckCatalog);
    AppendCommonSeedFields(out, seed);
    std::string checks;
    for (const ItemPlacement& placement : seed.placements) {
        if (!checks.empty()) {
            checks += ",";
        }
        checks += placement.check.id;
    }
    out << ",\"checks\":" << json::EscapeString(checks);
    out << "}";
    return out.str();
}

std::string BuildCheckCompletePacket(const Seed& seed, const NetworkEvent& event) {
    return BuildEventPacket(PacketCheckComplete, seed, event);
}

std::string BuildGiveItemPacket(const Seed& seed, const NetworkEvent& event) {
    return BuildEventPacket(PacketGiveItem, seed, event);
}

std::vector<std::string> BuildPacketsForEvent(const Seed& seed, const NetworkEvent& event) {
    std::vector<std::string> packets;
    if (event.type == NetworkEventType::CheckComplete) {
        packets.push_back(BuildCheckCompletePacket(seed, event));
        if (event.targetPlayer != seed.playerId) {
            packets.push_back(BuildGiveItemPacket(seed, event));
        }
    } else if (event.type == NetworkEventType::GiveItem) {
        packets.push_back(BuildGiveItemPacket(seed, event));
    }
    return packets;
}

std::string BuildCrossGameTransitionPacket(const Seed& seed, const CrossGameTransition& transition) {
    std::ostringstream out;
    out << "{\"type\":" << json::EscapeString(PacketCrossGameTransition);
    AppendCommonSeedFields(out, seed);
    out << ",\"fromGame\":" << json::EscapeString(ToString(transition.fromGame));
    out << ",\"toGame\":" << json::EscapeString(ToString(transition.toGame));
    out << ",\"toEntrance\":" << json::EscapeString(transition.toEntrance);
    if (transition.toNativeId.has_value()) {
        out << ",\"toNativeId\":" << *transition.toNativeId;
    }
    if (transition.ootAge.has_value()) {
        out << ",\"ootAge\":" << *transition.ootAge;
    }
    out << "}";
    return out.str();
}

std::optional<CrossGameTransition> ParseCrossGameTransitionPacket(const std::string& jsonText) {
    try {
        const json::Value root = json::Parse(jsonText);
        if (!root.IsObject() || ReadString(root, "type") != PacketCrossGameTransition) {
            return std::nullopt;
        }

        const auto fromGame = GameFromString(ReadString(root, "fromGame"));
        const auto toGame = GameFromString(ReadString(root, "toGame"));
        if (!fromGame.has_value() || !toGame.has_value()) {
            return std::nullopt;
        }

        CrossGameTransition transition;
        transition.fromGame = *fromGame;
        transition.toGame = *toGame;
        transition.toEntrance = ReadString(root, "toEntrance");
        if (const json::Value* nativeId = root.Find("toNativeId");
            nativeId != nullptr && nativeId->IsNumber() && nativeId->AsNumber() >= 0) {
            transition.toNativeId = static_cast<uint32_t>(nativeId->AsNumber());
        }
        if (const json::Value* ootAge = root.Find("ootAge");
            ootAge != nullptr && ootAge->IsNumber() && ootAge->AsNumber() >= 0) {
            transition.ootAge = static_cast<uint32_t>(ootAge->AsNumber());
        }
        return transition;
    } catch (...) {
        return std::nullopt;
    }
}

namespace {

std::optional<NetworkEvent> ParseEventPacket(const std::string& jsonText, const char* packetType,
                                             NetworkEventType eventType) {
    try {
        const json::Value root = json::Parse(jsonText);
        if (!root.IsObject() || ReadString(root, "type") != packetType) {
            return std::nullopt;
        }

        const auto checkGame = GameFromString(ReadString(root, "checkGame"));
        const auto itemGame = GameFromString(ReadString(root, "itemGame"));
        if (!checkGame.has_value() || !itemGame.has_value()) {
            return std::nullopt;
        }

        return NetworkEvent{
            .type = eventType,
            .targetPlayer = ReadU16(root, "targetPlayer"),
            .sourcePlayer = ReadU16(root, "sourcePlayer"),
            .checkId = ReadString(root, "checkId"),
            .checkGame = *checkGame,
            .item = {
                .game = *itemGame,
                .id = ReadString(root, "itemId"),
                .name = ReadString(root, "itemName"),
            },
            .deliveryKey = ReadString(root, "deliveryKey"),
        };
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

std::optional<NetworkEvent> ParseGiveItemPacket(const std::string& jsonText) {
    return ParseEventPacket(jsonText, PacketGiveItem, NetworkEventType::GiveItem);
}

std::optional<NetworkEvent> ParseCheckCompletePacket(const std::string& jsonText) {
    return ParseEventPacket(jsonText, PacketCheckComplete, NetworkEventType::CheckComplete);
}

} // namespace ootmm::anchor
