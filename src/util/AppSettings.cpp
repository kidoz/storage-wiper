/**
 * @file AppSettings.cpp
 * @brief Persisted application settings implementation
 */

#include "util/AppSettings.hpp"

#include <algorithm>
#include <fstream>
#include <format>
#include <map>
#include <optional>
#include <sstream>

namespace util {

namespace {

// Simple "key=value" lines; unknown keys and values are ignored so older or
// newer files never produce garbage settings.
auto parse_line(const std::string& line)
    -> std::optional<std::pair<std::string, std::string>> {
    const auto pos = line.find('=');
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    return std::make_pair(line.substr(0, pos), line.substr(pos + 1));
}

}  // namespace

auto AppSettings::file_path() -> std::filesystem::path {
    namespace fs = std::filesystem;

    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
        return fs::path{xdg} / "storage-wiper" / "settings.conf";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return fs::path{home} / ".config" / "storage-wiper" / "settings.conf";
    }
    return {};
}

auto AppSettings::load() -> AppSettingsData {
    AppSettingsData data{};

    const auto path = file_path();
    if (path.empty()) {
        return data;
    }

    std::ifstream file{path};
    if (!file) {
        return data;
    }

    std::string line;
    while (std::getline(file, line)) {
        const auto entry = parse_line(line);
        if (!entry) {
            continue;
        }
        const auto& [key, value] = *entry;
        if (key == "algorithm") {
            try {
                const int id = std::stoi(value);
                data.algorithm_id = std::clamp(id, 0, MAX_ALGORITHM_ID);
            } catch (const std::exception&) {
                // keep default
            }
        } else if (key == "verification") {
            data.verification_enabled = value == "true" || value == "1";
        }
    }

    return data;
}

auto AppSettings::save(const AppSettingsData& data) -> bool {
    const auto path = file_path();
    if (path.empty()) {
        return false;
    }

    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return false;
        }
    }

    std::ofstream file{path, std::ios::trunc};
    if (!file) {
        return false;
    }
    file << std::format("algorithm={}\n", data.algorithm_id);
    file << std::format("verification={}\n", data.verification_enabled ? "true" : "false");
    return file.good();
}

}  // namespace util
