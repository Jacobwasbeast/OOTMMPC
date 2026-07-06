#pragma once

#include "ootmm/Types.hpp"

#include <string>
#include <vector>

namespace ootmm {

enum class GrantOperationKind {
    RawItem,
    AddRupees,
    AddAmmo,
    AddHealth,
    AddHealthCapacity,
    AddMagic,
    AddToken,
    AddDungeonItem,
    SetQuestFlag,
    ActivateOwl,
};

struct GrantOperation {
    GrantOperationKind kind = GrantOperationKind::RawItem;
    Game targetGame = Game::Oot;
    std::string itemId;
    std::string slot;
    int amount = 1;
};

[[nodiscard]] std::vector<GrantOperation> ResolveGrantOperations(const ItemRef& item, Game activeGame);
[[nodiscard]] std::string ToString(GrantOperationKind kind);

} // namespace ootmm
