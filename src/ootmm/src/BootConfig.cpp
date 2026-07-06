#include "ootmm/BootConfig.hpp"

#include "SimpleJson.hpp"

#include <fstream>
#include <sstream>

namespace ootmm {

std::string SerializeBootConfig(const BootConfig& config) {
    std::ostringstream out;
    out << "{\"seedPath\":" << json::EscapeString(config.seedPath);
    out << ",\"statePath\":" << json::EscapeString(config.statePath);
    out << ",\"inboxPath\":" << json::EscapeString(config.inboxPath);
    out << ",\"outboxPath\":" << json::EscapeString(config.outboxPath);
    if (!config.presenceOutPath.empty()) {
        out << ",\"presenceOutPath\":" << json::EscapeString(config.presenceOutPath);
    }
    if (!config.presenceInPath.empty()) {
        out << ",\"presenceInPath\":" << json::EscapeString(config.presenceInPath);
    }
    if (!config.mmSaveDir.empty()) {
        out << ",\"mmSaveDir\":" << json::EscapeString(config.mmSaveDir);
    }
    if (!config.selftestReportPath.empty()) {
        out << ",\"selftestReportPath\":" << json::EscapeString(config.selftestReportPath);
    }
    out << ",\"allowDebugMenus\":" << (config.allowDebugMenus ? "true" : "false");
    out << ",\"bootGame\":" << json::EscapeString(ToString(config.bootGame));
    if (config.bootEntrance.has_value()) {
        out << ",\"bootEntrance\":" << *config.bootEntrance;
    }
    if (config.ootAge.has_value()) {
        out << ",\"ootAge\":" << *config.ootAge;
    }
    // Decimal string so the full 64-bit handle survives (a JSON number parsed through a double
    // would lose precision).
    if (config.parentWindow.has_value()) {
        out << ",\"parentWindow\":" << json::EscapeString(std::to_string(*config.parentWindow));
    }
    out << "}";
    return out.str();
}

std::optional<BootConfig> ParseBootConfig(const std::string& jsonText) {
    try {
        const json::Value root = json::Parse(jsonText);
        if (!root.IsObject()) {
            return std::nullopt;
        }

        const auto str = [&](const char* key) -> std::string {
            const json::Value* value = root.Find(key);
            return (value != nullptr && value->IsString()) ? value->AsString() : std::string();
        };

        BootConfig config;
        config.seedPath = str("seedPath");
        config.statePath = str("statePath");
        config.inboxPath = str("inboxPath");
        config.outboxPath = str("outboxPath");
        config.presenceOutPath = str("presenceOutPath");
        config.presenceInPath = str("presenceInPath");
        config.mmSaveDir = str("mmSaveDir");
        config.selftestReportPath = str("selftestReportPath");

        if (const json::Value* allowDebug = root.Find("allowDebugMenus");
            allowDebug != nullptr && allowDebug->IsBool()) {
            config.allowDebugMenus = allowDebug->AsBool();
        }

        if (const json::Value* bootGame = root.Find("bootGame"); bootGame != nullptr && bootGame->IsString()) {
            if (const auto parsed = GameFromString(bootGame->AsString()); parsed.has_value()) {
                config.bootGame = *parsed;
            }
        }

        if (const json::Value* bootEntrance = root.Find("bootEntrance");
            bootEntrance != nullptr && bootEntrance->IsNumber() && bootEntrance->AsNumber() >= 0) {
            config.bootEntrance = static_cast<uint32_t>(bootEntrance->AsNumber());
        }
        if (const json::Value* ootAge = root.Find("ootAge");
            ootAge != nullptr && ootAge->IsNumber() && ootAge->AsNumber() >= 0) {
            config.ootAge = static_cast<uint32_t>(ootAge->AsNumber());
        }

        if (const json::Value* parentWindow = root.Find("parentWindow"); parentWindow != nullptr) {
            // Accept the decimal-string form (current writer) or a bare number (tolerant).
            if (parentWindow->IsString()) {
                try {
                    config.parentWindow = static_cast<uint64_t>(std::stoull(parentWindow->AsString()));
                } catch (...) {
                    // leave unset on malformed input
                }
            } else if (parentWindow->IsNumber() && parentWindow->AsNumber() >= 0) {
                config.parentWindow = static_cast<uint64_t>(parentWindow->AsNumber());
            }
        }

        return config;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<BootConfig> LoadBootConfigFromFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return ParseBootConfig(buffer.str());
}

} // namespace ootmm
