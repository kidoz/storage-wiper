/**
 * @file MockDiskService.hpp
 * @brief Google Mock implementation of IDiskService
 */

#pragma once

#include "services/IDiskService.hpp"

#include <gmock/gmock.h>

class MockDiskService : public IDiskService {
public:
    MOCK_METHOD(std::vector<DiskInfo>, get_available_disks, (), (override));
    MOCK_METHOD((std::expected<void, util::Error>), unmount_disk, (const std::string& path),
                (override));
    MOCK_METHOD(bool, is_disk_writable, (const std::string& path), (override));
    MOCK_METHOD((std::expected<uint64_t, util::Error>), get_disk_size, (const std::string& path),
                (override));
    MOCK_METHOD((std::expected<void, util::Error>), validate_device_path, (const std::string& path),
                (override));

    // Helper: Create a nice mock with sensible defaults
    static std::shared_ptr<MockDiskService> CreateNiceMock() {
        auto mock = std::make_shared<testing::NiceMock<MockDiskService>>();

        // Default: validation passes
        ON_CALL(*mock, validate_device_path(testing::_))
            .WillByDefault(testing::Return(std::expected<void, util::Error>{}));

        // Default: disk is writable
        ON_CALL(*mock, is_disk_writable(testing::_)).WillByDefault(testing::Return(true));

        // Default: empty disk list
        ON_CALL(*mock, get_available_disks())
            .WillByDefault(testing::Return(std::vector<DiskInfo>{}));

        return mock;
    }

    // Helper: Create test disk data
    static DiskInfo CreateTestDisk(const std::string& path = "/dev/sda",
                                   uint64_t size = 1'024ULL * 1'024 * 1'024, bool mounted = false) {
        return DiskInfo{.path = path,
                        .model = "Test Disk",
                        .serial = "TEST123",
                        .size_bytes = size,
                        .is_removable = true,
                        .is_ssd = false,
                        .filesystem = mounted ? "ext4" : "",
                        .is_mounted = mounted,
                        .mount_point = mounted ? "/mnt/test" : "",
                        .is_lvm_pv = false};
    }
};
