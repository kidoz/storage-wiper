/**
 * @file MockWipeAlgorithm.hpp
 * @brief Google Mock implementation of IWipeAlgorithm
 */

#pragma once

#include "algorithms/IWipeAlgorithm.hpp"
#include <gmock/gmock.h>

class MockWipeAlgorithm : public IWipeAlgorithm {
public:
    MOCK_METHOD(bool, execute,
                (int fd, uint64_t size, ProgressCallback callback,
                 const std::atomic<bool>& cancel_flag), (override));
    MOCK_METHOD(bool, execute_on_device,
                (const std::string& device_path, uint64_t size, ProgressCallback callback,
                 const std::atomic<bool>& cancel_flag), (override));
    MOCK_METHOD(bool, requires_device_access, (), (const, override));
    MOCK_METHOD(std::string, get_name, (), (const, override));
    MOCK_METHOD(std::string, get_description, (), (const, override));
    MOCK_METHOD(int, get_pass_count, (), (const, override));
    MOCK_METHOD(bool, is_ssd_compatible, (), (const, override));

    // Factory for pre-configured mock
    static std::shared_ptr<MockWipeAlgorithm> CreateDefault(
            const std::string& name = "TestAlgorithm",
            int passes = 1,
            bool ssd_compatible = true) {
        auto mock = std::make_shared<testing::NiceMock<MockWipeAlgorithm>>();

        ON_CALL(*mock, get_name())
            .WillByDefault(testing::Return(name));
        ON_CALL(*mock, get_description())
            .WillByDefault(testing::Return("Test algorithm description"));
        ON_CALL(*mock, get_pass_count())
            .WillByDefault(testing::Return(passes));
        ON_CALL(*mock, is_ssd_compatible())
            .WillByDefault(testing::Return(ssd_compatible));
        ON_CALL(*mock, execute(testing::_, testing::_, testing::_, testing::_))
            .WillByDefault(testing::Return(true));
        ON_CALL(*mock, execute_on_device(testing::_, testing::_, testing::_, testing::_))
            .WillByDefault(testing::Return(true));
        ON_CALL(*mock, requires_device_access())
            .WillByDefault(testing::Return(false));

        return mock;
    }

    // Helper to simulate progress during execute
    static bool ExecuteWithProgress(ProgressCallback callback,
                                    uint64_t total_bytes,
                                    int passes,
                                    const std::atomic<bool>& cancel_flag) {
        for (int pass = 1; pass <= passes && !cancel_flag.load(); ++pass) {
            uint64_t written = 0;
            while (written < total_bytes && !cancel_flag.load()) {
                written += std::min(uint64_t{1024 * 1024}, total_bytes - written);
                if (callback) {
                    WipeProgress progress{
                        .bytes_written = written,
                        .total_bytes = total_bytes,
                        .current_pass = pass,
                        .total_passes = passes,
                        .percentage = (static_cast<double>(written) / static_cast<double>(total_bytes)) * 100.0,
                        .status = "Writing...",
                        .is_complete = false,
                        .has_error = false,
                        .error_message = ""
                    };
                    callback(progress);
                }
            }
        }
        return !cancel_flag.load();
    }
};
