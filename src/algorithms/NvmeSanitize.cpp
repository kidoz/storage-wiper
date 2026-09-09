/**
 * @file NvmeSanitize.cpp
 * @brief Implementation of pure NVMe sanitize decoding helpers
 */

#include "algorithms/NvmeSanitize.hpp"

#include <cstdint>

namespace nvme_sanitize {

namespace {

auto read_le16(const uint8_t* p) -> uint16_t {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

auto read_le32(const uint8_t* p) -> uint32_t {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

auto parse_capabilities(const uint8_t* identify_ctrl) -> Capabilities {
    const auto oacs = read_le16(identify_ctrl + ID_CTRL_OFFSET_OACS);
    const auto sanicap = read_le32(identify_ctrl + ID_CTRL_OFFSET_SANICAP);
    const uint8_t fna = identify_ctrl[ID_CTRL_OFFSET_FNA];

    Capabilities caps;
    // Only the three operation bits say whether Sanitize can actually be used;
    // the deallocation-behaviour bits can be set on their own.
    caps.sanitize_supported = (sanicap & SANICAP_OPERATION_MASK) != 0;
    caps.crypto_erase = (sanicap & SANICAP_CRYPTO_ERASE) != 0;
    caps.block_erase = (sanicap & SANICAP_BLOCK_ERASE) != 0;
    caps.overwrite = (sanicap & SANICAP_OVERWRITE) != 0;
    caps.format_nvm = (oacs & OACS_FORMAT_NVM) != 0;
    caps.format_crypto_erase = caps.format_nvm && (fna & FNA_CRYPTO_ERASE) != 0;
    return caps;
}

auto choose_erase_plan(const Capabilities& caps) -> ErasePlan {
    if (caps.sanitize_supported) {
        if (caps.crypto_erase) {
            return ErasePlan::SANITIZE_CRYPTO_ERASE;
        }
        if (caps.block_erase) {
            return ErasePlan::SANITIZE_BLOCK_ERASE;
        }
        if (caps.overwrite) {
            return ErasePlan::SANITIZE_OVERWRITE;
        }
    }
    if (caps.format_crypto_erase) {
        return ErasePlan::FORMAT_CRYPTO_ERASE;
    }
    return ErasePlan::UNSUPPORTED;
}

auto is_sanitize_plan(ErasePlan plan) -> bool {
    return plan == ErasePlan::SANITIZE_CRYPTO_ERASE || plan == ErasePlan::SANITIZE_BLOCK_ERASE ||
           plan == ErasePlan::SANITIZE_OVERWRITE;
}

auto is_success_status(SanitizeStatus status) -> bool {
    return status == SanitizeStatus::COMPLETED_SUCCESS ||
           status == SanitizeStatus::COMPLETED_SUCCESS_NO_DEALLOCATE;
}

auto sanitize_cdw10(ErasePlan plan) -> uint32_t {
    switch (plan) {
        case ErasePlan::SANITIZE_CRYPTO_ERASE:
            return SANACT_CRYPTO_ERASE;
        case ErasePlan::SANITIZE_BLOCK_ERASE:
            return SANACT_BLOCK_ERASE;
        case ErasePlan::SANITIZE_OVERWRITE:
            // The pass count belongs here in bits 7:4. Leaving it zero would
            // request the maximum of 16 passes.
            return SANACT_OVERWRITE |
                   ((OVERWRITE_PASSES & SANITIZE_CDW10_OWPASS_MASK) << SANITIZE_CDW10_OWPASS_SHIFT);
        case ErasePlan::FORMAT_CRYPTO_ERASE:
        case ErasePlan::UNSUPPORTED:
            return 0;
    }
    return 0;
}

auto format_cdw10(uint8_t flbas, uint32_t ses) -> uint32_t {
    // FLBAS carries the format index in bits 3:0 and, on controllers with more
    // than 16 formats, its upper two bits in 6:5. Both halves are copied back
    // into their CDW10 positions so the namespace keeps its current LBA size.
    const uint32_t lbaf = flbas & FORMAT_CDW10_LBAF_MASK;
    const uint32_t lbafu = (static_cast<uint32_t>(flbas) >> 5) & 0x3;

    return lbaf | (ses << FORMAT_CDW10_SES_SHIFT) | (lbafu << FORMAT_CDW10_LBAFU_SHIFT);
}

auto erase_plan_name(ErasePlan plan) -> std::string_view {
    switch (plan) {
        case ErasePlan::SANITIZE_CRYPTO_ERASE:
            return "NVMe Sanitize: cryptographic erase";
        case ErasePlan::SANITIZE_BLOCK_ERASE:
            return "NVMe Sanitize: block erase";
        case ErasePlan::SANITIZE_OVERWRITE:
            return "NVMe Sanitize: overwrite";
        case ErasePlan::FORMAT_CRYPTO_ERASE:
            return "NVMe Format: cryptographic erase";
        case ErasePlan::UNSUPPORTED:
            return "unsupported";
    }
    return "unsupported";
}

auto parse_log_page(const uint8_t* log_page) -> LogPage {
    const auto sprog = read_le16(log_page);
    const auto sstat = read_le16(log_page + 2);

    LogPage page;

    // SSTAT status is three bits wide. Masking it to two would fold the
    // "completed, no-deallocate not honoured" value onto "never sanitized",
    // turning a finished erase into an endless poll.
    switch (sstat & 0x7) {
        case 0:
            page.status = SanitizeStatus::NEVER_SANITIZED;
            break;
        case 1:
            page.status = SanitizeStatus::COMPLETED_SUCCESS;
            break;
        case 2:
            page.status = SanitizeStatus::IN_PROGRESS;
            break;
        case 3:
            page.status = SanitizeStatus::COMPLETED_FAILED;
            break;
        case 4:
            page.status = SanitizeStatus::COMPLETED_SUCCESS_NO_DEALLOCATE;
            break;
        default:
            page.status = SanitizeStatus::UNKNOWN;
            break;
    }

    // SPROG is a numerator over 65536, not a percentage.
    constexpr uint32_t SPROG_DENOMINATOR = 65'536;
    const auto percent = (static_cast<uint32_t>(sprog) * 100) / SPROG_DENOMINATOR;
    page.progress_percent = static_cast<uint8_t>(percent > 100 ? 100 : percent);
    return page;
}

auto controller_path_of(std::string_view namespace_path) -> std::optional<std::string> {
    constexpr std::string_view PREFIX = "/dev/nvme";
    if (!namespace_path.starts_with(PREFIX)) {
        return std::nullopt;
    }

    // The 'n' of "nvme0n1" after the controller index marks a namespace
    // device; its absence means the path is already the controller chardev.
    const auto pos = namespace_path.find('n', PREFIX.size());
    const auto digits = [](std::string_view value) {
        return !value.empty() && value.find_first_not_of("0123456789") == std::string_view::npos;
    };
    if (pos == std::string_view::npos ||
        !digits(namespace_path.substr(PREFIX.size(), pos - PREFIX.size())) ||
        !digits(namespace_path.substr(pos + 1))) {
        return std::nullopt;
    }

    return std::string{namespace_path.substr(0, pos)};
}

}  // namespace nvme_sanitize
