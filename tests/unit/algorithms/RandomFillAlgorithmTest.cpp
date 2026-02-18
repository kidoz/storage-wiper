/**
 * @file RandomFillAlgorithmTest.cpp
 * @brief Unit tests for RandomFillAlgorithm
 */

#include "algorithms/RandomFillAlgorithm.hpp"

#include "fixtures/TestFixtures.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <set>
#include <thread>

class RandomFillAlgorithmTest : public AlgorithmTestFixture {
protected:
    RandomFillAlgorithm algorithm;

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
TEST_F(RandomFillAlgorithmTest, GetName_ReturnsRandomData) {
    EXPECT_EQ(algorithm.get_name(), "Random Data");
}

TEST_F(RandomFillAlgorithmTest, GetDescription_ReturnsNonEmpty) {
    EXPECT_FALSE(algorithm.get_description().empty());
}

TEST_F(RandomFillAlgorithmTest, GetPassCount_ReturnsOne) {
    EXPECT_EQ(algorithm.get_pass_count(), 1);
}

TEST_F(RandomFillAlgorithmTest, IsSsdCompatible_ReturnsTrue) {
    EXPECT_TRUE(algorithm.is_ssd_compatible());
}

// Test: zero-size input succeeds immediately
TEST_F(RandomFillAlgorithmTest, Execute_ZeroSize_ReturnsTrue) {
    bool result = algorithm.execute(pipe_fds[1], 0, nullptr, cancel_flag);
    EXPECT_TRUE(result);
}

// Test: writes non-zero data (random)
TEST_F(RandomFillAlgorithmTest, Execute_WritesRandomData) {
    constexpr uint64_t test_size = 8'192;
    std::vector<uint8_t> read_buffer(test_size, 0);

    // Reader thread captures data
    std::thread reader([this, &read_buffer]() {
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

    // Count unique byte values - random data should have variety
    std::set<uint8_t> unique_bytes(read_buffer.begin(), read_buffer.end());

    // Random data of 8KB should have many different byte values
    // (statistically, should have all 256 with high probability)
    EXPECT_GT(unique_bytes.size(), 200u) << "Random data should contain many different byte values";
}

// Test: progress callback is called
TEST_F(RandomFillAlgorithmTest, Execute_CallsProgressCallback) {
    constexpr uint64_t test_size = 4'096;

    std::thread reader([this]() {
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

    for (const auto& progress : captured_progress) {
        EXPECT_EQ(progress.current_pass, 1);
        EXPECT_EQ(progress.total_passes, 1);
    }
}

// Test: cancellation works
TEST_F(RandomFillAlgorithmTest, Execute_CancellationStopsWriting) {
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

    EXPECT_FALSE(result);
}

// Test: two executions produce different data
TEST_F(RandomFillAlgorithmTest, Execute_ProducesDifferentDataEachTime) {
    constexpr uint64_t test_size = 1'024;

    // First execution
    std::vector<uint8_t> buffer1(test_size);
    {
        int fds1[2];
        ASSERT_EQ(pipe(fds1), 0);

        std::thread reader([&fds1, &buffer1]() {
            size_t total = 0;
            while (total < test_size) {
                ssize_t n = read(fds1[0], buffer1.data() + total, test_size - total);
                if (n <= 0)
                    break;
                total += n;
            }
        });

        RandomFillAlgorithm algo1;
        algo1.execute(fds1[1], test_size, nullptr, cancel_flag);
        reader.join();

        close(fds1[0]);
        close(fds1[1]);
    }

    // Second execution
    std::vector<uint8_t> buffer2(test_size);
    {
        int fds2[2];
        ASSERT_EQ(pipe(fds2), 0);

        std::thread reader([&fds2, &buffer2]() {
            size_t total = 0;
            while (total < test_size) {
                ssize_t n = read(fds2[0], buffer2.data() + total, test_size - total);
                if (n <= 0)
                    break;
                total += n;
            }
        });

        RandomFillAlgorithm algo2;
        algo2.execute(fds2[1], test_size, nullptr, cancel_flag);
        reader.join();

        close(fds2[0]);
        close(fds2[1]);
    }

    // Buffers should be different (extremely unlikely to be same with random data)
    EXPECT_NE(buffer1, buffer2) << "Two random fills should produce different data";
}
