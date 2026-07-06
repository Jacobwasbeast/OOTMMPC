#pragma once

#include "ootmm/NativeItem.hpp"
#include "ootmm/PortBridge.hpp"

#include <optional>

namespace ootmm {

struct ResolvedGrant {
    ReceivedItem delivery;
    GrantOperation operation;
    std::optional<NativeItemSymbol> nativeSymbol;
};

class IGrantOperationAdapter {
  public:
    virtual ~IGrantOperationAdapter() = default;
    [[nodiscard]] virtual Game ActiveGame() const = 0;
    virtual void ApplyGrant(const ResolvedGrant& grant) = 0;
};

class ResolvingGameAdapter final : public IGameAdapter {
  public:
    ResolvingGameAdapter(IGrantOperationAdapter& operationAdapter, NativePort nativePort);

    void GrantItem(const ReceivedItem& item) override;

  private:
    IGrantOperationAdapter& operationAdapter_;
    NativePort nativePort_;
};

} // namespace ootmm
