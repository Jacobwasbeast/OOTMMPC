#pragma once

#include "ootmm/Runtime.hpp"

#include <optional>
#include <vector>

namespace ootmm::anchor {

inline constexpr const char* PacketHandshake = "OOTMM_HANDSHAKE";
inline constexpr const char* PacketCheckComplete = "OOTMM_CHECK_COMPLETE";
inline constexpr const char* PacketGiveItem = "OOTMM_GIVE_ITEM";
inline constexpr const char* PacketSyncState = "OOTMM_SYNC_STATE";
inline constexpr const char* PacketCrossGameTransition = "OOTMM_CROSS_GAME_TRANSITION";
// Full check catalog (every checkId in this player's world), sent once on join for /list-checks.
inline constexpr const char* PacketCheckCatalog = "OOTMM_CHECK_CATALOG";

[[nodiscard]] std::string BuildHandshakePacket(const Seed& seed, const std::string& clientName);
// Local player's catalog: playerId + every checkId, comma-joined (ids are OOT_/MM_ prefixed and
// comma-free, so each id's game is recoverable).
[[nodiscard]] std::string BuildCheckCatalogPacket(const Seed& seed);
[[nodiscard]] std::string BuildCheckCompletePacket(const Seed& seed, const NetworkEvent& event);
[[nodiscard]] std::string BuildGiveItemPacket(const Seed& seed, const NetworkEvent& event);
[[nodiscard]] std::vector<std::string> BuildPacketsForEvent(const Seed& seed, const NetworkEvent& event);
[[nodiscard]] std::optional<NetworkEvent> ParseGiveItemPacket(const std::string& jsonText);
// Check-complete event, emitted for EVERY collected check (give-item packets only for
// remote-owned items); the launcher's tracker subscribes to these.
[[nodiscard]] std::optional<NetworkEvent> ParseCheckCompletePacket(const std::string& jsonText);

// Cross-game handoff signal: a port writes it to its outbox on crossing; the coordinator
// tears the port down and boots the destination port at transition.toNativeId.
[[nodiscard]] std::string BuildCrossGameTransitionPacket(const Seed& seed, const CrossGameTransition& transition);
[[nodiscard]] std::optional<CrossGameTransition> ParseCrossGameTransitionPacket(const std::string& jsonText);

} // namespace ootmm::anchor
