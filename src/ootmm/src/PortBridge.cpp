#include "ootmm/PortBridge.hpp"

namespace ootmm {

PortBridge::PortBridge(IGameAdapter& gameAdapter, INetworkAdapter& networkAdapter)
    : gameAdapter_(gameAdapter), networkAdapter_(networkAdapter) {}

void PortBridge::LoadSeed(Seed seed) {
    runtime_.LoadSeed(std::move(seed));
}

const Runtime& PortBridge::GetRuntime() const {
    return runtime_;
}

Runtime& PortBridge::GetRuntime() {
    return runtime_;
}

void PortBridge::OnCheckCollected(Game game, const std::string& checkId) {
    for (const NetworkEvent& event : runtime_.CompleteCheck(game, checkId)) {
        networkAdapter_.SendEvent(event);
    }
}

bool PortBridge::OnRemoteItem(const NetworkEvent& event) {
    return runtime_.ReceiveRemoteItem(event);
}

std::size_t PortBridge::PumpQueuedItems(std::size_t maxItems) {
    std::size_t granted = 0;
    while (granted < maxItems) {
        std::optional<ReceivedItem> item = runtime_.PopQueuedItem();
        if (!item.has_value()) {
            break;
        }
        // Grant-time idempotence: the item queue is persisted and replayed on every boot/handoff,
        // so an entry that was already granted on a prior launch must NOT be granted again (else
        // progressive upgrades climb a tier each round trip). Drop it and keep draining.
        if (runtime_.IsItemGranted(item->deliveryKey)) {
            continue;
        }
        gameAdapter_.GrantItem(*item);
        // Records the granted key AND marks it unconfirmed until a native save persists it, so a grant
        // whose ledger is persisted without a save is replayed on load instead of lost forever.
        runtime_.RecordGrant(*item);
        ++granted;
    }
    return granted;
}

} // namespace ootmm
