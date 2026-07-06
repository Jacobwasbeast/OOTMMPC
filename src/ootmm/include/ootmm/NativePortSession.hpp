#pragma once

#include "ootmm/GrantApplier.hpp"
#include "ootmm/PortBridge.hpp"

#include <filesystem>
#include <string>

namespace ootmm {

class NativePortSession final {
  public:
    NativePortSession(IGrantOperationAdapter& grantAdapter, INetworkAdapter& networkAdapter, NativePort nativePort);

    void LoadSeed(Seed seed);
    void LoadSeedFile(const std::filesystem::path& path);
    [[nodiscard]] bool HasSeed() const;

    void OnCheckCollected(Game game, const std::string& checkId);
    [[nodiscard]] const EntranceMapping* FindEntranceOverride(Game game, uint32_t fromNativeId) const;
    [[nodiscard]] bool OnRemoteGiveItemJson(const std::string& jsonText);
    std::size_t PumpQueuedItems(std::size_t maxItems = 16);

    [[nodiscard]] const Runtime& GetRuntime() const;
    [[nodiscard]] Runtime& GetRuntime();

  private:
    bool hasSeed_ = false;
    Game activeGame_ = Game::Oot; // the game this port runs, for remote check-complete restores
    ResolvingGameAdapter resolvingAdapter_;
    PortBridge bridge_;
};

} // namespace ootmm
