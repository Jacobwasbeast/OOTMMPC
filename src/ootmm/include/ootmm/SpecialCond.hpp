#pragma once

#include "ootmm/Runtime.hpp"

#include <cstddef>
#include <string>

namespace ootmm {

// og Config_SpecialCond (config.c:48-305): counts one condition's enabled item families across
// BOTH games via the durable granted-item ledger; shared items count exactly once.
[[nodiscard]] std::size_t SpecialCondOwned(const Runtime& runtime, const Seed::SpecialCond& cond);

// True when the named condition (BRIDGE / MOON / LACS / GANON_BK / MAJORA) is satisfied, i.e.
// owned >= cond.count. Absent or zero-count conditions are trivially satisfied (og parity).
[[nodiscard]] bool SpecialCondSatisfied(const Runtime& runtime, const Seed& seed, const std::string& name);

} // namespace ootmm
