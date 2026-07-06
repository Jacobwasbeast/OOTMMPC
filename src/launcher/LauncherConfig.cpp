#include "LauncherConfig.hpp"

#include "SimpleJson.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace ootmm::launcher {

namespace fs = std::filesystem;

LauncherConfig LoadLauncherConfig(const fs::path& path) {
    LauncherConfig config;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return config;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    try {
        const json::Value root = json::Parse(buffer.str());
        if (!root.IsObject()) {
            return config;
        }
        const auto str = [&](const char* key, std::string& out) {
            if (const json::Value* v = root.Find(key); v != nullptr && v->IsString()) {
                out = v->AsString();
            }
        };
        const auto boolean = [&](const char* key, bool& out) {
            if (const json::Value* v = root.Find(key); v != nullptr && v->IsBool()) {
                out = v->AsBool();
            }
        };
        const auto number = [&](const char* key, double fallback) -> double {
            const json::Value* v = root.Find(key);
            return (v != nullptr && v->IsNumber()) ? v->AsNumber() : fallback;
        };
        str("seedPath", config.seedPath);
        str("ootExe", config.ootExe);
        str("mmExe", config.mmExe);
        str("multiworldAddress", config.multiworldAddress);
        str("playerName", config.playerName);
        str("playerModelOot", config.playerModelOot);
        str("playerModelMm", config.playerModelMm);
        boolean("hostRelay", config.hostRelay);
        boolean("showPlayerNametags", config.showPlayerNametags);
        boolean("enablePvp", config.enablePvp);
        config.relayPort = std::clamp(static_cast<int>(number("relayPort", config.relayPort)), 1, 65535);
        config.theme = std::clamp(static_cast<int>(number("theme", config.theme)), 0, 2);
        config.uiScale = std::clamp(static_cast<float>(number("uiScale", config.uiScale)), 0.75f, 2.0f);
        config.sidebarWidth = std::clamp(static_cast<int>(number("sidebarWidth", config.sidebarWidth)), 280, 640);
        boolean("showSidebar", config.showSidebar);
        boolean("allowDebugMenus", config.allowDebugMenus);
        boolean("speedMatchFramerate", config.speedMatchFramerate);
    } catch (...) {
        // Malformed config: fall back to defaults rather than failing the launcher.
    }
    return config;
}

void SaveLauncherConfig(const fs::path& path, const LauncherConfig& config) {
    std::ostringstream out;
    out << "{\"seedPath\":" << json::EscapeString(config.seedPath);
    out << ",\"ootExe\":" << json::EscapeString(config.ootExe);
    out << ",\"mmExe\":" << json::EscapeString(config.mmExe);
    out << ",\"multiworldAddress\":" << json::EscapeString(config.multiworldAddress);
    out << ",\"playerName\":" << json::EscapeString(config.playerName);
    out << ",\"playerModelOot\":" << json::EscapeString(config.playerModelOot);
    out << ",\"playerModelMm\":" << json::EscapeString(config.playerModelMm);
    out << ",\"hostRelay\":" << (config.hostRelay ? "true" : "false");
    out << ",\"showPlayerNametags\":" << (config.showPlayerNametags ? "true" : "false");
    out << ",\"enablePvp\":" << (config.enablePvp ? "true" : "false");
    out << ",\"relayPort\":" << config.relayPort;
    out << ",\"theme\":" << config.theme;
    out << ",\"uiScale\":" << config.uiScale;
    out << ",\"sidebarWidth\":" << config.sidebarWidth;
    out << ",\"showSidebar\":" << (config.showSidebar ? "true" : "false");
    out << ",\"allowDebugMenus\":" << (config.allowDebugMenus ? "true" : "false");
    out << ",\"speedMatchFramerate\":" << (config.speedMatchFramerate ? "true" : "false");
    out << "}";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << out.str();
}

namespace {

constexpr const char* kDisabledSuffix = ".disabled";

bool HasSuffix(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool IsRequiredMod(const std::string& name) {
    // Cross-game real-asset archives: disabling them breaks foreign item rendering.
    return name == "ootmm_mm_models.o2r" || name == "ootmm_oot_models.o2r";
}

bool IsLauncherGeneratedMod(const std::string& name) {
    // Internal archives the launcher regenerates each session (remote players' repacked model
    // sets, the local player's canonical-override skin) — not user-managed mods.
    return name.rfind("zz_coop_", 0) == 0 || name.rfind("zz_localmodel_", 0) == 0;
}

} // namespace

std::vector<ModEntry> ScanMods(const fs::path& exeDir) {
    std::vector<ModEntry> mods;
    const fs::path modsDir = exeDir / "mods";
    std::error_code ec;
    if (!fs::is_directory(modsDir, ec)) {
        return mods;
    }
    for (const fs::directory_entry& entry : fs::directory_iterator(modsDir, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        std::string fileName = entry.path().filename().string();
        bool enabled = true;
        std::string display = fileName;
        if (HasSuffix(fileName, kDisabledSuffix)) {
            enabled = false;
            display = fileName.substr(0, fileName.size() - std::string(kDisabledSuffix).size());
        }
        std::string lower = display;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!HasSuffix(lower, ".o2r") && !HasSuffix(lower, ".otr")) {
            continue;
        }
        if (IsLauncherGeneratedMod(display)) {
            continue;
        }
        ModEntry mod;
        mod.path = entry.path();
        mod.name = display;
        mod.onDiskEnabled = enabled;
        mod.required = IsRequiredMod(lower);
        mod.enabled = mod.required ? true : enabled;
        mods.push_back(std::move(mod));
    }
    std::sort(mods.begin(), mods.end(), [](const ModEntry& a, const ModEntry& b) {
        if (a.required != b.required) {
            return a.required > b.required; // required archives listed first
        }
        return a.name < b.name;
    });
    return mods;
}

std::vector<std::string> ApplyModStates(std::vector<ModEntry>& mods) {
    std::vector<std::string> errors;
    for (ModEntry& mod : mods) {
        const bool want = mod.required ? true : mod.enabled;
        if (want == mod.onDiskEnabled) {
            continue;
        }
        fs::path target = mod.path.parent_path();
        target /= want ? mod.name : (mod.name + kDisabledSuffix);
        std::error_code ec;
        fs::rename(mod.path, target, ec);
        if (ec) {
            errors.push_back(mod.name + ": " + ec.message());
            continue;
        }
        mod.path = target;
        mod.onDiskEnabled = want;
        mod.enabled = want;
    }
    return errors;
}

} // namespace ootmm::launcher
