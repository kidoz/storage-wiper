/**
 * @file GOSTAlgorithmTest.cpp
 * @brief Unit tests for GOSTAlgorithm
 */

#include "algorithms/GOSTAlgorithm.hpp"

#include "fixtures/TestFixtures.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstring>

class GOSTAlgorithmTest : public AlgorithmTestFixture {
protected:
    GOSTAlgorithm algorithm;
};

// Test: algorithm metadata
TEST_F(GOSTAlgorithmTest, GetName_ReturnsGOST) {
    EXPECT_EQ(algorithm.get_name(), "GOST R 50739-95");
}

TEST_F(GOSTAlgorithmTest, GetDescription_ReturnsNonEmpty) {
    EXPECT_FALSE(algorithm.get_description().empty());
    EXPECT_NE(algorithm.get_description().find("GOST"), std::string::npos);
}

TEST_F(GOSTAlgorithmTest, GetPassCount_ReturnsTwo) {
    EXPECT_EQ(algorithm.get_pass_count(), 2);
}

TEST_F(GOSTAlgorithmTest, IsSsdCompatible_ReturnsFalse) {
    EXPECT_FALSE(algorithm.is_ssd_compatible());
}

// Test: zero-size input succeeds immediately
TEST_F(GOSTAlgorithmTest, Execute_ZeroSize_ReturnsTrue) {
    TempTestFile temp_file;
    ASSERT_TRUE(temp_file.valid());
    bool result = algorithm.execute(temp_file.fd(), 0, nullptr, cancel_flag);
    EXPECT_TRUE(result);
}

// Test: progress callback is called with correct pass info
// Uses temp file because GOST algorithm requires seeking between passes
TEST_F(GOSTAlgorithmTest, Execute_CallsProgressCallbackWithPasses) {
    TempTestFile temp_file;
    ASSERT_TRUE(temp_file.valid());

    constexpr uint64_t test_size = 4'096;
    ASSERT_TRUE(temp_file.resize(test_size));
    ASSERT_TRUE(temp_file.seek_start());

    auto callback = CreateCapturingCallback();
    bool result = algorithm.execute(temp_file.fd(), test_size, callback, cancel_flag);

    EXPECT_TRUE(result);
    EXPECT_FALSE(captured_progress.empty());

    // Verify we see both passes
    bool saw_pass_1 = false, saw_pass_2 = false;
    for (const auto& progress : captured_progress) {
        EXPECT_EQ(progress.total_passes, 2);
        if (progress.current_pass == 1)
            saw_pass_1 = true;
        if (progress.current_pass == 2)
            saw_pass_2 = true;
    }
    EXPECT_TRUE(saw_pass_1);
    EXPECT_TRUE(saw_pass_2);
}

// Test: null callback doesn't crash
TEST_F(GOSTAlgorithmTest, Execute_NullCallback_DoesNotCrash) {
    TempTestFile temp_file;
    ASSERT_TRUE(temp_file.valid());

    constexpr uint64_t test_size = 1'024;
    ASSERT_TRUE(temp_file.resize(test_size));
    ASSERT_TRUE(temp_file.seek_start());

    bool result = algorithm.execute(temp_file.fd(), test_size, nullptr, cancel_flag);
    EXPECT_TRUE(result);
}

// Test: cancellation stops execution
TEST_F(GOSTAlgorithmTest, Execute_CancellationStopsWriting) {
    TempTestFile temp_file;
    ASSERT_TRUE(temp_file.valid());

    constexpr uint64_t test_size = 4'096;
    ASSERT_TRUE(temp_file.resize(test_size));

    cancel_flag.store(true);

    bool result = algorithm.execute(temp_file.fd(), test_size, nullptr, cancel_flag);
    EXPECT_FALSE(result);
}
