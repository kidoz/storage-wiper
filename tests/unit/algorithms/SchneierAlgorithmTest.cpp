/**
 * @file SchneierAlgorithmTest.cpp
 * @brief Unit tests for SchneierAlgorithm
 */

#include "algorithms/SchneierAlgorithm.hpp"

#include "fixtures/TestFixtures.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstring>

class SchneierAlgorithmTest : public AlgorithmTestFixture {
protected:
    SchneierAlgorithm algorithm;
};

// Test: algorithm metadata
TEST_F(SchneierAlgorithmTest, GetName_ReturnsSchneier) {
    EXPECT_EQ(algorithm.get_name(), "Schneier Method");
}

TEST_F(SchneierAlgorithmTest, GetDescription_ReturnsNonEmpty) {
    EXPECT_FALSE(algorithm.get_description().empty());
    EXPECT_NE(algorithm.get_description().find("Schneier"), std::string::npos);
}

TEST_F(SchneierAlgorithmTest, GetPassCount_ReturnsSeven) {
    EXPECT_EQ(algorithm.get_pass_count(), 7);
}

TEST_F(SchneierAlgorithmTest, IsSsdCompatible_ReturnsFalse) {
    EXPECT_FALSE(algorithm.is_ssd_compatible());
}

// Test: zero-size input succeeds immediately
TEST_F(SchneierAlgorithmTest, Execute_ZeroSize_ReturnsTrue) {
    TempTestFile temp_file;
    ASSERT_TRUE(temp_file.valid());
    bool result = algorithm.execute(temp_file.fd(), 0, nullptr, cancel_flag);
    EXPECT_TRUE(result);
}

// Test: progress callback is called with correct pass info
// Uses temp file because Schneier algorithm requires seeking between passes
TEST_F(SchneierAlgorithmTest, Execute_CallsProgressCallbackWithPasses) {
    TempTestFile temp_file;
    ASSERT_TRUE(temp_file.valid());

    constexpr uint64_t test_size = 2'048;
    ASSERT_TRUE(temp_file.resize(test_size));
    ASSERT_TRUE(temp_file.seek_start());

    auto callback = CreateCapturingCallback();
    bool result = algorithm.execute(temp_file.fd(), test_size, callback, cancel_flag);

    EXPECT_TRUE(result);
    EXPECT_FALSE(captured_progress.empty());

    // Verify total_passes is correctly reported
    for (const auto& progress : captured_progress) {
        EXPECT_EQ(progress.total_passes, 7);
        EXPECT_GE(progress.current_pass, 1);
        EXPECT_LE(progress.current_pass, 7);
    }
}

// Test: null callback doesn't crash
TEST_F(SchneierAlgorithmTest, Execute_NullCallback_DoesNotCrash) {
    TempTestFile temp_file;
    ASSERT_TRUE(temp_file.valid());

    constexpr uint64_t test_size = 1'024;
    ASSERT_TRUE(temp_file.resize(test_size));
    ASSERT_TRUE(temp_file.seek_start());

    bool result = algorithm.execute(temp_file.fd(), test_size, nullptr, cancel_flag);
    EXPECT_TRUE(result);
}

// Test: cancellation stops execution
TEST_F(SchneierAlgorithmTest, Execute_CancellationStopsWriting) {
    TempTestFile temp_file;
    ASSERT_TRUE(temp_file.valid());

    constexpr uint64_t test_size = 4'096;
    ASSERT_TRUE(temp_file.resize(test_size));

    cancel_flag.store(true);

    bool result = algorithm.execute(temp_file.fd(), test_size, nullptr, cancel_flag);
    EXPECT_FALSE(result);
}
