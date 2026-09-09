/**
 * @file NvmeSanitizeTest.cpp
 * @brief Unit tests for the pure NVMe sanitize decoding helpers
 *
 * These verify the byte-level decoding of Identify Controller capability
 * fields, erase-plan selection, Sanitize Status log page parsing, and the
 * controller path derivation - all without touching a real drive.
 */

#include "algorithms/NvmeSanitize.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

using namespace nvme_sanitize;

struct IdentifyFields {
    uint16_t oacs = 0;
    uint32_t sanicap = 0;
    uint8_t fna = 0;
};

std::array<uint8_t, 4'096> make_identify_controller(const IdentifyFields& fields) {
    std::array<uint8_t, 4'096> data{};

    data[ID_CTRL_OFFSET_OACS] = static_cast<uint8_t>(fields.oacs & 0xFF);
    data[ID_CTRL_OFFSET_OACS + 1] = static_cast<uint8_t>((fields.oacs >> 8) & 0xFF);

    data[ID_CTRL_OFFSET_SANICAP] = static_cast<uint8_t>(fields.sanicap & 0xFF);
    data[ID_CTRL_OFFSET_SANICAP + 1] = static_cast<uint8_t>((fields.sanicap >> 8) & 0xFF);
    data[ID_CTRL_OFFSET_SANICAP + 2] = static_cast<uint8_t>((fields.sanicap >> 16) & 0xFF);
    data[ID_CTRL_OFFSET_SANICAP + 3] = static_cast<uint8_t>((fields.sanicap >> 24) & 0xFF);

    data[ID_CTRL_OFFSET_FNA] = fields.fna;
    return data;
}

std::array<uint8_t, 512> make_sanitize_log(uint16_t sprog, uint16_t sstat) {
    std::array<uint8_t, 512> log{};
    log[0] = static_cast<uint8_t>(sprog & 0xFF);
    log[1] = static_cast<uint8_t>((sprog >> 8) & 0xFF);
    log[2] = static_cast<uint8_t>(sstat & 0xFF);
    log[3] = static_cast<uint8_t>((sstat >> 8) & 0xFF);
    return log;
}

// ==========================================================================
// parse_capabilities
// ==========================================================================

TEST(NvmeSanitizeTest, ParseCapabilities_AllSanitizeActions) {
    const auto identify = make_identify_controller(
        {.oacs = 0,
         .sanicap = SANICAP_OVERWRITE | SANICAP_BLOCK_ERASE | SANICAP_CRYPTO_ERASE,
         .fna = 0});
    const auto caps = parse_capabilities(identify.data());

    EXPECT_TRUE(caps.sanitize_supported);
    EXPECT_TRUE(caps.crypto_erase);
    EXPECT_TRUE(caps.block_erase);
    EXPECT_TRUE(caps.overwrite);
    EXPECT_FALSE(caps.format_nvm);
    EXPECT_FALSE(caps.format_crypto_erase);
}

// The SANICAP bit positions come from the NVMe base specification: CES is
// bit 0, BES bit 1, OWS bit 2. These use literal values on purpose, so a
// mistake in the header constants cannot cancel itself out here.
TEST(NvmeSanitizeTest, ParseCapabilities_CryptoEraseIsBitZero) {
    const auto identify = make_identify_controller({.oacs = 0, .sanicap = 0x1, .fna = 0});
    const auto caps = parse_capabilities(identify.data());

    EXPECT_TRUE(caps.sanitize_supported);
    EXPECT_TRUE(caps.crypto_erase);
    EXPECT_FALSE(caps.block_erase);
    EXPECT_FALSE(caps.overwrite);
}

TEST(NvmeSanitizeTest, ParseCapabilities_BlockEraseIsBitOne) {
    const auto identify = make_identify_controller({.oacs = 0, .sanicap = 0x2, .fna = 0});
    const auto caps = parse_capabilities(identify.data());

    EXPECT_FALSE(caps.crypto_erase);
    EXPECT_TRUE(caps.block_erase);
    EXPECT_FALSE(caps.overwrite);
}

TEST(NvmeSanitizeTest, ParseCapabilities_OverwriteIsBitTwo) {
    const auto identify = make_identify_controller({.oacs = 0, .sanicap = 0x4, .fna = 0});
    const auto caps = parse_capabilities(identify.data());

    EXPECT_FALSE(caps.crypto_erase);
    EXPECT_FALSE(caps.block_erase);
    EXPECT_TRUE(caps.overwrite);
}

TEST(NvmeSanitizeTest, ParseCapabilities_CryptoOnlyDriveChoosesCryptoErase) {
    // Regression: a drive advertising only crypto erase was decoded as
    // overwrite-only and sent an action it rejects.
    const auto identify = make_identify_controller({.oacs = 0, .sanicap = 0x1, .fna = 0});
    const auto caps = parse_capabilities(identify.data());

    EXPECT_EQ(choose_erase_plan(caps), ErasePlan::SANITIZE_CRYPTO_ERASE);
}

TEST(NvmeSanitizeTest, ParseCapabilities_DeallocationBitsAloneAreNotSanitizeSupport) {
    // NDI (bit 29) and NODMMAS (bits 31:30) describe deallocation behaviour,
    // not which sanitize operations exist.
    const auto identify = make_identify_controller({.oacs = 0, .sanicap = 0xE000'0000, .fna = 0});
    const auto caps = parse_capabilities(identify.data());

    EXPECT_FALSE(caps.sanitize_supported);
    EXPECT_EQ(choose_erase_plan(caps), ErasePlan::UNSUPPORTED);
}

TEST(NvmeSanitizeTest, ParseCapabilities_ZeroSanicapMeansNoSanitize) {
    const auto identify =
        make_identify_controller({.oacs = OACS_FORMAT_NVM, .sanicap = 0, .fna = FNA_CRYPTO_ERASE});
    const auto caps = parse_capabilities(identify.data());

    EXPECT_FALSE(caps.sanitize_supported);
    EXPECT_TRUE(caps.format_nvm);
    EXPECT_TRUE(caps.format_crypto_erase);
}

// ==========================================================================
// choose_erase_plan
// ==========================================================================

TEST(NvmeSanitizeTest, ChoosePlan_PrefersCryptoErase) {
    const auto plan = choose_erase_plan({.sanitize_supported = true,
                                         .crypto_erase = true,
                                         .block_erase = true,
                                         .overwrite = true,
                                         .format_nvm = true,
                                         .format_crypto_erase = true});
    EXPECT_EQ(plan, ErasePlan::SANITIZE_CRYPTO_ERASE);
}

TEST(NvmeSanitizeTest, ChoosePlan_FallsBackToBlockErase) {
    const auto plan = choose_erase_plan({.sanitize_supported = true,
                                         .crypto_erase = false,
                                         .block_erase = true,
                                         .overwrite = true,
                                         .format_nvm = true,
                                         .format_crypto_erase = true});
    EXPECT_EQ(plan, ErasePlan::SANITIZE_BLOCK_ERASE);
}

TEST(NvmeSanitizeTest, ChoosePlan_FallsBackToOverwrite) {
    const auto plan = choose_erase_plan({.sanitize_supported = true,
                                         .crypto_erase = false,
                                         .block_erase = false,
                                         .overwrite = true,
                                         .format_nvm = true,
                                         .format_crypto_erase = true});
    EXPECT_EQ(plan, ErasePlan::SANITIZE_OVERWRITE);
}

TEST(NvmeSanitizeTest, ChoosePlan_FormatCryptoWhenSanitizeAbsent) {
    const auto plan = choose_erase_plan({.sanitize_supported = false,
                                         .crypto_erase = false,
                                         .block_erase = false,
                                         .overwrite = false,
                                         .format_nvm = true,
                                         .format_crypto_erase = true});
    EXPECT_EQ(plan, ErasePlan::FORMAT_CRYPTO_ERASE);
}

TEST(NvmeSanitizeTest, ChoosePlan_FormatWithoutCryptoIsNotUsed) {
    const auto plan = choose_erase_plan({.sanitize_supported = false,
                                         .crypto_erase = false,
                                         .block_erase = false,
                                         .overwrite = false,
                                         .format_nvm = true,
                                         .format_crypto_erase = false});
    EXPECT_EQ(plan, ErasePlan::UNSUPPORTED);
}

TEST(NvmeSanitizeTest, ChoosePlan_UnsupportedWithoutAnyCapability) {
    const auto plan = choose_erase_plan({});
    EXPECT_EQ(plan, ErasePlan::UNSUPPORTED);
}

// ==========================================================================
// sanitize_cdw10 / is_sanitize_plan / erase_plan_name
// ==========================================================================

TEST(NvmeSanitizeTest, SanitizeCdw10_MatchesPlanActions) {
    // SANACT occupies bits 2:0
    EXPECT_EQ(sanitize_cdw10(ErasePlan::SANITIZE_CRYPTO_ERASE), 4u);
    EXPECT_EQ(sanitize_cdw10(ErasePlan::SANITIZE_BLOCK_ERASE), 2u);
    EXPECT_EQ(sanitize_cdw10(ErasePlan::FORMAT_CRYPTO_ERASE), 0u);
    EXPECT_EQ(sanitize_cdw10(ErasePlan::UNSUPPORTED), 0u);
}

TEST(NvmeSanitizeTest, SanitizeCdw10_OverwriteCarriesExplicitPassCount) {
    // OWPASS is bits 7:4 of CDW10, and a value of zero means 16 passes, so one
    // pass must be encoded as 1 << 4 alongside SANACT = 3.
    const auto cdw10 = sanitize_cdw10(ErasePlan::SANITIZE_OVERWRITE);

    EXPECT_EQ(cdw10 & 0x7, 3u) << "SANACT must select the overwrite action";
    EXPECT_EQ((cdw10 >> 4) & 0xF, 1u) << "a zero pass count would request 16 passes";
    EXPECT_EQ(cdw10, 0x13u);
}

// ==========================================================================
// format_cdw10
// ==========================================================================

TEST(NvmeSanitizeTest, FormatCdw10_PlacesSecureEraseSettingAtBitNine) {
    // SES is bits 11:9. Writing the value 2 into the low bits instead would
    // request LBA format 2 with no secure erase at all.
    const auto cdw10 = format_cdw10(0, FORMAT_SES_CRYPTO_ERASE);

    EXPECT_EQ((cdw10 >> 9) & 0x7, 2u);
    EXPECT_EQ(cdw10, 0x400u);
}

TEST(NvmeSanitizeTest, FormatCdw10_PreservesCurrentLbaFormat) {
    // FLBAS bits 3:0 name the format in use; the erase must not change it.
    const auto cdw10 = format_cdw10(0x3, FORMAT_SES_CRYPTO_ERASE);

    EXPECT_EQ(cdw10 & 0xF, 3u);
    EXPECT_EQ((cdw10 >> 9) & 0x7, 2u);
}

TEST(NvmeSanitizeTest, FormatCdw10_CarriesUpperLbaFormatBits) {
    // FLBAS bits 6:5 hold the upper format index bits on controllers with more
    // than 16 formats; CDW10 expects them at bits 13:12.
    const auto cdw10 = format_cdw10(0x62, FORMAT_SES_CRYPTO_ERASE);

    EXPECT_EQ(cdw10 & 0xF, 2u);
    EXPECT_EQ((cdw10 >> 12) & 0x3, 3u);
}

TEST(NvmeSanitizeTest, FormatCdw10_UserDataEraseSetting) {
    EXPECT_EQ((format_cdw10(0, FORMAT_SES_USER_DATA_ERASE) >> 9) & 0x7, 1u);
}

TEST(NvmeSanitizeTest, IsSanitizePlan_DistinguishesSanitizeFromFormat) {
    EXPECT_TRUE(is_sanitize_plan(ErasePlan::SANITIZE_CRYPTO_ERASE));
    EXPECT_TRUE(is_sanitize_plan(ErasePlan::SANITIZE_BLOCK_ERASE));
    EXPECT_TRUE(is_sanitize_plan(ErasePlan::SANITIZE_OVERWRITE));
    EXPECT_FALSE(is_sanitize_plan(ErasePlan::FORMAT_CRYPTO_ERASE));
    EXPECT_FALSE(is_sanitize_plan(ErasePlan::UNSUPPORTED));
}

TEST(NvmeSanitizeTest, ErasePlanNames_AreNonEmpty) {
    for (const auto plan :
         {ErasePlan::SANITIZE_CRYPTO_ERASE, ErasePlan::SANITIZE_BLOCK_ERASE,
          ErasePlan::SANITIZE_OVERWRITE, ErasePlan::FORMAT_CRYPTO_ERASE, ErasePlan::UNSUPPORTED}) {
        EXPECT_FALSE(erase_plan_name(plan).empty());
    }
}

// ==========================================================================
// parse_log_page
// ==========================================================================

TEST(NvmeSanitizeTest, ParseLogPage_InProgressWithPercent) {
    // SPROG is a numerator over 65536, so half way is 32768, not 50.
    const auto log = make_sanitize_log(32'768, 0x0002);  // SSTAT = 2: in progress
    const auto page = parse_log_page(log.data());

    EXPECT_EQ(page.status, SanitizeStatus::IN_PROGRESS);
    EXPECT_EQ(page.progress_percent, 50);
}

TEST(NvmeSanitizeTest, ParseLogPage_SmallProgressIsNotFullyComplete) {
    // Regression: treating SPROG as a percentage made any real progress read
    // as 100% within seconds, which also pinned the ETA at zero.
    const auto log = make_sanitize_log(200, 0x0002);
    const auto page = parse_log_page(log.data());

    EXPECT_EQ(page.progress_percent, 0);
}

TEST(NvmeSanitizeTest, ParseLogPage_QuarterProgress) {
    const auto log = make_sanitize_log(16'384, 0x0002);
    const auto page = parse_log_page(log.data());

    EXPECT_EQ(page.progress_percent, 25);
}

TEST(NvmeSanitizeTest, ParseLogPage_CompletedSuccess) {
    const auto log = make_sanitize_log(65'535, 0x0001);
    const auto page = parse_log_page(log.data());

    EXPECT_EQ(page.status, SanitizeStatus::COMPLETED_SUCCESS);
    EXPECT_TRUE(is_success_status(page.status));
}

TEST(NvmeSanitizeTest, ParseLogPage_CompletedSuccessWithoutDeallocate) {
    // SSTAT value 4 is a real terminal success. Masking the field to two bits
    // folded it onto "never sanitized" and polled until the 24-hour timeout.
    const auto log = make_sanitize_log(65'535, 0x0004);
    const auto page = parse_log_page(log.data());

    EXPECT_EQ(page.status, SanitizeStatus::COMPLETED_SUCCESS_NO_DEALLOCATE);
    EXPECT_TRUE(is_success_status(page.status));
}

TEST(NvmeSanitizeTest, ParseLogPage_ReservedStatusIsUnknown) {
    for (const uint16_t sstat : {uint16_t{5}, uint16_t{6}, uint16_t{7}}) {
        const auto log = make_sanitize_log(0, sstat);
        EXPECT_EQ(parse_log_page(log.data()).status, SanitizeStatus::UNKNOWN);
    }
}

TEST(NvmeSanitizeTest, ParseLogPage_IgnoresBitsAboveTheStatusField) {
    // Completed-passes and global-erased flags live above bit 2.
    const auto log = make_sanitize_log(0, 0xFF01);
    EXPECT_EQ(parse_log_page(log.data()).status, SanitizeStatus::COMPLETED_SUCCESS);
}

TEST(NvmeSanitizeTest, IsSuccessStatus_OnlyForCompletedStates) {
    EXPECT_FALSE(is_success_status(SanitizeStatus::NEVER_SANITIZED));
    EXPECT_FALSE(is_success_status(SanitizeStatus::IN_PROGRESS));
    EXPECT_FALSE(is_success_status(SanitizeStatus::COMPLETED_FAILED));
    EXPECT_FALSE(is_success_status(SanitizeStatus::UNKNOWN));
    EXPECT_TRUE(is_success_status(SanitizeStatus::COMPLETED_SUCCESS));
    EXPECT_TRUE(is_success_status(SanitizeStatus::COMPLETED_SUCCESS_NO_DEALLOCATE));
}

TEST(NvmeSanitizeTest, ParseLogPage_CompletedFailed) {
    const auto log = make_sanitize_log(0, 0x0003);
    const auto page = parse_log_page(log.data());

    EXPECT_EQ(page.status, SanitizeStatus::COMPLETED_FAILED);
}

TEST(NvmeSanitizeTest, ParseLogPage_NeverSanitized) {
    const auto log = make_sanitize_log(0, 0x0000);
    const auto page = parse_log_page(log.data());

    EXPECT_EQ(page.status, SanitizeStatus::NEVER_SANITIZED);
}

TEST(NvmeSanitizeTest, ParseLogPage_ProgressNeverExceedsHundred) {
    const auto log = make_sanitize_log(0xFFFF, 0x0002);
    const auto page = parse_log_page(log.data());

    EXPECT_LE(page.progress_percent, 100);
    EXPECT_GE(page.progress_percent, 99);
}

// ==========================================================================
// controller_path_of
// ==========================================================================

TEST(NvmeSanitizeTest, ControllerPathOf_NamespaceDevice) {
    EXPECT_EQ(controller_path_of("/dev/nvme0n1"), std::optional<std::string>{"/dev/nvme0"});
    EXPECT_EQ(controller_path_of("/dev/nvme12n2"), std::optional<std::string>{"/dev/nvme12"});
}

TEST(NvmeSanitizeTest, ControllerPathOf_ControllerDeviceReturnsNullopt) {
    EXPECT_EQ(controller_path_of("/dev/nvme0"), std::nullopt);
    EXPECT_EQ(controller_path_of("/dev/nvme1"), std::nullopt);
}

TEST(NvmeSanitizeTest, ControllerPathOf_NonNvmeReturnsNullopt) {
    EXPECT_EQ(controller_path_of("/dev/sda"), std::nullopt);
    EXPECT_EQ(controller_path_of("/dev/mmcblk0"), std::nullopt);
}

}  // namespace

TEST(NvmeSanitizeSafetyTest, RejectsPartitionAndMalformedNamespacePaths) {
    for (const auto* path :
         {"/dev/nvme0n1p1", "/dev/nvme0n", "/dev/nvmen1", "/dev/nvme0n1/../../sda"}) {
        EXPECT_EQ(nvme_sanitize::controller_path_of(path), std::nullopt);
    }
}
