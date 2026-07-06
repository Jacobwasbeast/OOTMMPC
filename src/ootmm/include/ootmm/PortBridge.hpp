#pragma once

#include "ootmm/Runtime.hpp"

namespace ootmm {

class IGameAdapter {
  public:
    virtual ~IGameAdapter() = default;
    virtual void GrantItem(const ReceivedItem& item) = 0;
};

class INetworkAdapter {
  public:
    virtual ~INetworkAdapter() = default;
    virtual void SendEvent(const NetworkEvent& event) = 0;
};

class PortBridge {
  public:
    explicit PortBridge(IGameAdapter& gameAdapter, INetworkAdapter& networkAdapter);

    void LoadSeed(Seed seed);
    [[nodiscard]] const Runtime& GetRuntime() const;
    [[nodiscard]] Runtime& GetRuntime();

    void OnCheckCollected(Game game, const std::string& checkId);
    bool OnRemoteItem(const NetworkEvent& event);
    std::size_t PumpQueuedItems(std::size_t maxItems = 16);

  private:
    Runtime runtime_;
    IGameAdapter& gameAdapter_;
    INetworkAdapter& networkAdapter_;
};

} // namespace ootmm
