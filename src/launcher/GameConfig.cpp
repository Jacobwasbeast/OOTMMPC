#include "GameConfig.hpp"

#include <nlohmann/json.hpp>

#include <fstream>

namespace ootmm::launcher {

namespace fs = std::filesystem;
using nlohmann::json;

struct GameConfig::Impl {
    json root = json::object();
    fs::path path;
    bool loaded = false;
};

GameConfig::GameConfig() : impl_(std::make_unique<Impl>()) {
}
GameConfig::~GameConfig() = default;
GameConfig::GameConfig(GameConfig&&) noexcept = default;
GameConfig& GameConfig::operator=(GameConfig&&) noexcept = default;

namespace {

// Walks a dotted path; returns nullptr when any segment is missing or non-object.
const json* Find(const json& root, const std::string& path) {
    const json* node = &root;
    size_t start = 0;
    while (true) {
        const size_t dot = path.find('.', start);
        const std::string segment = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!node->is_object()) {
            return nullptr;
        }
        const auto it = node->find(segment);
        if (it == node->end()) {
            return nullptr;
        }
        node = &*it;
        if (dot == std::string::npos) {
            return node;
        }
        start = dot + 1;
    }
}

json& Ensure(json& root, const std::string& path) {
    json* node = &root;
    size_t start = 0;
    while (true) {
        const size_t dot = path.find('.', start);
        const std::string segment = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!node->is_object()) {
            *node = json::object();
        }
        node = &(*node)[segment];
        if (dot == std::string::npos) {
            return *node;
        }
        start = dot + 1;
    }
}

} // namespace

bool GameConfig::Load(const fs::path& path) {
    impl_->path = path;
    impl_->root = json::object();
    impl_->loaded = false;

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        // Fresh install: no config yet. Start empty — the game merges defaults for absent keys.
        impl_->loaded = true;
        return true;
    }
    try {
        impl_->root = json::parse(input, nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
        if (!impl_->root.is_object()) {
            impl_->root = json::object();
        }
        impl_->loaded = true;
        return true;
    } catch (...) {
        impl_->root = json::object();
        return false;
    }
}

bool GameConfig::Loaded() const {
    return impl_->loaded;
}

const fs::path& GameConfig::Path() const {
    return impl_->path;
}

bool GameConfig::Save() {
    if (!impl_->loaded || impl_->path.empty()) {
        return false;
    }
    std::ofstream output(impl_->path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output << impl_->root.dump(4);
    return static_cast<bool>(output);
}

int GameConfig::GetIntAt(const std::string& path, int fallback) const {
    const json* node = Find(impl_->root, path);
    if (node == nullptr) {
        return fallback;
    }
    if (node->is_number_integer() || node->is_number_unsigned()) {
        return node->get<int>();
    }
    if (node->is_boolean()) {
        return node->get<bool>() ? 1 : 0;
    }
    if (node->is_number_float()) {
        return static_cast<int>(node->get<double>());
    }
    return fallback;
}

double GameConfig::GetFloatAt(const std::string& path, double fallback) const {
    const json* node = Find(impl_->root, path);
    return node != nullptr && node->is_number() ? node->get<double>() : fallback;
}

bool GameConfig::GetBoolAt(const std::string& path, bool fallback) const {
    const json* node = Find(impl_->root, path);
    if (node == nullptr) {
        return fallback;
    }
    if (node->is_boolean()) {
        return node->get<bool>();
    }
    if (node->is_number()) {
        return node->get<double>() != 0.0;
    }
    return fallback;
}

std::string GameConfig::GetStringAt(const std::string& path, const std::string& fallback) const {
    const json* node = Find(impl_->root, path);
    return node != nullptr && node->is_string() ? node->get<std::string>() : fallback;
}

void GameConfig::SetIntAt(const std::string& path, int value) {
    Ensure(impl_->root, path) = value;
}

void GameConfig::SetFloatAt(const std::string& path, double value) {
    Ensure(impl_->root, path) = value;
}

void GameConfig::SetBoolAt(const std::string& path, bool value) {
    Ensure(impl_->root, path) = value;
}

void GameConfig::SetStringAt(const std::string& path, const std::string& value) {
    Ensure(impl_->root, path) = value;
}

int GameConfig::GetCVarInt(const std::string& key, int fallback) const {
    return GetIntAt("CVars." + key, fallback);
}

double GameConfig::GetCVarFloat(const std::string& key, double fallback) const {
    return GetFloatAt("CVars." + key, fallback);
}

std::string GameConfig::GetCVarString(const std::string& key, const std::string& fallback) const {
    return GetStringAt("CVars." + key, fallback);
}

void GameConfig::SetCVarInt(const std::string& key, int value) {
    SetIntAt("CVars." + key, value);
}

void GameConfig::SetCVarFloat(const std::string& key, double value) {
    SetFloatAt("CVars." + key, value);
}

void GameConfig::SetCVarString(const std::string& key, const std::string& value) {
    SetStringAt("CVars." + key, value);
}

} // namespace ootmm::launcher
