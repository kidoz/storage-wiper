/**
 * @file VerificationHelperTest.cpp
 * @brief Unit tests for post-wipe verification utilities.
 */

#include "algorithms/VerificationHelper.hpp"

#include "fixtures/TestFixtures.hpp"
#include "util/SecureRandom.hpp"

#include <gtest/gtest.h>

#include <unistd.h>

#include <span>
#include <vector>

namespace {

constexpr size_t TEST_SIZE = 256 * 1'024;  // 256 KiB

class VerificationHelperTest : public AlgorithmTestFixture {
protected:
    TempTestFile file;

    void WriteBytes(std::span<const uint8_t> bytes) {
        ASSERT_TRUE(file.valid());
        ASSERT_EQ(write(file.fd(), bytes.data(), bytes.size()), static_cast<ssize_t>(bytes.size()));
        ASSERT_TRUE(file.seek_start());
    }
};

TEST_F(VerificationHelperTest, VerifyZeros_AllZeros_Passes) {
    std::vector<uint8_t> zeros(TEST_SIZE, 0x00);
    WriteBytes(zeros);

    EXPECT_TRUE(verification::verify_zeros(file.fd(), TEST_SIZE, nullptr, cancel_flag));
}

TEST_F(VerificationHelperTest, VerifyZeros_SingleNonZeroByte_Fails) {
    std::vector<uint8_t> data(TEST_SIZE, 0x00);
    data[TEST_SIZE / 2] = 0x01;
    WriteBytes(data);

    EXPECT_FALSE(verification::verify_zeros(file.fd(), TEST_SIZE, nullptr, cancel_flag));
}

TEST_F(VerificationHelperTest, VerifyPattern_MatchingPattern_Passes) {
    std::vector<uint8_t> data(TEST_SIZE, 0xAA);
    WriteBytes(data);

    EXPECT_TRUE(verification::verify_pattern(file.fd(), TEST_SIZE, 0xAA, nullptr, cancel_flag));
}

TEST_F(VerificationHelperTest, VerifyPattern_WrongPattern_Fails) {
    std::vector<uint8_t> data(TEST_SIZE, 0xAA);
    WriteBytes(data);

    EXPECT_FALSE(verification::verify_pattern(file.fd(), TEST_SIZE, 0x55, nullptr, cancel_flag));
}

TEST_F(VerificationHelperTest, VerifyRandom_CsprngData_Passes) {
    std::vector<uint8_t> data(TEST_SIZE);
    util::secure_random_fill(std::as_writable_bytes(std::span{data}));
    WriteBytes(data);

    EXPECT_TRUE(verification::verify_random(file.fd(), TEST_SIZE, nullptr, cancel_flag));
}

TEST_F(VerificationHelperTest, VerifyRandom_AllZeros_Fails) {
    std::vector<uint8_t> zeros(TEST_SIZE, 0x00);
    WriteBytes(zeros);

    EXPECT_FALSE(verification::verify_random(file.fd(), TEST_SIZE, nullptr, cancel_flag));
}

TEST_F(VerificationHelperTest, VerifyRandom_RepeatingText_Fails) {
    // Structured, non-uniform data must fail the chi-squared test.
    std::vector<uint8_t> data(TEST_SIZE);
    const std::string text = "residual filesystem data ";
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>(text[i % text.size()]);
    }
    WriteBytes(data);

    EXPECT_FALSE(verification::verify_random(file.fd(), TEST_SIZE, nullptr, cancel_flag));
}

TEST_F(VerificationHelperTest, VerifyRandom_ZeroSize_Passes) {
    EXPECT_TRUE(verification::verify_random(file.fd(), 0, nullptr, cancel_flag));
}

TEST_F(VerificationHelperTest, VerifyRandom_Cancelled_Fails) {
    std::vector<uint8_t> data(TEST_SIZE);
    util::secure_random_fill(std::as_writable_bytes(std::span{data}));
    WriteBytes(data);

    cancel_flag.store(true);
    EXPECT_FALSE(verification::verify_random(file.fd(), TEST_SIZE, nullptr, cancel_flag));
}

TEST_F(VerificationHelperTest, VerifyRandom_EmitsProgress) {
    std::vector<uint8_t> data(TEST_SIZE);
    util::secure_random_fill(std::as_writable_bytes(std::span{data}));
    WriteBytes(data);

    EXPECT_TRUE(
        verification::verify_random(file.fd(), TEST_SIZE, CreateCapturingCallback(), cancel_flag));
    ASSERT_FALSE(captured_progress.empty());
    EXPECT_TRUE(captured_progress.back().verification_in_progress);
    EXPECT_DOUBLE_EQ(captured_progress.back().verification_percentage, 100.0);
}

}  // namespace
