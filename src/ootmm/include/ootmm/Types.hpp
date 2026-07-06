#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ootmm {

enum class Game {
    Oot,
    Mm,
};

enum class NetworkEventType {
    CheckComplete,
    GiveItem,
    SyncState,
};

struct ItemRef {
    Game game = Game::Oot;
    std::string id;
    std::string name;
};

struct CheckRef {
    Game game = Game::Oot;
    std::string id;
    std::string name;
    std::vector<std::string> aliases;
    // Physical identity in the vanilla N64 game, used by the ports to resolve a
    // check to their native check by (scene, type, flag) when the name differs.
    // sceneId is the vanilla scene id; type is OoTMM's category ("chest",
    // "collectible", "gs", "sf", "npc", "shop", ...); flag is the vanilla
    // flag/index for flag-based types (absent for symbolic types like npc).
    std::optional<uint32_t> sceneId;
    std::string type;
    std::optional<uint32_t> flag;
};

struct ItemPlacement {
    CheckRef check;
    ItemRef item;
    uint16_t ownerPlayer = 1;
    uint16_t sourcePlayer = 1;
    bool major = false;

    [[nodiscard]] std::string DeliveryKey() const;
};

struct EntranceMapping {
    Game fromGame = Game::Oot;
    std::string from;
    std::optional<uint32_t> fromNativeId;
    Game toGame = Game::Oot;
    std::string to;
    std::optional<uint32_t> toNativeId;

    // True when traversing this entrance hands control to the other game (the
    // OoTMM N64 build flags these with MASK_FOREIGN_ENTRANCE = 0x80000000).
    [[nodiscard]] bool IsCrossGame() const { return fromGame != toGame; }
};

// Emitted by a running port when the player crosses an entrance into the other
// game. The coordinator consumes it, tears down the active port, and boots the
// destination port positioned at toNativeId.
struct CrossGameTransition {
    Game fromGame = Game::Oot;
    Game toGame = Game::Mm;
    std::string toEntrance;
    std::optional<uint32_t> toNativeId;
    // OoT Link's age at the handoff (0 = adult, 1 = child, OoT convention). Set on
    // OoT -> MM transitions so MM can keep Link's age (OoTMM crossAge).
    std::optional<uint32_t> ootAge;
};

struct StartingItem {
    ItemRef item;
    uint16_t count = 1;
};

struct ReceivedItem {
    ItemRef item;
    uint16_t sourcePlayer = 1;
    std::string sourceCheckId;
    std::string deliveryKey;
    bool fromRemote = false;
};

struct NetworkEvent {
    NetworkEventType type = NetworkEventType::CheckComplete;
    uint16_t targetPlayer = 1;
    uint16_t sourcePlayer = 1;
    std::string checkId;
    Game checkGame = Game::Oot;
    ItemRef item;
    std::string deliveryKey;
};

[[nodiscard]] std::string ToString(Game game);
[[nodiscard]] std::optional<Game> GameFromString(const std::string& value);

} // namespace ootmm
