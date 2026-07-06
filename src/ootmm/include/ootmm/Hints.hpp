#pragma once

#include "ootmm/Seed.hpp"

#include <string>

namespace ootmm::hints {

// og OoTMM hint text builder — a verbatim port of generator src/common/hint.c +
// En_Wonder_Talk.c / Dm_Stk.c, producing engine-agnostic marked-up text. The ports
// translate the markup into their own control codes and do their own line wrapping:
//   "@R" red  "@G" green  "@B" blue  "@Y" yellow  "@T" teal  "@P" pink
//   "@S" silver  "@O" orange  "@0" clear/reset color
//   '\n'  line break        "@b" textbox/box break
// (OoT renders @O as yellow, matching og's TEXT_COLOR_ORANGE alias.)

// Whether the active seed carries a hints section (older seeds don't).
[[nodiscard]] bool HasHints(const Seed& seed);

// The gossip-stone hint for a stone key (raw id | 0x20 grotto | 0x40 moon). Unknown
// keys produce og's deterministic junk fallback (kJunkHints[key % 30]), like og
// Hint_Display does for stones with no record.
[[nodiscard]] std::string GossipText(const Seed& seed, Game stoneGame, int stoneKey);

// Temple of Time altar (OoT). Child: the 3 spiritual-stone locations + what reuniting
// the stones leads to (the Ocarina of Time / Song of Time checks' items). Adult: the 6
// medallion locations + the Ganon Boss Key region when ganonBossKey == 'anywhere'.
[[nodiscard]] std::string AltarChildText(const Seed& seed);
[[nodiscard]] std::string AltarAdultText(const Seed& seed);

// Sheik/Ganon Light Arrows hint (OoT): "Have you found the Light Arrows ...?"
[[nodiscard]] std::string LightArrowsText(const Seed& seed);

// MM Oath to Order hint (moon access): single-region, or the per-region note counts
// when the song is split into notes (detected from the oathToOrder region list).
[[nodiscard]] std::string OathToOrderText(const Seed& seed);

} // namespace ootmm::hints
