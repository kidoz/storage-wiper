/**
 * @file WipeCertificateTest.cpp
 * @brief Unit tests for wipe certificate generation and export
 */

#include "util/WipeCertificate.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using util::WipeCertificateData;

WipeCertificateData sample_data() {
    WipeCertificateData data{};
    data.device_path = "/dev/sda";
    data.model = "Samsung SSD 870 \"Extra\"";
    data.serial = "S6PXNZ0R123456";
    data.size_bytes = 500'107'862'016ULL;
    data.algorithm_name = "Hardware Secure Erase";
    data.nist_category = "NIST 800-88 Purge";
    data.total_passes = 1;
    data.started_at = "2026-09-04T10:00:00Z";
    data.completed_at = "2026-09-04T10:12:00Z";
    data.duration_seconds = 720;
    data.peak_speed_bytes_per_sec = 150 * 1'024 * 1'024;
    data.verification_enabled = false;
    data.verification_passed = false;
    data.success = true;
    data.tool_version = "1.4.3";
    return data;
}

/// RAII temporary directory (mkdtemp)
class TempDir {
public:
    TempDir() {
        std::string tmpl = std::string{std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp"} +
                           "/storage_wiper_cert_XXXXXX";
        std::vector<char> buffer{tmpl.begin(), tmpl.end()};
        buffer.push_back('\0');
        if (mkdtemp(buffer.data()) != nullptr) {
            path_ = buffer.data();
        }
    }

    ~TempDir() {
        if (!path_.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(path_, ec);
        }
    }

    const std::filesystem::path& path() const { return path_; }
    bool valid() const { return !path_.empty(); }

private:
    std::filesystem::path path_;
};

// ==========================================================================
// JSON output
// ==========================================================================

TEST(WipeCertificateJsonTest, ContainsKeyFields) {
    const auto json = util::wipe_certificate_json(sample_data());

    EXPECT_NE(json.find("\"type\": \"storage-wiper-certificate\""), std::string::npos);
    EXPECT_NE(json.find("\"path\": \"/dev/sda\""), std::string::npos);
    EXPECT_NE(json.find("\"serial\": \"S6PXNZ0R123456\""), std::string::npos);
    EXPECT_NE(json.find("\"algorithm\": \"Hardware Secure Erase\""), std::string::npos);
    EXPECT_NE(json.find("\"nist_category\": \"NIST 800-88 Purge\""), std::string::npos);
    EXPECT_NE(json.find("\"size_bytes\": 500107862016"), std::string::npos);
    EXPECT_NE(json.find("\"success\": true"), std::string::npos);
    EXPECT_NE(json.find("\"started_at\": \"2026-09-04T10:00:00Z\""), std::string::npos);
}

TEST(WipeCertificateJsonTest, EscapesQuotesAndBackslashes) {
    auto data = sample_data();
    data.model = "Weird \"Model\" \\ Oil";
    const auto json = util::wipe_certificate_json(data);

    EXPECT_NE(json.find("\\\"Model\\\""), std::string::npos);
    EXPECT_NE(json.find("\\\\ Oil"), std::string::npos);
}

// ==========================================================================
// Text output
// ==========================================================================

TEST(WipeCertificateTextTest, ContainsKeyFields) {
    const auto text = util::wipe_certificate_text(sample_data());

    EXPECT_NE(text.find("WIPE CERTIFICATE"), std::string::npos);
    EXPECT_NE(text.find("Status:               SUCCESS"), std::string::npos);
    EXPECT_NE(text.find("Device:               /dev/sda"), std::string::npos);
    EXPECT_NE(text.find("Serial:               S6PXNZ0R123456"), std::string::npos);
    EXPECT_NE(text.find("Sanitization:         NIST 800-88 Purge"), std::string::npos);
    EXPECT_NE(text.find("Verification:         not requested"), std::string::npos);
    EXPECT_NE(text.find("Duration:             12m 00s"), std::string::npos);
}

TEST(WipeCertificateTextTest, VerificationLabels) {
    auto data = sample_data();

    data.verification_enabled = true;
    data.verification_passed = true;
    EXPECT_NE(util::wipe_certificate_text(data).find("Verification:         passed"),
              std::string::npos);

    data.verification_passed = false;
    EXPECT_NE(util::wipe_certificate_text(data).find("Verification:         FAILED"),
              std::string::npos);
}

// ==========================================================================
// Timestamp helpers
// ==========================================================================

TEST(WipeCertificateTimeTest, Iso8601Utc_OfEpoch) {
    const auto time = std::chrono::system_clock::time_point{std::chrono::seconds{0}};
    EXPECT_EQ(util::iso8601_utc(time), "1970-01-01T00:00:00Z");
}

TEST(WipeCertificateTimeTest, CertificateTimestamp_IsFilenameSafe) {
    const auto time = std::chrono::system_clock::time_point{std::chrono::seconds{0}};
    EXPECT_EQ(util::certificate_timestamp(time), "19700101-000000");
}

// ==========================================================================
// Default directory
// ==========================================================================

TEST(WipeCertificateDefaultDirTest, HonorsXdgDataHome) {
    const char* old_xdg = std::getenv("XDG_DATA_HOME");
    const std::string saved = old_xdg != nullptr ? old_xdg : "";

    setenv("XDG_DATA_HOME", "/tmp/fake-xdg", 1);
    EXPECT_EQ(util::wipe_certificate_default_dir().string(),
              "/tmp/fake-xdg/storage-wiper/certificates");

    if (old_xdg != nullptr) {
        setenv("XDG_DATA_HOME", saved.c_str(), 1);
    } else {
        unsetenv("XDG_DATA_HOME");
    }
}

// ==========================================================================
// File writing
// ==========================================================================

TEST(WipeCertificateWriteTest, WritesJsonAndTxtToExplicitBase) {
    TempDir dir;
    ASSERT_TRUE(dir.valid());

    const auto result = util::write_certificate_to_base(dir.path() / "my-wipe", sample_data());
    ASSERT_TRUE(result.has_value()) << result.error();

    std::ifstream json_file{dir.path() / "my-wipe.json"};
    ASSERT_TRUE(json_file.good());
    std::stringstream json;
    json << json_file.rdbuf();
    EXPECT_NE(json.str().find("storage-wiper-certificate"), std::string::npos);

    std::ifstream text_file{dir.path() / "my-wipe.txt"};
    ASSERT_TRUE(text_file.good());
    std::stringstream text;
    text << text_file.rdbuf();
    EXPECT_NE(text.str().find("WIPE CERTIFICATE"), std::string::npos);
}

TEST(WipeCertificateWriteTest, AutoNamesContainDevice) {
    TempDir dir;
    ASSERT_TRUE(dir.valid());

    const auto result = util::write_wipe_certificate(dir.path() / "nested", sample_data());
    ASSERT_TRUE(result.has_value()) << result.error();

    EXPECT_TRUE(std::filesystem::exists(result->string() + ".json"));
    EXPECT_TRUE(std::filesystem::exists(result->string() + ".txt"));
    EXPECT_NE(result->filename().string().find("sda"), std::string::npos);
}

TEST(WipeCertificateWriteTest, CreatesMissingDirectories) {
    TempDir dir;
    ASSERT_TRUE(dir.valid());

    const auto result = util::write_wipe_certificate(dir.path() / "a" / "b" / "c", sample_data());
    EXPECT_TRUE(result.has_value()) << result.error();
}

}  // namespace
