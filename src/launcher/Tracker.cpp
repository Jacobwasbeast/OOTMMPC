#include "Tracker.hpp"

#include "SimpleJson.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>

namespace ootmm::launcher {

namespace {

std::string CheckKey(Game game, const std::string& checkId) {
    return ToString(game) + (":" + checkId);
}

std::string NowText() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", local.tm_hour, local.tm_min, local.tm_sec);
    return buffer;
}

} // namespace

void Tracker::LoadSeed(const Seed& seed) {
    checks_.clear();
    byKey_.clear();
    log_.clear();
    remoteCounts_.clear();
    remoteReceipts_.clear();
    collectedCount_ = 0;
    ++revision_;
    checks_.reserve(seed.placements.size());
    for (const ItemPlacement& placement : seed.placements) {
        TrackerCheck check;
        check.game = placement.check.game;
        check.checkId = placement.check.id;
        check.name = !placement.check.name.empty() ? placement.check.name : placement.check.id;
        check.type = placement.check.type;
        check.itemId = placement.item.id;
        check.itemName = !placement.item.name.empty() ? placement.item.name : placement.item.id;
        check.ownerPlayer = placement.ownerPlayer;
        byKey_[CheckKey(check.game, check.checkId)] = checks_.size();
        checks_.push_back(std::move(check));
    }
}

void Tracker::MarkCollected(Game game, const std::string& checkId, const std::string& itemName,
                            uint16_t targetPlayer, uint16_t localPlayer, bool logIt) {
    std::string checkName = checkId;
    const auto it = byKey_.find(CheckKey(game, checkId));
    bool fresh = true;
    if (it != byKey_.end()) {
        TrackerCheck& check = checks_[it->second];
        fresh = !check.collected;
        if (fresh) {
            check.collected = true;
            ++collectedCount_;
            ++revision_;
        }
        checkName = check.name;
    }
    if (logIt && fresh) {
        CheckLogEntry entry;
        entry.time = NowText();
        entry.game = game;
        entry.checkName = checkName;
        entry.itemName = itemName;
        entry.targetPlayer = targetPlayer;
        entry.sentToRemote = targetPlayer != localPlayer;
        log_.push_back(std::move(entry));
    }
}

bool Tracker::AddRemoteReceipt(const std::string& itemId, const std::string& deliveryKey) {
    if (itemId.empty() || deliveryKey.empty()) {
        return false;
    }
    auto [it, inserted] = remoteReceipts_.emplace(deliveryKey, true);
    if (!inserted) {
        return false;
    }
    ++remoteCounts_[itemId];
    ++revision_;
    return true;
}

std::unordered_map<std::string, int> Tracker::ItemCounts(const Seed& seed) const {
    std::unordered_map<std::string, int> counts;
    for (const StartingItem& starting : seed.startingItems) {
        counts[starting.item.id] += starting.count;
    }
    for (const TrackerCheck& check : checks_) {
        if (check.collected && check.ownerPlayer == seed.playerId) {
            ++counts[check.itemId];
        }
    }
    for (const auto& [itemId, count] : remoteCounts_) {
        counts[itemId] += count;
    }
    return counts;
}

void Tracker::SyncFromStateFile(const std::filesystem::path& statePath) {
    std::ifstream input(statePath, std::ios::binary);
    if (!input) {
        return;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    try {
        const json::Value root = json::Parse(buffer.str());
        if (!root.IsObject()) {
            return;
        }
        const json::Value* completed = root.Find("completedChecks");
        if (completed == nullptr || !completed->IsArray()) {
            return;
        }
        for (const json::Value& entry : completed->AsArray()) {
            if (!entry.IsString()) {
                continue;
            }
            const std::string& key = entry.AsString();
            const size_t colon = key.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            const auto game = GameFromString(key.substr(0, colon));
            if (!game.has_value()) {
                continue;
            }
            MarkCollected(*game, key.substr(colon + 1), {}, 0, 0, /*logIt=*/false);
        }
    } catch (...) {
        // A torn read while the port is mid-write is harmless; the next sync catches up.
    }
}

} // namespace ootmm::launcher
