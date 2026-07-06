#include "ootmm/GrantApplier.hpp"

namespace ootmm {

ResolvingGameAdapter::ResolvingGameAdapter(IGrantOperationAdapter& operationAdapter, NativePort nativePort)
    : operationAdapter_(operationAdapter), nativePort_(nativePort) {}

void ResolvingGameAdapter::GrantItem(const ReceivedItem& item) {
    const std::vector<GrantOperation> operations = ResolveGrantOperations(item.item, operationAdapter_.ActiveGame());
    for (const GrantOperation& operation : operations) {
        operationAdapter_.ApplyGrant(ResolvedGrant{
            .delivery = item,
            .operation = operation,
            .nativeSymbol = ResolveNativeItemSymbol(operation, nativePort_),
        });
    }
}

} // namespace ootmm
