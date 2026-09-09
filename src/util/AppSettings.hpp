/**
 * @file AppSettings.hpp
 * @brief Persisted application settings (last-used wipe preset)
 *
 * Stores the most recently used algorithm and verification choice so the GUI
 * can restore them on the next start. Kept deliberately tiny: the "preset"
 * is the previous session's configuration, not a named profile store.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace util {

struct AppSettingsData {
    int algorithm_id = 0;            ///< WipeAlgorithm as int
    bool verification_enabled = false;
};

class AppSettings {
public:
    /// Config file location: $XDG_CONFIG_HOME (or ~/.config)/storage-wiper/settings.conf
    [[nodiscard]] static auto file_path() -> std::filesystem::path;

    /// Load settings; returns defaults when the file is missing or invalid
    [[nodiscard]] static auto load() -> AppSettingsData;

    /// Persist settings; creates the directory as needed. Best-effort: returns
    /// false on I/O errors without throwing.
    static auto save(const AppSettingsData& data) -> bool;

    /// Highest valid algorithm id (WipeAlgorithm::ATA_SECURE_ERASE)
    static constexpr int MAX_ALGORITHM_ID = 7;
};

}  // namespace util
