/**
 * @file NvmeSanitize.hpp
 * @brief Pure NVMe sanitize helpers: capability decoding, erase-plan
 *        selection, and sanitize status log parsing
 *
 * The ioctl plumbing lives in ATASecureEraseAlgorithm.cpp. Everything in this
 * header is side-effect-free byte decoding so it can be unit tested without
 * a drive attached.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace nvme_sanitize {

// Byte offsets inside the Identify Controller data structure (NVMe 1.3+,
// little-endian fields).
constexpr size_t ID_CTRL_OFFSET_OACS = 256;     ///< Optional Admin Command Support (u16)
constexpr size_t ID_CTRL_OFFSET_SANICAP = 328;  ///< Sanitize Capabilities (u32)
constexpr size_t ID_CTRL_OFFSET_FNA = 524;      ///< Format NVM Attributes (u8)

/// Byte offset of FLBAS (Formatted LBA Size) inside Identify Namespace data
constexpr size_t ID_NS_OFFSET_FLBAS = 26;

// OACS bit 1: Format NVM command supported
constexpr uint16_t OACS_FORMAT_NVM = 0x0002;

// SANICAP operation bits, NVMe base spec figure "Sanitize Capabilities".
// CES is bit 0 and OWS is bit 2; the remaining bits (NDI, NODMMAS) describe
// deallocation behaviour and say nothing about which operations exist, so only
// these three decide whether the Sanitize command is usable.
constexpr uint32_t SANICAP_CRYPTO_ERASE = 1U << 0;  ///< CES
constexpr uint32_t SANICAP_BLOCK_ERASE = 1U << 1;   ///< BES
constexpr uint32_t SANICAP_OVERWRITE = 1U << 2;     ///< OWS
constexpr uint32_t SANICAP_OPERATION_MASK =
    SANICAP_CRYPTO_ERASE | SANICAP_BLOCK_ERASE | SANICAP_OVERWRITE;

// FNA bit 2: cryptographic erase supported as a Format NVM secure-erase setting
constexpr uint8_t FNA_CRYPTO_ERASE = 1U << 2;

// Sanitize command CDW10 SANACT (bits 2:0) action values
constexpr uint32_t SANACT_EXIT_FAILURE_MODE = 1;
constexpr uint32_t SANACT_BLOCK_ERASE = 2;
constexpr uint32_t SANACT_OVERWRITE = 3;
constexpr uint32_t SANACT_CRYPTO_ERASE = 4;

// Sanitize command CDW10 field positions. OWPASS is in CDW10, not CDW11:
// CDW11 is OVRPAT, the 32-bit overwrite pattern.
constexpr uint32_t SANITIZE_CDW10_OWPASS_SHIFT = 4;  ///< Overwrite pass count, bits 7:4
constexpr uint32_t SANITIZE_CDW10_OWPASS_MASK = 0xF;

/// Overwrite passes to request. A pass count of zero means 16 passes, so this
/// must be encoded explicitly; one pass satisfies NIST SP 800-88 Purge on
/// modern drives and more only adds wear.
constexpr uint32_t OVERWRITE_PASSES = 1;

// Format NVM command CDW10 field positions. SES sits at bits 11:9; bits 3:0
// select the LBA format, so writing the SES value into the low bits silently
// requests a reformat with no secure erase at all.
constexpr uint32_t FORMAT_CDW10_LBAF_MASK = 0xF;   ///< LBA format index, bits 3:0
constexpr uint32_t FORMAT_CDW10_SES_SHIFT = 9;     ///< Secure Erase Settings, bits 11:9
constexpr uint32_t FORMAT_CDW10_LBAFU_SHIFT = 12;  ///< LBA format index upper bits, 13:12
constexpr uint32_t FORMAT_SES_USER_DATA_ERASE = 1;
constexpr uint32_t FORMAT_SES_CRYPTO_ERASE = 2;

// Sanitize Status log page (Get Log Page, LID 0x81)
constexpr uint8_t SANITIZE_LOG_ID = 0x81;
constexpr size_t SANITIZE_LOG_SIZE = 512;

/// Sanitize Status (SSTAT bits 2:0) of the most recent sanitize operation
enum class SanitizeStatus : uint8_t {
    NEVER_SANITIZED = 0,    ///< No sanitize since manufacture
    COMPLETED_SUCCESS = 1,  ///< Most recent sanitize finished successfully
    IN_PROGRESS = 2,        ///< A sanitize operation is currently running
    COMPLETED_FAILED = 3,   ///< Most recent sanitize ended with a failure
    /// Finished successfully, but the no-deallocate request was not honoured.
    /// Still a completed erase, and drives do report it.
    COMPLETED_SUCCESS_NO_DEALLOCATE = 4,
    UNKNOWN = 7,  ///< Reserved value; treated as "status not settled yet"
};

/// True for the two status values that mean the media was erased
[[nodiscard]] auto is_success_status(SanitizeStatus status) -> bool;

/// Firmware erase method selected for a controller
enum class ErasePlan : uint8_t {
    SANITIZE_CRYPTO_ERASE,  ///< Sanitize command, Crypto Erase action
    SANITIZE_BLOCK_ERASE,   ///< Sanitize command, Block Erase action
    SANITIZE_OVERWRITE,     ///< Sanitize command, Overwrite action
    FORMAT_CRYPTO_ERASE,    ///< Format NVM with cryptographic erase setting
    UNSUPPORTED,            ///< No firmware erase capability
};

/// Decoded Sanitize Status log page
struct LogPage {
    SanitizeStatus status = SanitizeStatus::NEVER_SANITIZED;
    /// Completion percentage derived from SPROG, which the spec defines as a
    /// numerator over 65536 rather than a percentage
    uint8_t progress_percent = 0;
};

/// Firmware erase capabilities decoded from Identify Controller data
struct Capabilities {
    bool sanitize_supported = false;   ///< SANICAP non-zero (Sanitize command available)
    bool crypto_erase = false;         ///< Sanitize Crypto Erase supported
    bool block_erase = false;          ///< Sanitize Block Erase supported
    bool overwrite = false;            ///< Sanitize Overwrite supported
    bool format_nvm = false;           ///< Format NVM command supported
    bool format_crypto_erase = false;  ///< Format NVM with cryptographic erase supported
};

/**
 * @brief What the controller can do, decoded from a 4096-byte Identify
 *        Controller structure
 */
[[nodiscard]] auto parse_capabilities(const uint8_t* identify_ctrl) -> Capabilities;

/**
 * @brief Pick the strongest available firmware erase method
 *
 * Preference order follows NIST SP 800-88 "Purge" strength: Sanitize Crypto
 * Erase (instant, no wear) beats Sanitize Block Erase, which beats Sanitize
 * Overwrite; Format NVM with cryptographic erase is the fallback for
 * controllers without the Sanitize command.
 */
[[nodiscard]] auto choose_erase_plan(const Capabilities& caps) -> ErasePlan;

/**
 * @brief True when the plan runs through the Sanitize command (asynchronous,
 *        with progress polling). False for Format NVM (blocking) or
 *        UNSUPPORTED.
 */
[[nodiscard]] auto is_sanitize_plan(ErasePlan plan) -> bool;

/**
 * @brief Sanitize command CDW10 for the plan: the SANACT action, plus the
 *        overwrite pass count for the overwrite action
 * @return 0 when the plan does not use the Sanitize command
 */
[[nodiscard]] auto sanitize_cdw10(ErasePlan plan) -> uint32_t;

/**
 * @brief Format NVM command CDW10
 * @param flbas FLBAS byte from Identify Namespace, naming the format the
 *        namespace currently uses. It is preserved so the erase does not
 *        also change the LBA size.
 * @param ses Secure Erase Settings value (FORMAT_SES_*)
 */
[[nodiscard]] auto format_cdw10(uint8_t flbas, uint32_t ses) -> uint32_t;

/// Human-readable description of the selected erase method
[[nodiscard]] auto erase_plan_name(ErasePlan plan) -> std::string_view;

/// Decode a 512-byte Sanitize Status log page
[[nodiscard]] auto parse_log_page(const uint8_t* log_page) -> LogPage;

/**
 * @brief Derive the controller character device of an NVMe namespace
 *
 * /dev/nvme0n1 -> /dev/nvme0. Returns nullopt for paths that are not NVMe
 * namespace block devices (including the controller character devices
 * themselves, which have no namespace suffix).
 */
[[nodiscard]] auto controller_path_of(std::string_view namespace_path)
    -> std::optional<std::string>;

}  // namespace nvme_sanitize
