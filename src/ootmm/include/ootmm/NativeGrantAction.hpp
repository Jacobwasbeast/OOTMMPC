#pragma once

#include "ootmm/GrantApplier.hpp"

#include <optional>
#include <string>

namespace ootmm {

enum class NativeGrantActionKind {
    GiveRawItem,
    GiveRandomizerItem,
    ChangeRupees,
    ChangeAmmo,
    ChangeHealth,
    AddHealthCapacity,
    AddMagic,
    AddToken,
    SetDungeonItem,
    AddDungeonKey,
    SetQuestFlag,
    ActivateOwl,
};

struct NativeGrantAction {
    NativePort port = NativePort::ShipOfHarkinian;
    NativeGrantActionKind kind = NativeGrantActionKind::GiveRawItem;
    std::string functionName;
    std::string primarySymbol;
    std::string secondarySymbol;
    std::string itemId;
    int amount = 1;
};

[[nodiscard]] std::optional<NativeGrantAction> ResolveNativeGrantAction(const ResolvedGrant& grant, NativePort port);
[[nodiscard]] std::string ToString(NativeGrantActionKind kind);

} // namespace ootmm
