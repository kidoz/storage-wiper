/**
 * @file AppSettingsTest.cpp
 * @brief Unit tests for persisted wipe preset settings
 */

#include "util/AppSettings.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

/// RAII redirect of XDG_CONFIG_HOME into a fresh temp directory
class TempConfigHome {
public:
    TempConfigHome() {
        const char* tmp = std::getenv("TMPDIR");
        std::string tmpl =
            std::string{tmp != nullptr ? tmp : "/tmp"} + "/storage_wiper_settings_XXXXXX";
        std::vector<char> buffer{tmpl.begin(), tmpl.end()};
        buffer.push_back('\0');
        if (mkdtemp(buffer.data()) != nullptr) {
            dir_ = buffer.data();
        }
        const char* old = std::getenv("XDG_CONFIG_HOME");
        saved_ = old != nullptr ? old : "";
        had_saved_ = old != nullptr;
        setenv("XDG_CONFIG_HOME", dir_.c_str(), 1);
    }

    ~TempConfigHome() {
        if (had_saved_) {
            setenv("XDG_CONFIG_HOME", saved_.c_str(), 1);
        } else {
            unsetenv("XDG_CONFIG_HOME");
        }
        if (!dir_.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(dir_, ec);
        }
    }

    [[nodiscard]] auto config_file() const -> std::filesystem::path {
        return dir_ / "storage-wiper" / "settings.conf";
    }

private:
    std::filesystem::path dir_;
    std::string saved_;
    bool had_saved_ = false;
};

TEST(AppSettingsTest, Load_ReturnsDefaultsWhenFileMissing) {
    TempConfigHome home;
    const auto data = util::AppSettings::load();
    EXPECT_EQ(data.algorithm_id, 0);
    EXPECT_FALSE(data.verification_enabled);
}

TEST(AppSettingsTest, SaveThenLoad_RoundTrips) {
    TempConfigHome home;
    EXPECT_TRUE(util::AppSettings::save({.algorithm_id = 7, .verification_enabled = true}));

    const auto data = util::AppSettings::load();
    EXPECT_EQ(data.algorithm_id, 7);
    EXPECT_TRUE(data.verification_enabled);
}

TEST(AppSettingsTest, Load_ClampsOutOfRangeAlgorithmId) {
    TempConfigHome home;
    std::filesystem::create_directories(home.config_file().parent_path());
    std::ofstream{home.config_file()} << "algorithm=99\nverification=false\n";

    const auto data = util::AppSettings::load();
    EXPECT_EQ(data.algorithm_id, util::AppSettings::MAX_ALGORITHM_ID);
}

TEST(AppSettingsTest, Load_IgnoresGarbageLines) {
    TempConfigHome home;
    std::filesystem::create_directories(home.config_file().parent_path());
    std::ofstream{home.config_file()} << "this is not k=v\n\nverification=true\n";

    const auto data = util::AppSettings::load();
    EXPECT_EQ(data.algorithm_id, 0);
    EXPECT_TRUE(data.verification_enabled);
}

TEST(AppSettingsTest, FilePath_LivesUnderStorageWiperConfigDir) {
    TempConfigHome home;
    const auto path = util::AppSettings::file_path();
    EXPECT_NE(path.string().find("storage-wiper"), std::string::npos);
    EXPECT_EQ(path.filename().string(), "settings.conf");
}

}  // namespace
