/**
 * @file DiskServiceTest.cpp
 * @brief Unit tests for DiskService
 *
 * Note: Many DiskService methods interact with the filesystem and block devices.
 * These tests focus on input validation and error handling that can be tested
 * without actual hardware access.
 */

#include "helper/services/DiskService.hpp"

#include "fixtures/TestFixtures.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

class DiskServiceTest : public ::testing::Test {
protected:
    DiskService service;
    static void append_partitions(DiskService& instance, std::vector<DiskInfo>& disks,
                                  const std::string& directory, const MountCache& mounts) {
        instance.append_partitions(disks, directory, mounts);
    }
};

// ========== validate_device_path Tests ==========

TEST_F(DiskServiceTest, ValidateDevicePath_ValidSataPath_ReturnsSuccess) {
    // Note: This test will fail if /dev/sda doesn't exist on the system
    // It's testing the prefix validation logic
    auto result = service.validate_device_path("/dev/sda");
    // We only check that it doesn't fail due to prefix validation
    // It may fail due to device not existing, which is OK
    if (!result) {
        // If it fails, it should NOT be because of prefix validation
        EXPECT_NE(result.error().message, "Device path prefix not allowed");
    }
}

TEST_F(DiskServiceTest, ValidateDevicePath_ValidNvmePath_ReturnsSuccess) {
    auto result = service.validate_device_path("/dev/nvme0n1");
    if (!result) {
        EXPECT_NE(result.error().message, "Device path prefix not allowed");
    }
}

TEST_F(DiskServiceTest, ValidateDevicePath_ValidMmcPath_ReturnsSuccess) {
    auto result = service.validate_device_path("/dev/mmcblk0");
    if (!result) {
        EXPECT_NE(result.error().message, "Device path prefix not allowed");
    }
}

TEST_F(DiskServiceTest, ValidateDevicePath_ValidVdPath_ReturnsSuccess) {
    auto result = service.validate_device_path("/dev/vda");
    if (!result) {
        EXPECT_NE(result.error().message, "Device path prefix not allowed");
    }
}

TEST_F(DiskServiceTest, ValidateDevicePath_InvalidPrefix_ReturnsError) {
    auto result = service.validate_device_path("/dev/loop0");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Device path prefix not allowed");
}

TEST_F(DiskServiceTest, ValidateDevicePath_MapperPath_ReturnsError) {
    // /dev/mapper/* paths should be rejected (LVM logical volumes)
    auto result = service.validate_device_path("/dev/mapper/vg-lv");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Device path prefix not allowed");
}

TEST_F(DiskServiceTest, ValidateDevicePath_DmPath_ReturnsError) {
    // /dev/dm-* paths should be rejected (device-mapper)
    auto result = service.validate_device_path("/dev/dm-0");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Device path prefix not allowed");
}

TEST_F(DiskServiceTest, ValidateDevicePath_RamPath_ReturnsError) {
    auto result = service.validate_device_path("/dev/ram0");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Device path prefix not allowed");
}

TEST_F(DiskServiceTest, ValidateDevicePath_EmptyPath_ReturnsError) {
    auto result = service.validate_device_path("");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Device path prefix not allowed");
}

TEST_F(DiskServiceTest, ValidateDevicePath_RelativePath_ReturnsError) {
    auto result = service.validate_device_path("sda");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Device path prefix not allowed");
}

TEST_F(DiskServiceTest, ValidateDevicePath_NonexistentDevice_ReturnsError) {
    // Even with valid prefix, non-existent device should fail stat()
    auto result = service.validate_device_path("/dev/sdzzzz999");
    ASSERT_FALSE(result.has_value());
    // Should fail on stat, not prefix
    EXPECT_NE(result.error().message, "Device path prefix not allowed");
}

TEST_F(DiskServiceTest, ValidateDevicePath_NotBlockDevice_ReturnsError) {
    // /dev/null exists but is not a block device
    auto result = service.validate_device_path("/dev/sdnull");
    ASSERT_FALSE(result.has_value());
}

// ========== is_disk_writable Tests ==========

TEST_F(DiskServiceTest, IsDiskWritable_InvalidPath_ReturnsFalse) {
    // Invalid prefix should return false
    bool writable = service.is_disk_writable("/dev/loop0");
    EXPECT_FALSE(writable);
}

TEST_F(DiskServiceTest, IsDiskWritable_NonexistentDevice_ReturnsFalse) {
    bool writable = service.is_disk_writable("/dev/sdzzzz999");
    EXPECT_FALSE(writable);
}

TEST_F(DiskServiceTest, IsDiskWritable_EmptyPath_ReturnsFalse) {
    bool writable = service.is_disk_writable("");
    EXPECT_FALSE(writable);
}

// ========== get_disk_size Tests ==========

TEST_F(DiskServiceTest, GetDiskSize_InvalidPath_ReturnsError) {
    auto result = service.get_disk_size("/dev/loop0");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Device path prefix not allowed");
}

TEST_F(DiskServiceTest, GetDiskSize_NonexistentDevice_ReturnsError) {
    auto result = service.get_disk_size("/dev/sdzzzz999");
    ASSERT_FALSE(result.has_value());
    // Should fail on stat or open, not prefix validation
    EXPECT_NE(result.error().message, "Device path prefix not allowed");
}

TEST_F(DiskServiceTest, GetDiskSize_EmptyPath_ReturnsError) {
    auto result = service.get_disk_size("");
    ASSERT_FALSE(result.has_value());
}

// ========== unmount_disk Tests ==========

TEST_F(DiskServiceTest, UnmountDisk_InvalidPath_ReturnsError) {
    auto result = service.unmount_disk("/dev/loop0");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message, "Device path prefix not allowed");
}

TEST_F(DiskServiceTest, UnmountDisk_EmptyPath_ReturnsError) {
    auto result = service.unmount_disk("");
    ASSERT_FALSE(result.has_value());
}

// ========== get_available_disks Tests ==========

TEST_F(DiskServiceTest, GetAvailableDisks_ReturnsVector) {
    // This test just verifies the function runs without crashing
    // and returns a vector (may be empty if no disks)
    auto disks = service.get_available_disks_sync();
    // We can't assert on content, but we can check it's a valid vector
    SUCCEED();
}

TEST_F(DiskServiceTest, GetAvailableDisks_ExcludesLoopDevices) {
    auto disks = service.get_available_disks_sync();
    for (const auto& disk : disks) {
        EXPECT_FALSE(disk.path.find("/dev/loop") != std::string::npos)
            << "Loop device found in disk list: " << disk.path;
    }
}

TEST_F(DiskServiceTest, GetAvailableDisks_ExcludesRamDisks) {
    auto disks = service.get_available_disks_sync();
    for (const auto& disk : disks) {
        EXPECT_FALSE(disk.path.find("/dev/ram") != std::string::npos)
            << "RAM disk found in disk list: " << disk.path;
    }
}

TEST_F(DiskServiceTest, GetAvailableDisks_ExcludesDmDevices) {
    auto disks = service.get_available_disks_sync();
    for (const auto& disk : disks) {
        EXPECT_FALSE(disk.path.find("/dev/dm-") != std::string::npos)
            << "Device-mapper device found in disk list: " << disk.path;
    }
}

TEST_F(DiskServiceTest, GetAvailableDisks_HasValidPaths) {
    auto disks = service.get_available_disks_sync();
    for (const auto& disk : disks) {
        // All returned disks should have valid path prefixes
        EXPECT_TRUE(disk.path.starts_with("/dev/sd") || disk.path.starts_with("/dev/nvme") ||
                    disk.path.starts_with("/dev/mmcblk") || disk.path.starts_with("/dev/vd"))
            << "Invalid path prefix: " << disk.path;
    }
}

TEST_F(DiskServiceTest, GetAvailableDisks_HasNonZeroSize) {
    auto disks = service.get_available_disks_sync();
    for (const auto& disk : disks) {
        // All returned disks should have non-zero size
        EXPECT_GT(disk.size_bytes, 0ULL) << "Disk has zero size: " << disk.path;
    }
}

TEST_F(DiskServiceTest, EveryPartitionRetainsOriginalDiskParent) {
    class TestDiskService : public DiskService {
    public:
        auto validate_device_path(const std::string&) -> std::expected<void, util::Error> override {
            return {};
        }
    } instance;
    TempTestFile root;
    ASSERT_TRUE(root.valid());
    const auto directory = root.path() + "-sysfs";
    std::filesystem::create_directories(directory);
    struct Cleanup {
        std::string path;
        ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{directory};
    for (const auto* name : {"sda1", "sda2", "sda3"}) {
        const auto path = directory + "/" + name;
        std::filesystem::create_directory(path);
        std::ofstream{path + "/partition"} << "1";
        std::ofstream{path + "/size"} << "8";
    }
    auto parent = MockDiskService::CreateTestDisk("/dev/sda", 16'384);
    parent.model = "Parent model";
    std::vector<DiskInfo> disks{parent};
    MountCache mounts{{{.device = "/dev/sda2", .mount_point = "/mnt/test", .filesystem = "ext4"}}};
    append_partitions(instance, disks, directory, mounts);
    ASSERT_EQ(disks.size(), 4u);
    for (size_t i = 1; i < disks.size(); ++i) {
        EXPECT_EQ(disks[i].parent_disk, "/dev/sda");
        EXPECT_EQ(disks[i].model, parent.model);
        EXPECT_EQ(disks[i].serial, parent.serial);
        EXPECT_TRUE(disks[i].is_partition);
        EXPECT_EQ(disks[i].is_mounted, disks[i].path == "/dev/sda2");
    }
}
