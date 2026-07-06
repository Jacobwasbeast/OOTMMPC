#include "ootmm/NativePortSession.hpp"

#include "ootmm/AnchorBridge.hpp"
#include "ootmm/Seed.hpp"

namespace ootmm {

NativePortSession::NativePortSession(IGrantOperationAdapter& grantAdapter, INetworkAdapter& networkAdapter, NativePort nativePort)
    : activeGame_(nativePort == NativePort::ShipOfHarkinian ? Game::Oot : Game::Mm),
      resolvingAdapter_(grantAdapter, nativePort), bridge_(resolvingAdapter_, networkAdapter) {}

void NativePortSession::LoadSeed(Seed seed) {
    bridge_.LoadSeed(std::move(seed));
    hasSeed_ = true;
}

void NativePortSession::LoadSeedFile(const std::filesystem::path& path) {
    LoadSeed(LoadSeedFromFile(path));
}

bool NativePortSession::HasSeed() const {
    return hasSeed_;
}

void NativePortSession::OnCheckCollected(Game game, const std::string& checkId) {
    if (!hasSeed_) {
        return;
    }

    bridge_.OnCheckCollected(game, checkId);
}

const EntranceMapping* NativePortSession::FindEntranceOverride(Game game, uint32_t fromNativeId) const {
    if (!hasSeed_) {
        return nullptr;
    }

    return bridge_.GetRuntime().FindEntrance(game, fromNativeId);
}

bool NativePortSession::OnRemoteGiveItemJson(const std::string& jsonText) {
    if (!hasSeed_) {
        return false;
    }

    // Remote check completions — another player's live collect, or the relay's join replay of
    // the room's durable history — mark the check done so the world matches the server (checks
    // stop re-offering, CSMC reverts to vanilla). Replays of OUR OWN history also restore the
    // locally-owned items (delivery-key dedup keeps resumed saves untouched); everything else
    // arrives in its own GIVE_ITEM packet.
    if (const auto check = anchor::ParseCheckCompletePacket(jsonText); check.has_value()) {
        return bridge_.GetRuntime().ApplyRemoteCheckComplete(*check, activeGame_);
    }

    const auto event = anchor::ParseGiveItemPacket(jsonText);
    if (!event.has_value()) {
        return false;
    }

    return bridge_.OnRemoteItem(*event);
}

std::size_t NativePortSession::PumpQueuedItems(std::size_t maxItems) {
    if (!hasSeed_) {
        return 0;
    }

    return bridge_.PumpQueuedItems(maxItems);
}

const Runtime& NativePortSession::GetRuntime() const {
    return bridge_.GetRuntime();
}

Runtime& NativePortSession::GetRuntime() {
    return bridge_.GetRuntime();
}

} // namespace ootmm
