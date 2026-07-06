// A headless stand-in for a real port (SoH / 2S2H), used to exercise the
// coordinator's save-and-relaunch loop deterministically without launching a
// GUI game. It reads the BootConfig the coordinator handed it (via the
// OOTMM_BOOT_CONFIG environment variable), then:
//   * on the first OoT boot (no boot entrance) it simulates the player walking
//     into a cross-game load zone by emitting a CROSS_GAME_TRANSITION packet for
//     the first OoT->other-game entrance in the seed, then exits;
//   * on any boot that was reached via a cross-game entrance, it simulates the
//     player quitting (exits with no transition).
// This lets an automated test confirm the coordinator boots the start game,
// performs the handoff, and boots the destination game at the right entrance.

#include "ootmm/AnchorBridge.hpp"
#include "ootmm/BootConfig.hpp"
#include "ootmm/FilePacketBridge.hpp"
#include "ootmm/Seed.hpp"

#include <cstdlib>
#include <iostream>

int main() {
    const char* configPath = std::getenv("OOTMM_BOOT_CONFIG");
    if (configPath == nullptr) {
        std::cerr << "[stub] OOTMM_BOOT_CONFIG not set\n";
        return 1;
    }

    const auto config = ootmm::LoadBootConfigFromFile(configPath);
    if (!config.has_value()) {
        std::cerr << "[stub] could not read boot config at " << configPath << "\n";
        return 1;
    }

    ootmm::Seed seed;
    try {
        seed = ootmm::LoadSeedFromFile(config->seedPath);
    } catch (const std::exception& error) {
        std::cerr << "[stub] could not load seed: " << error.what() << "\n";
        return 1;
    }

    std::cout << "[stub] booted game=" << ootmm::ToString(config->bootGame);
    if (config->bootEntrance.has_value()) {
        std::cout << " entrance=" << *config->bootEntrance;
    }
    std::cout << "\n";

    if (config->bootGame == ootmm::Game::Oot && !config->bootEntrance.has_value()) {
        for (const ootmm::EntranceMapping& entrance : seed.entrances) {
            if (entrance.fromGame == ootmm::Game::Oot && entrance.IsCrossGame()) {
                const ootmm::CrossGameTransition transition{ entrance.fromGame, entrance.toGame, entrance.to,
                                                             entrance.toNativeId };
                ootmm::AppendPacketLines(config->outboxPath,
                                         { ootmm::anchor::BuildCrossGameTransitionPacket(seed, transition) });
                std::cout << "[stub] emitted cross-game transition to " << ootmm::ToString(entrance.toGame) << "\n";
                return 0;
            }
        }
        std::cerr << "[stub] no cross-game entrance found in seed\n";
        return 1;
    }

    std::cout << "[stub] simulating quit (no transition)\n";
    return 0;
}
