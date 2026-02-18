/**
 * @file TestFixtures.hpp
 * @brief Common test fixtures for storage-wiper tests
 */

#pragma once

#include "di/Container.hpp"
#include "mocks/MockDiskService.hpp"
#include "mocks/MockWipeAlgorithm.hpp"
#include "mocks/MockWipeService.hpp"

#include <glibmm.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <string>
#include <vector>

/**
 * @brief Base fixture providing DI container setup/teardown
 */
class ContainerTestFixture : public ::testing::Test {
protected:
    di::Container container;

    void SetUp() override { di::ServiceLocator::reset(); }

    void TearDown() override {
        container.clear();
        di::ServiceLocator::reset();
    }
};

/**
 * @brief Fixture with pre-configured mock services for ViewModel tests
 */
class ViewModelTestFixture : public ::testing::Test {
protected:
    std::shared_ptr<MockDiskService> mock_disk_service;
    std::shared_ptr<MockWipeService> mock_wipe_service;

    void SetUp() override {
        mock_disk_service = std::make_shared<testing::NiceMock<MockDiskService>>();
        mock_wipe_service = std::make_shared<testing::NiceMock<MockWipeService>>();

        SetupDefaultExpectations();
    }

    virtual void SetupDefaultExpectations() {
        // Default: empty disk list
        ON_CALL(*mock_disk_service, get_available_disks(testing::_))
            .WillByDefault(testing::Invoke(
                [](auto callback) { callback(std::vector<DiskInfo>{}); }));

        // Default: validation passes
        ON_CALL(*mock_disk_service, validate_device_path(testing::_))
            .WillByDefault(testing::Return(std::expected<void, util::Error>{}));

        // Default: disk is writable
        ON_CALL(*mock_disk_service, is_disk_writable(testing::_))
            .WillByDefault(testing::Return(true));

        // Default algorithm info
        ON_CALL(*mock_wipe_service, get_algorithm_name(testing::_))
            .WillByDefault(testing::Return("Test Algorithm"));
        ON_CALL(*mock_wipe_service, get_algorithm_description(testing::_))
            .WillByDefault(testing::Return("Test description"));
        ON_CALL(*mock_wipe_service, get_pass_count(testing::_)).WillByDefault(testing::Return(1));
        ON_CALL(*mock_wipe_service, is_ssd_compatible(testing::_))
            .WillByDefault(testing::Return(true));
    }

    void PumpMainLoop() {
        // Dispatch all pending idle handlers
        auto context = Glib::MainContext::get_default();
        while (context->pending()) {
            context->iteration(false);
        }
    }

    void TearDown() override {
        mock_disk_service.reset();
        mock_wipe_service.reset();
    }
};

/**
 * @brief Fixture for algorithm tests with progress capture
 */
class AlgorithmTestFixture : public ::testing::Test {
protected:
    std::atomic<bool> cancel_flag{false};
    std::vector<WipeProgress> captured_progress;

    ProgressCallback CreateCapturingCallback() {
        return [this](const WipeProgress& progress) {
            captured_progress.push_back(progress);
        };
    }

    void SetUp() override {
        cancel_flag.store(false);
        captured_progress.clear();
    }
};

/**
 * @brief Helper for testing threaded operations with timeouts
 */
class ThreadingTestHelper {
public:
    template <typename Callable>
    static bool WaitFor(Callable&& callable,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds{5'000}) {
        auto future = std::async(std::launch::async, std::forward<Callable>(callable));
        return future.wait_for(timeout) == std::future_status::ready;
    }

    template <typename Predicate>
    static bool WaitUntil(Predicate&& predicate,
                          std::chrono::milliseconds timeout = std::chrono::milliseconds{5'000},
                          std::chrono::milliseconds poll_interval = std::chrono::milliseconds{10}) {
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < timeout) {
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(poll_interval);
        }
        return false;
    }
};

/**
 * @brief RAII helper for temporary test files (for algorithms requiring seek)
 */
class TempTestFile {
public:
    TempTestFile() {
        char templ[] = "/tmp/storage_wiper_test_XXXXXX";
        fd_ = mkstemp(templ);
        if (fd_ >= 0) {
            path_ = templ;
        }
    }

    ~TempTestFile() {
        if (fd_ >= 0) {
            close(fd_);
        }
        if (!path_.empty()) {
            unlink(path_.c_str());
        }
    }

    // Non-copyable
    TempTestFile(const TempTestFile&) = delete;
    TempTestFile& operator=(const TempTestFile&) = delete;

    int fd() const { return fd_; }
    const std::string& path() const { return path_; }
    bool valid() const { return fd_ >= 0; }

    // Truncate/extend to specific size
    bool resize(size_t size) { return ftruncate(fd_, static_cast<off_t>(size)) == 0; }

    // Seek to beginning
    bool seek_start() { return lseek(fd_, 0, SEEK_SET) == 0; }

private:
    int fd_ = -1;
    std::string path_;
};

/**
 * @brief RAII helper for temporary test buffers
 */
class TestMemoryBuffer {
public:
    explicit TestMemoryBuffer(size_t size) : buffer_(size, 0) {}

    uint8_t* data() { return buffer_.data(); }
    const uint8_t* data() const { return buffer_.data(); }
    size_t size() const { return buffer_.size(); }

    // Verify pattern written at offset
    bool VerifyPattern(size_t offset, uint8_t pattern, size_t length) const {
        if (offset + length > buffer_.size())
            return false;
        for (size_t i = 0; i < length; ++i) {
            if (buffer_[offset + i] != pattern)
                return false;
        }
        return true;
    }

    // Check if buffer contains only zeros
    bool IsAllZeros() const {
        return std::all_of(buffer_.begin(), buffer_.end(), [](uint8_t b) { return b == 0; });
    }

    // Fill with pattern for testing reads
    void Fill(uint8_t pattern) { std::fill(buffer_.begin(), buffer_.end(), pattern); }

private:
    std::vector<uint8_t> buffer_;
};
