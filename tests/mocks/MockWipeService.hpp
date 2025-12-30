/**
 * @file MockWipeService.hpp
 * @brief Google Mock implementation of IWipeService
 */

#pragma once

#include "services/IWipeService.hpp"
#include <gmock/gmock.h>
#include <thread>
#include <chrono>

class MockWipeService : public IWipeService {
public:
    MOCK_METHOD(bool, wipe_disk,
                (const std::string& disk_path, WipeAlgorithm algorithm,
                 ProgressCallback callback), (override));
    MOCK_METHOD(std::string, get_algorithm_name, (WipeAlgorithm algo), (override));
    MOCK_METHOD(std::string, get_algorithm_description, (WipeAlgorithm algo), (override));
    MOCK_METHOD(int, get_pass_count, (WipeAlgorithm algo), (override));
    MOCK_METHOD(bool, is_ssd_compatible, (WipeAlgorithm algo), (override));
    MOCK_METHOD(bool, cancel_current_operation, (), (override));

    // Helper: Create a nice mock with sensible defaults
    static std::shared_ptr<MockWipeService> CreateNiceMock() {
        auto mock = std::make_shared<testing::NiceMock<MockWipeService>>();

        ON_CALL(*mock, get_algorithm_name(testing::_))
            .WillByDefault(testing::Return("Test Algorithm"));
        ON_CALL(*mock, get_algorithm_description(testing::_))
            .WillByDefault(testing::Return("Test algorithm description"));
        ON_CALL(*mock, get_pass_count(testing::_))
            .WillByDefault(testing::Return(1));
        ON_CALL(*mock, is_ssd_compatible(testing::_))
            .WillByDefault(testing::Return(true));
        ON_CALL(*mock, wipe_disk(testing::_, testing::_, testing::_))
            .WillByDefault(testing::Return(true));
        ON_CALL(*mock, cancel_current_operation())
            .WillByDefault(testing::Return(true));

        return mock;
    }

    // Helper: Simulate successful wipe with progress callbacks
    static void SimulateSuccessfulWipe(ProgressCallback callback,
                                       int passes = 1,
                                       std::chrono::milliseconds delay = std::chrono::milliseconds{10}) {
        for (int pass = 1; pass <= passes; ++pass) {
            for (int pct = 0; pct <= 100; pct += 25) {
                if (callback) {
                    WipeProgress progress{
                        .bytes_written = static_cast<uint64_t>(pct * 10000),
                        .total_bytes = 1000000,
                        .current_pass = pass,
                        .total_passes = passes,
                        .percentage = static_cast<double>(pct),
                        .status = "Wiping...",
                        .is_complete = false,
                        .has_error = false,
                        .error_message = ""
                    };
                    callback(progress);
                }
                std::this_thread::sleep_for(delay);
            }
        }

        // Send completion
        if (callback) {
            WipeProgress complete{
                .bytes_written = 1000000,
                .total_bytes = 1000000,
                .current_pass = passes,
                .total_passes = passes,
                .percentage = 100.0,
                .status = "Complete",
                .is_complete = true,
                .has_error = false,
                .error_message = ""
            };
            callback(complete);
        }
    }

    // Helper: Simulate wipe failure
    static void SimulateFailedWipe(ProgressCallback callback,
                                   const std::string& error_message) {
        if (callback) {
            WipeProgress progress{
                .bytes_written = 250000,
                .total_bytes = 1000000,
                .current_pass = 1,
                .total_passes = 1,
                .percentage = 25.0,
                .status = "Error",
                .is_complete = true,
                .has_error = true,
                .error_message = error_message
            };
            callback(progress);
        }
    }
};
