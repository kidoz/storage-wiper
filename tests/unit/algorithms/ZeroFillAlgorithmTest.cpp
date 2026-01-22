/**
 * @file ZeroFillAlgorithmTest.cpp
 * @brief Unit tests for ZeroFillAlgorithm
 */

#include "algorithms/ZeroFillAlgorithm.hpp"

#include "fixtures/TestFixtures.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <thread>

class ZeroFillAlgorithmTest : public AlgorithmTestFixture {
protected:
    ZeroFillAlgorithm algorithm;

    // Create a pipe for testing write operations
    int pipe_fds[2] = {-1, -1};

    void SetUp() override {
        AlgorithmTestFixture::SetUp();
        ASSERT_EQ(pipe(pipe_fds), 0) << "Failed to create test pipe";
    }

    void TearDown() override {
        if (pipe_fds[0] >= 0)
            close(pipe_fds[0]);
        if (pipe_fds[1] >= 0)
            close(pipe_fds[1]);
        AlgorithmTestFixture::TearDown();
    }
};

// Test: algorithm metadata
TEST_F(ZeroFillAlgorithmTest, GetName_ReturnsZeroFill) {
    EXPECT_EQ(algorithm.get_name(), "Zero Fill");
}

TEST_F(ZeroFillAlgorithmTest, GetDescription_ReturnsNonEmpty) {
    EXPECT_FALSE(algorithm.get_description().empty());
}

TEST_F(ZeroFillAlgorithmTest, GetPassCount_ReturnsOne) {
    EXPECT_EQ(algorithm.get_pass_count(), 1);
}

TEST_F(ZeroFillAlgorithmTest, IsSsdCompatible_ReturnsTrue) {
    EXPECT_TRUE(algorithm.is_ssd_compatible());
}

// Test: zero-size input succeeds immediately
TEST_F(ZeroFillAlgorithmTest, Execute_ZeroSize_ReturnsTrue) {
    bool result = algorithm.execute(pipe_fds[1], 0, nullptr, cancel_flag);
    EXPECT_TRUE(result);
}

// Test: progress callback is called with correct values
TEST_F(ZeroFillAlgorithmTest, Execute_CallsProgressCallback) {
    constexpr uint64_t test_size = 4'096;

    // Start reader thread to consume pipe data
    std::thread reader([this, test_size]() {
        std::vector<uint8_t> buffer(test_size);
        size_t total_read = 0;
        while (total_read < test_size) {
            ssize_t n = read(pipe_fds[0], buffer.data() + total_read, test_size - total_read);
            if (n <= 0)
                break;
            total_read += n;
        }
    });

    auto callback = CreateCapturingCallback();
    bool result = algorithm.execute(pipe_fds[1], test_size, callback, cancel_flag);

    reader.join();

    EXPECT_TRUE(result);
    EXPECT_FALSE(captured_progress.empty());

    // Verify progress values
    for (const auto& progress : captured_progress) {
        EXPECT_EQ(progress.current_pass, 1);
        EXPECT_EQ(progress.total_passes, 1);
        EXPECT_LE(progress.bytes_written, test_size);
        EXPECT_GE(progress.percentage, 0.0);
        EXPECT_LE(progress.percentage, 100.0);
    }
}

// Test: null callback doesn't crash
TEST_F(ZeroFillAlgorithmTest, Execute_NullCallback_DoesNotCrash) {
    constexpr uint64_t test_size = 1'024;

    std::thread reader([this, test_size]() {
        std::vector<uint8_t> buffer(test_size);
        read(pipe_fds[0], buffer.data(), test_size);
    });

    bool result = algorithm.execute(pipe_fds[1], test_size, nullptr, cancel_flag);

    reader.join();

    EXPECT_TRUE(result);
}

// Test: cancellation stops execution
TEST_F(ZeroFillAlgorithmTest, Execute_CancellationStopsWriting) {
    // Small size - cancellation is immediate
    constexpr uint64_t test_size = 4'096;

    // Pre-set cancel flag
    cancel_flag.store(true);

    // Reader thread to consume any written data
    std::atomic<bool> reader_done{false};
    std::thread reader([this, &reader_done]() {
        std::vector<uint8_t> buffer(4'096);
        while (!reader_done.load()) {
            ssize_t n = read(pipe_fds[0], buffer.data(), buffer.size());
            if (n <= 0)
                break;
        }
    });

    bool result = algorithm.execute(pipe_fds[1], test_size, nullptr, cancel_flag);

    reader_done.store(true);
    close(pipe_fds[1]);  // Close write end to unblock reader
    pipe_fds[1] = -1;
    reader.join();

    EXPECT_FALSE(result);  // Should return false when cancelled
}

// Test: writes only zeros
TEST_F(ZeroFillAlgorithmTest, Execute_WritesOnlyZeros) {
    constexpr uint64_t test_size = 8'192;
    std::vector<uint8_t> read_buffer(test_size, 0xFF);  // Pre-fill with non-zeros

    // Reader thread captures data
    std::thread reader([this, &read_buffer, test_size]() {
        size_t total_read = 0;
        while (total_read < test_size) {
            ssize_t n = read(pipe_fds[0], read_buffer.data() + total_read, test_size - total_read);
            if (n <= 0)
                break;
            total_read += n;
        }
    });

    algorithm.execute(pipe_fds[1], test_size, nullptr, cancel_flag);
    reader.join();

    // Verify all bytes are zeros
    for (size_t i = 0; i < test_size; ++i) {
        EXPECT_EQ(read_buffer[i], 0) << "Non-zero byte at offset " << i;
    }
}

// Test: progress percentage reaches 100 on completion
TEST_F(ZeroFillAlgorithmTest, Execute_ProgressReaches100OnCompletion) {
    constexpr uint64_t test_size = 2'048;

    std::thread reader([this, test_size]() {
        std::vector<uint8_t> buffer(test_size);
        size_t total_read = 0;
        while (total_read < test_size) {
            ssize_t n = read(pipe_fds[0], buffer.data() + total_read, test_size - total_read);
            if (n <= 0)
                break;
            total_read += n;
        }
    });

    auto callback = CreateCapturingCallback();
    algorithm.execute(pipe_fds[1], test_size, callback, cancel_flag);

    reader.join();

    ASSERT_FALSE(captured_progress.empty());

    // Find the maximum percentage reported
    double max_percentage = 0.0;
    for (const auto& progress : captured_progress) {
        max_percentage = std::max(max_percentage, progress.percentage);
    }

    EXPECT_DOUBLE_EQ(max_percentage, 100.0);
}
