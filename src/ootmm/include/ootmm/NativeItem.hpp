#pragma once

#include "ootmm/ItemGrant.hpp"

#include <optional>
#include <string>

namespace ootmm {

enum class NativePort {
    ShipOfHarkinian,
    TwoShip2Harkinian,
};

struct NativeItemSymbol {
    NativePort port = NativePort::ShipOfHarkinian;
    Game game = Game::Oot;
    std::string itemId;
    std::string itemEnum;
    std::string getItemEnum;
    std::string randomizerEnum;
};

[[nodiscard]] std::optional<NativeItemSymbol> ResolveNativeItemSymbol(const GrantOperation& operation, NativePort port);
[[nodiscard]] std::optional<NativeItemSymbol> ResolveNativeItemSymbol(const ItemRef& item, Game activeGame, NativePort port);
[[nodiscard]] std::string ToString(NativePort port);

} // namespace ootmm
