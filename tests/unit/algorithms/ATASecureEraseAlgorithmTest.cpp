/**
 * @file ATASecureEraseAlgorithmTest.cpp
 * @brief Unit tests for ATASecureEraseAlgorithm
 *
 * Note: ATASecureErase requires device-level access and cannot be fully
 * tested with pipes. These tests verify the algorithm's metadata and
 * interface behavior.
 */

#include "algorithms/ATASecureEraseAlgorithm.hpp"

#include "fixtures/TestFixtures.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

class ATASecureEraseAlgorithmTest : public AlgorithmTestFixture {
protected:
    ATASecureEraseAlgorithm algorithm;
};

// Test: algorithm metadata
TEST_F(ATASecureEraseAlgorithmTest, GetName_ReturnsATASecureErase) {
    EXPECT_EQ(algorithm.get_name(), "ATA Secure Erase");
}

TEST_F(ATASecureEraseAlgorithmTest, GetDescription_ReturnsNonEmpty) {
    EXPECT_FALSE(algorithm.get_description().empty());
    EXPECT_NE(algorithm.get_description().find("SSD"), std::string::npos);
}

TEST_F(ATASecureEraseAlgorithmTest, GetPassCount_ReturnsOne) {
    // ATA Secure Erase is a single hardware command
    EXPECT_EQ(algorithm.get_pass_count(), 1);
}

TEST_F(ATASecureEraseAlgorithmTest, IsSsdCompatible_ReturnsTrue) {
    // ATA Secure Erase is specifically designed for SSDs
    EXPECT_TRUE(algorithm.is_ssd_compatible());
}

TEST_F(ATASecureEraseAlgorithmTest, RequiresDeviceAccess_ReturnsTrue) {
    // ATA Secure Erase requires device-level access, not file descriptor
    EXPECT_TRUE(algorithm.requires_device_access());
}

// Test: execute() method returns false (use execute_on_device instead)
TEST_F(ATASecureEraseAlgorithmTest, Execute_ReturnsFalse) {
    // execute() always returns false for ATA Secure Erase
    // because it requires device-level access via execute_on_device()
    bool result = algorithm.execute(-1, 0, nullptr, cancel_flag);
    EXPECT_FALSE(result);
}

// Test: execute_on_device with invalid path fails gracefully
TEST_F(ATASecureEraseAlgorithmTest, ExecuteOnDevice_InvalidPath_ReturnsFalse) {
    // Using a non-existent device path should fail
    bool result = algorithm.execute_on_device("/dev/nonexistent_device_12345", 1'024 * 1'024,
                                              nullptr, cancel_flag);
    EXPECT_FALSE(result);
}

// Test: execute_on_device respects cancellation flag
TEST_F(ATASecureEraseAlgorithmTest, ExecuteOnDevice_Cancelled_ReturnsFalse) {
    cancel_flag.store(true);

    // Even with an invalid path, cancellation should be respected
    bool result = algorithm.execute_on_device("/dev/nonexistent_device_12345", 1'024 * 1'024,
                                              nullptr, cancel_flag);
    EXPECT_FALSE(result);
}

// Test: progress callback with error message
TEST_F(ATASecureEraseAlgorithmTest, ExecuteOnDevice_InvalidPath_ReportsError) {
    auto callback = CreateCapturingCallback();

    algorithm.execute_on_device("/dev/nonexistent_device_12345", 1'024 * 1'024, callback,
                                cancel_flag);

    // Should have received at least one progress update with error
    EXPECT_FALSE(captured_progress.empty());

    bool found_error = false;
    for (const auto& progress : captured_progress) {
        if (progress.has_error) {
            found_error = true;
            EXPECT_FALSE(progress.error_message.empty());
        }
    }
    EXPECT_TRUE(found_error);
}
