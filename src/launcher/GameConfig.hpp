#pragma once

// Editor for a port's own config file (shipofharkinian.json / 2ship's config): typed get/set on
// dotted paths (CVars nested one object level per dot segment under the "CVars" root). Writes back
// preserving untouched keys WITH their original numeric type — the ports read ints and floats
// distinctly, so this uses a type-preserving JSON DOM (nlohmann), not the launcher's double-only
// SimpleJson. Edits target a game that is NOT running (the ports write config on exit).

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace ootmm::launcher {

class GameConfig {
  public:
    GameConfig();
    ~GameConfig();
    GameConfig(GameConfig&&) noexcept;
    GameConfig& operator=(GameConfig&&) noexcept;
    GameConfig(const GameConfig&) = delete;
    GameConfig& operator=(const GameConfig&) = delete;

    // Missing file yields an empty document so a fresh install can still be configured.
    bool Load(const std::filesystem::path& path);
    [[nodiscard]] bool Loaded() const;
    [[nodiscard]] const std::filesystem::path& Path() const;

    // Writes the document back (4-space indent, matching the ports' own output).
    bool Save();

    // Absolute dotted paths into the document (e.g. "Window.Backend.Id").
    [[nodiscard]] int GetIntAt(const std::string& path, int fallback) const;
    [[nodiscard]] double GetFloatAt(const std::string& path, double fallback) const;
    [[nodiscard]] bool GetBoolAt(const std::string& path, bool fallback) const;
    [[nodiscard]] std::string GetStringAt(const std::string& path, const std::string& fallback) const;
    void SetIntAt(const std::string& path, int value);
    void SetFloatAt(const std::string& path, double value);
    void SetBoolAt(const std::string& path, bool value);
    void SetStringAt(const std::string& path, const std::string& value);

    // CVar convenience: dotted key under the "CVars" root (e.g. "gSettings.Fullscreen").
    [[nodiscard]] int GetCVarInt(const std::string& key, int fallback) const;
    [[nodiscard]] double GetCVarFloat(const std::string& key, double fallback) const;
    [[nodiscard]] std::string GetCVarString(const std::string& key, const std::string& fallback) const;
    void SetCVarInt(const std::string& key, int value);
    void SetCVarFloat(const std::string& key, double value);
    void SetCVarString(const std::string& key, const std::string& value);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ootmm::launcher
