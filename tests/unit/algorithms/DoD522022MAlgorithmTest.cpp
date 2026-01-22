/**
 * @file DoD522022MAlgorithmTest.cpp
 * @brief Unit tests for DoD522022MAlgorithm
 */

#include "algorithms/DoD522022MAlgorithm.hpp"

#include "fixtures/TestFixtures.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <thread>

class DoD522022MAlgorithmTest : public AlgorithmTestFixture {
protected:
    DoD522022MAlgorithm algorithm;
};

// Test: algorithm metadata
TEST_F(DoD522022MAlgorithmTest, GetName_ReturnsDoD) {
    EXPECT_EQ(algorithm.get_name(), "DoD 5220.22-M");
}

TEST_F(DoD522022MAlgorithmTest, GetDescription_ReturnsNonEmpty) {
    EXPECT_FALSE(algorithm.get_description().empty());
    // Description is "US Department of Defense 3-pass standard"
    EXPECT_NE(algorithm.get_description().find("Defense"), std::string::npos);
}

TEST_F(DoD522022MAlgorithmTest, GetPassCount_ReturnsThree) {
    EXPECT_EQ(algorithm.get_pass_count(), 3);
}

TEST_F(DoD522022MAlgorithmTest, IsSsdCompatible_ReturnsFalse) {
    // Multi-pass algorithms are not SSD compatible
    EXPECT_FALSE(algorithm.is_ssd_compatible());
}

// Test: zero-size input succeeds immediately
TEST_F(DoD522022MAlgorithmTest, Execute_ZeroSize_ReturnsTrue) {
    TempTestFile temp_file;
    ASSERT_TRUE(temp_file.valid());
    bool result = algorithm.execute(temp_file.fd(), 0, nullptr, cancel_flag);
    EXPECT_TRUE(result);
}

// Test: progress callback is called with correct pass info
// Uses temp file because DoD algorithm requires seeking between passes
TEST_F(DoD522022MAlgorithmTest, Execute_CallsProgressCallbackWithPasses) {
    TempTestFile temp_file;
    ASSERT_TRUE(temp_file.valid());

    constexpr uint64_t test_size = 4'096;
    ASSERT_TRUE(temp_file.resize(test_size));
    ASSERT_TRUE(temp_file.seek_start());

    auto callback = CreateCapturingCallback();
    bool result = algorithm.execute(temp_file.fd(), test_size, callback, cancel_flag);

    EXPECT_TRUE(result);
    EXPECT_FALSE(captured_progress.empty());

    // Verify we see all 3 passes
    bool saw_pass_1 = false, saw_pass_2 = false, saw_pass_3 = false;
    for (const auto& progress : captured_progress) {
        EXPECT_EQ(progress.total_passes, 3);
        if (progress.current_pass == 1)
            saw_pass_1 = true;
        if (progress.current_pass == 2)
            saw_pass_2 = true;
        if (progress.current_pass == 3)
            saw_pass_3 = true;
    }
    EXPECT_TRUE(saw_pass_1);
    EXPECT_TRUE(saw_pass_2);
    EXPECT_TRUE(saw_pass_3);
}

// Test: null callback doesn't crash
TEST_F(DoD522022MAlgorithmTest, Execute_NullCallback_DoesNotCrash) {
    TempTestFile temp_file;
    ASSERT_TRUE(temp_file.valid());

    constexpr uint64_t test_size = 1'024;
    ASSERT_TRUE(temp_file.resize(test_size));
    ASSERT_TRUE(temp_file.seek_start());

    bool result = algorithm.execute(temp_file.fd(), test_size, nullptr, cancel_flag);
    EXPECT_TRUE(result);
}

// Test: cancellation stops execution
TEST_F(DoD522022MAlgorithmTest, Execute_CancellationStopsWriting) {
    TempTestFile temp_file;
    ASSERT_TRUE(temp_file.valid());

    constexpr uint64_t test_size = 4'096;
    ASSERT_TRUE(temp_file.resize(test_size));

    cancel_flag.store(true);

    bool result = algorithm.execute(temp_file.fd(), test_size, nullptr, cancel_flag);
    EXPECT_FALSE(result);
}
