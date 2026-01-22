/**
 * @file GutmannAlgorithmTest.cpp
 * @brief Unit tests for GutmannAlgorithm
 */

#include "algorithms/GutmannAlgorithm.hpp"

#include "fixtures/TestFixtures.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstring>

class GutmannAlgorithmTest : public AlgorithmTestFixture {
protected:
    GutmannAlgorithm algorithm;
};

// Test: algorithm metadata
TEST_F(GutmannAlgorithmTest, GetName_ReturnsGutmann) {
    EXPECT_EQ(algorithm.get_name(), "Gutmann");
}

TEST_F(GutmannAlgorithmTest, GetDescription_ReturnsNonEmpty) {
    EXPECT_FALSE(algorithm.get_description().empty());
    EXPECT_NE(algorithm.get_description().find("Gutmann"), std::string::npos);
}

TEST_F(GutmannAlgorithmTest, GetPassCount_ReturnsThirtyFive) {
    EXPECT_EQ(algorithm.get_pass_count(), 35);
}

TEST_F(GutmannAlgorithmTest, IsSsdCompatible_ReturnsFalse) {
    // The Gutmann method is specifically designed for older HDDs and is not SSD compatible
    EXPECT_FALSE(algorithm.is_ssd_compatible());
}

// Test: zero-size input succeeds immediately
TEST_F(GutmannAlgorithmTest, Execute_ZeroSize_ReturnsTrue) {
    TempTestFile temp_file;
    ASSERT_TRUE(temp_file.valid());
    bool result = algorithm.execute(temp_file.fd(), 0, nullptr, cancel_flag);
    EXPECT_TRUE(result);
}

// Test: progress callback reports correct total passes
// Uses temp file because Gutmann algorithm requires seeking between passes
TEST_F(GutmannAlgorithmTest, Execute_ReportsCorrectTotalPasses) {
    TempTestFile temp_file;
    ASSERT_TRUE(temp_file.valid());

    // Use small size for fast testing (35 passes!)
    constexpr uint64_t test_size = 512;
    ASSERT_TRUE(temp_file.resize(test_size));
    ASSERT_TRUE(temp_file.seek_start());

    auto callback = CreateCapturingCallback();
    bool result = algorithm.execute(temp_file.fd(), test_size, callback, cancel_flag);

    EXPECT_TRUE(result);
    EXPECT_FALSE(captured_progress.empty());

    // Verify total_passes is correctly reported as 35
    for (const auto& progress : captured_progress) {
        EXPECT_EQ(progress.total_passes, 35);
        EXPECT_GE(progress.current_pass, 1);
        EXPECT_LE(progress.current_pass, 35);
    }
}

// Test: null callback doesn't crash
TEST_F(GutmannAlgorithmTest, Execute_NullCallback_DoesNotCrash) {
    TempTestFile temp_file;
    ASSERT_TRUE(temp_file.valid());

    constexpr uint64_t test_size = 512;  // Small size for fast testing
    ASSERT_TRUE(temp_file.resize(test_size));
    ASSERT_TRUE(temp_file.seek_start());

    bool result = algorithm.execute(temp_file.fd(), test_size, nullptr, cancel_flag);
    EXPECT_TRUE(result);
}

// Test: cancellation stops execution
TEST_F(GutmannAlgorithmTest, Execute_CancellationStopsWriting) {
    TempTestFile temp_file;
    ASSERT_TRUE(temp_file.valid());

    constexpr uint64_t test_size = 4'096;
    ASSERT_TRUE(temp_file.resize(test_size));

    cancel_flag.store(true);

    bool result = algorithm.execute(temp_file.fd(), test_size, nullptr, cancel_flag);
    EXPECT_FALSE(result);
}
