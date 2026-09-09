/**
 * @file WipeCertificate.hpp
 * @brief Wipe certificate generation and export
 *
 * Builds a machine-readable (JSON) and human-readable (text) certificate
 * describing a completed wipe operation: device identity, algorithm with its
 * NIST SP 800-88 sanitization category, timing, throughput, and verification
 * verdict. Certificates are the audit artifact for disposal workflows.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace util {

struct WipeCertificateData {
    std::string device_path;
    std::string model;
    std::string serial;
    uint64_t size_bytes = 0;
    std::string algorithm_name;
    std::string nist_category;  ///< e.g. "NIST 800-88 Purge"
    int total_passes = 0;

    std::string started_at;    ///< ISO 8601 UTC
    std::string completed_at;  ///< ISO 8601 UTC
    uint64_t duration_seconds = 0;
    uint64_t peak_speed_bytes_per_sec = 0;

    bool verification_enabled = false;
    bool verification_passed = false;
    bool is_partition = false;     ///< True when a single partition was wiped
    std::string parent_disk;       ///< Parent disk of the partition, empty otherwise
    uint64_t bad_block_count = 0;  ///< Sectors that could not be overwritten
    bool success = false;
    std::string tool_version;
};

/**
 * @brief Format a time point as an ISO 8601 UTC timestamp ("2026-09-04T12:34:56Z")
 */
[[nodiscard]] auto iso8601_utc(std::chrono::system_clock::time_point time) -> std::string;

/**
 * @brief Compact filesystem-safe timestamp ("20260904-123456") for file names
 */
[[nodiscard]] auto certificate_timestamp(std::chrono::system_clock::time_point time) -> std::string;

[[nodiscard]] auto wipe_certificate_json(const WipeCertificateData& data) -> std::string;

[[nodiscard]] auto wipe_certificate_text(const WipeCertificateData& data) -> std::string;

/**
 * @brief Default certificate directory
 *
 * $XDG_DATA_HOME/storage-wiper/certificates, falling back to
 * $HOME/.local/share/storage-wiper/certificates.
 */
[[nodiscard]] auto wipe_certificate_default_dir() -> std::filesystem::path;

/**
 * @brief Write the certificate as <base>.json and <base>.txt
 *
 * Parent directories are created as needed.
 *
 * @param base Base path without extension
 * @param data Certificate contents
 * @return The base path, or an error message
 */
[[nodiscard]] auto write_certificate_to_base(const std::filesystem::path& base,
                                             const WipeCertificateData& data)
    -> std::expected<std::filesystem::path, std::string>;

/**
 * @brief Write the certificate with an auto-generated file name
 *
 * The base file name is derived from the completion time and the device name.
 *
 * @param directory Target directory (created if missing)
 * @param data Certificate contents
 * @return Base path (without extension), or an error message
 */
[[nodiscard]] auto write_wipe_certificate(const std::filesystem::path& directory,
                                          const WipeCertificateData& data)
    -> std::expected<std::filesystem::path, std::string>;

}  // namespace util
