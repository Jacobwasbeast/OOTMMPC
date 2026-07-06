#pragma once

#include "ootmm/Types.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ootmm {
struct Seed;
}

namespace ootmm::presence {

// One player's live pose, sampled every frame by the running port and fanned out to the
// session via the relay (packet type OOTMM_PLAYER_STATE). Ephemeral: last value wins, never
// persisted, never deduped — it does NOT travel through the durable inbox/outbox item files.
struct PlayerPose {
    uint16_t playerId = 0;
    std::string clientName;
    Game game = Game::Oot;
    int sceneId = -1;
    int roomId = 0;
    float pos[3] = { 0.0f, 0.0f, 0.0f };
    int16_t yaw = 0;
    // Player's shape.yOffset (recomputed by the game every frame: swim bobbing, riding, ...).
    // Puppets that don't replay it draw floating/sunken whenever the sender's is non-zero.
    float yOffset = 0.0f;
    // Player's skelAnime.movementFlags. The player DRAW scales the root limb translation by
    // an age/form factor conditioned on these flags (Player_OverrideLimbDraw* root case);
    // puppets must replay the same scaling or child/form puppets draw floating.
    int moveFlags = 0;
    // OoT: age (0 adult, 1 child). MM: transformation (0 FD, 1 Goron, 2 Zora, 3 Deku, 4 Human).
    int form = 0;
    // Packed limb rotations (x,y,z per limb) copied from the player's SkelAnime joint table,
    // so puppets replay the exact animation pose. Count varies per game/form.
    std::vector<int16_t> jointTable;
    // Identity of the player-model mod archive this player uses ("" = vanilla Link). Remote
    // clients render vanilla Link until they have downloaded + installed this archive.
    std::string modelHash;
    // --- equipment visuals -----------------------------------------------------------------
    // The player draw never renders the skeleton's own hand/sheath/waist limb display lists;
    // it substitutes them BY NAME every frame from equipment state (sword drawn, shield on
    // arm/back, open/closed fist, held item). Custom player models therefore ship garbage or
    // nothing in those skeleton limbs. The sender captures the canonical resource path
    // ("objects/..." without the "__OTR__" prefix) its own draw chose for each of the four
    // equipment limbs; puppets substitute the same path re-rooted into the sender's model
    // namespace. "" = draw the skeleton's limb DL unchanged; "-" = draw nothing (e.g. OoT
    // child sheath with neither sword nor shield).
    std::string dlLeftHand;
    std::string dlRightHand;
    std::string dlSheath;
    std::string dlWaist;
    // Effective PLAYER_MODELTYPE_* of each hand as drawn (OoT: selects gauntlet plate DLs).
    int leftHandType = -1;
    int rightHandType = -1;
    int tunic = 0;    // OoT currentTunic (MM: unused)
    int boots = 0;    // currentBoots (OoT boot models / MM form boots)
    int strength = 0; // OoT strength upgrade (goron bracelet / silver / gold gauntlets)
    int mask = 0;     // currentMask (OoT child trade masks / MM masks), 0 = none
    // Sender frame counter; receivers drop out-of-order states and expire stale players.
    uint32_t seq = 0;
};

// --- Relay packets (newline-JSON, same dialect as AnchorBridge) --------------------------------
[[nodiscard]] std::string BuildPlayerStatePacket(const Seed& seed, const PlayerPose& pose);
[[nodiscard]] std::optional<PlayerPose> ParsePlayerStatePacket(const std::string& jsonText);

// --- Ephemeral game <-> launcher channel --------------------------------------------------------
// Atomic last-value files (write temp + rename): the port writes its own pose to `outPath`
// every frame; the launcher rewrites `inPath` with the latest roster of REMOTE poses. Readers
// tolerate torn/missing files by returning empty.
bool WritePoseFile(const std::filesystem::path& path, const PlayerPose& pose);
[[nodiscard]] std::optional<PlayerPose> ReadPoseFile(const std::filesystem::path& path);
// Launcher-controlled display options carried alongside the roster (a meta line in the same
// file), so toggles apply LIVE to the running game without a reboot.
struct RosterMeta {
    bool showNames = true;  // draw each remote player's client name above their puppet
    bool pvp = false;       // session-wide PvP: puppets get hurtboxes, hits deal real damage
    bool deathLink = false; // session-wide death link: one player's death kills everyone
    // Game-speed percentage (100 = vanilla) the game applies to its logic rate each frame.
    // Driven by the launcher's timer slider, constrained by the server's timer rule.
    int speedPercent = 100;
    // Pace the scaled logic rate fractionally against the render frame rate (keeps the full
    // 1% slider precision) instead of legacy whole-Hz steps. Launcher option, default on.
    bool speedSmooth = true;
};

// --- PvP -----------------------------------------------------------------------------------
// Attacker-authoritative hits: the game that sees ITS weapon collider connect with a remote
// player's puppet emits a PvpHit (outbox -> launcher -> relay). The victim's launcher mirrors
// hits addressed to its player into the roster file for ~2 seconds; the victim's game dedups
// by (sourcePlayer, seq) and applies real damage + knockback to the local player.
struct PvpHit {
    uint16_t sourcePlayer = 0; // attacker
    uint16_t targetPlayer = 0; // victim
    int damage = 0;            // native damage units from the attack collider (1 heart = 16)
    int16_t yaw = 0;           // knockback direction, attacker -> victim (binary angle)
    uint32_t seq = 0;          // attacker-scoped id; victims dedup replays on the last-value channel
};

inline constexpr const char* PacketPvpHit = "OOTMM_PVP_HIT";
[[nodiscard]] std::string BuildPvpHitPacket(const Seed& seed, const PvpHit& hit);
[[nodiscard]] std::optional<PvpHit> ParsePvpHitPacket(const std::string& jsonText);

// --- Death link ------------------------------------------------------------------------------
// When the server's death-link rule is on, a player's death is announced (outbox -> launcher ->
// relay); every other launcher mirrors deaths into its roster file and the game kills the local
// player. Receivers dedup by (sourcePlayer, seq) exactly like PvP hits.
struct DeathEvent {
    uint16_t sourcePlayer = 0; // who died
    uint32_t seq = 0;          // sender-scoped id; receivers dedup replays on the last-value channel
};
inline constexpr const char* PacketDeathLink = "OOTMM_DEATH_LINK";
[[nodiscard]] std::string BuildDeathLinkPacket(const Seed& seed, const DeathEvent& death);
[[nodiscard]] std::optional<DeathEvent> ParseDeathLinkPacket(const std::string& jsonText);

// Session-wide settings any launcher can change mid-game; last writer wins and every launcher
// adopts + re-derives its roster meta, so all clients converge on the same PvP state.
struct SessionMeta {
    bool pvp = false;
    bool deathLink = false;
    // Server timer rule: whether the launcher game-speed slider is available and the
    // percentage range it may take. Defaults mirror the server defaults (0..250, enabled).
    bool timerEnabled = true;
    int timerMin = 0;
    int timerMax = 250;
};
inline constexpr const char* PacketSessionMeta = "OOTMM_SESSION_META";
[[nodiscard]] std::string BuildSessionMetaPacket(const Seed& seed, const SessionMeta& meta);
[[nodiscard]] std::optional<SessionMeta> ParseSessionMetaPacket(const std::string& jsonText);

bool WriteRosterFile(const std::filesystem::path& path, const std::vector<PlayerPose>& roster,
                     const RosterMeta* meta = nullptr, const std::vector<PvpHit>* hits = nullptr,
                     const std::vector<DeathEvent>* deaths = nullptr);
[[nodiscard]] std::vector<PlayerPose> ReadRosterFile(const std::filesystem::path& path,
                                                     RosterMeta* outMeta = nullptr,
                                                     std::vector<PvpHit>* outHits = nullptr,
                                                     std::vector<DeathEvent>* outDeaths = nullptr);

// --- Player-model mod sync (bulk transfer over the relay line protocol) ------------------------
// Flow: on join every launcher announces its player's model archive (name + hash + size).
// A launcher missing that hash requests it; the owner streams base64 chunks. Completed
// archives land in the launcher's model cache keyed by hash; puppets render vanilla Link
// until the archive is installed (and the ports rebooted, when installation needs it).
inline constexpr const char* PacketPlayerState = "OOTMM_PLAYER_STATE";
inline constexpr const char* PacketModAnnounce = "OOTMM_MOD_ANNOUNCE";
inline constexpr const char* PacketModRequest = "OOTMM_MOD_REQUEST";
inline constexpr const char* PacketModChunk = "OOTMM_MOD_CHUNK";

struct ModAnnounce {
    uint16_t playerId = 0;
    Game game = Game::Oot; // which game's mods dir this model archive belongs to
    std::string name;      // display/file name, e.g. "LinkToThePastLink.o2r"
    std::string hash;      // content hash (FNV-1a 64 hex of the archive bytes)
    uint64_t size = 0;
};

struct ModRequest {
    uint16_t playerId = 0; // requester
    uint16_t targetPlayer = 0;
    std::string hash;
};

struct ModChunk {
    uint16_t playerId = 0; // sender/owner
    std::string hash;
    uint32_t seq = 0;
    uint32_t total = 0;
    std::string dataB64;
};

[[nodiscard]] std::string BuildModAnnouncePacket(const Seed& seed, const ModAnnounce& announce);
[[nodiscard]] std::optional<ModAnnounce> ParseModAnnouncePacket(const std::string& jsonText);
[[nodiscard]] std::string BuildModRequestPacket(const Seed& seed, const ModRequest& request);
[[nodiscard]] std::optional<ModRequest> ParseModRequestPacket(const std::string& jsonText);
[[nodiscard]] std::string BuildModChunkPacket(const Seed& seed, const ModChunk& chunk);
[[nodiscard]] std::optional<ModChunk> ParseModChunkPacket(const std::string& jsonText);

// Helpers shared by the launcher-side transfer.
[[nodiscard]] std::string Base64Encode(const uint8_t* data, size_t size);
[[nodiscard]] std::vector<uint8_t> Base64Decode(const std::string& text);
[[nodiscard]] std::string HashBytesHex(const uint8_t* data, size_t size); // FNV-1a 64
[[nodiscard]] std::string HashFileHex(const std::filesystem::path& path); // "" on error

} // namespace ootmm::presence
