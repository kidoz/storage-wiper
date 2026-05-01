/**
 * @file DevicePathMatcherTest.cpp
 * @brief Unit tests for device_path_matcher helpers.
 *
 * Regression target: prefix-only string matching let "/dev/sda" wrongly match
 * "/dev/sdaa1" (>26 SCSI-style disks) and force-unmount sibling devices.
 */

#include "helper/services/DevicePathMatcher.hpp"

#include <gtest/gtest.h>

using device_path_matcher::is_device_or_partition_of;
using device_path_matcher::is_partition_suffix;

// ========== is_partition_suffix ==========

TEST(DevicePathMatcher, PartitionSuffix_Empty_False) {
    EXPECT_FALSE(is_partition_suffix(""));
}

TEST(DevicePathMatcher, PartitionSuffix_PureDigits_True) {
    EXPECT_TRUE(is_partition_suffix("1"));
    EXPECT_TRUE(is_partition_suffix("9"));
    EXPECT_TRUE(is_partition_suffix("10"));
    EXPECT_TRUE(is_partition_suffix("128"));
}

TEST(DevicePathMatcher, PartitionSuffix_PPlusDigits_True) {
    EXPECT_TRUE(is_partition_suffix("p1"));
    EXPECT_TRUE(is_partition_suffix("p10"));
    EXPECT_TRUE(is_partition_suffix("p128"));
}

TEST(DevicePathMatcher, PartitionSuffix_PAlone_False) {
    EXPECT_FALSE(is_partition_suffix("p"));
}

TEST(DevicePathMatcher, PartitionSuffix_Letters_False) {
    EXPECT_FALSE(is_partition_suffix("a"));
    EXPECT_FALSE(is_partition_suffix("a1"));
    EXPECT_FALSE(is_partition_suffix("z9"));
    EXPECT_FALSE(is_partition_suffix("play"));
}

TEST(DevicePathMatcher, PartitionSuffix_DigitsThenLetters_False) {
    EXPECT_FALSE(is_partition_suffix("1p"));
    EXPECT_FALSE(is_partition_suffix("10x"));
}

// ========== is_device_or_partition_of: SATA/SCSI ==========

TEST(DevicePathMatcher, SataIdentity_Matches) {
    EXPECT_TRUE(is_device_or_partition_of("/dev/sda", "/dev/sda"));
}

TEST(DevicePathMatcher, SataPartitions_Match) {
    EXPECT_TRUE(is_device_or_partition_of("/dev/sda", "/dev/sda1"));
    EXPECT_TRUE(is_device_or_partition_of("/dev/sda", "/dev/sda10"));
    EXPECT_TRUE(is_device_or_partition_of("/dev/sda", "/dev/sda128"));
}

TEST(DevicePathMatcher, SataSiblingDisks_DoNotMatch) {
    // The bug: "/dev/sda" must NOT match "/dev/sdaa1" or "/dev/sdaa".
    // These appear on hosts with > 26 SCSI-style disks (sdaa..sdaz, sdba..).
    EXPECT_FALSE(is_device_or_partition_of("/dev/sda", "/dev/sdaa"));
    EXPECT_FALSE(is_device_or_partition_of("/dev/sda", "/dev/sdaa1"));
    EXPECT_FALSE(is_device_or_partition_of("/dev/sda", "/dev/sdaa10"));
    EXPECT_FALSE(is_device_or_partition_of("/dev/sda", "/dev/sdaplay"));
    EXPECT_FALSE(is_device_or_partition_of("/dev/sda", "/dev/sdb"));
    EXPECT_FALSE(is_device_or_partition_of("/dev/sda", "/dev/sdb1"));
}

// ========== is_device_or_partition_of: NVMe ==========

TEST(DevicePathMatcher, NvmeIdentity_Matches) {
    EXPECT_TRUE(is_device_or_partition_of("/dev/nvme0n1", "/dev/nvme0n1"));
}

TEST(DevicePathMatcher, NvmePartitions_Match) {
    EXPECT_TRUE(is_device_or_partition_of("/dev/nvme0n1", "/dev/nvme0n1p1"));
    EXPECT_TRUE(is_device_or_partition_of("/dev/nvme0n1", "/dev/nvme0n1p10"));
}

TEST(DevicePathMatcher, NvmeSiblings_DoNotMatch) {
    // "/dev/nvme0n1" prefix-matches "/dev/nvme0n11", "/dev/nvme0n1px", etc.
    EXPECT_FALSE(is_device_or_partition_of("/dev/nvme0n1", "/dev/nvme0n11"));
    EXPECT_FALSE(is_device_or_partition_of("/dev/nvme0n1", "/dev/nvme0n10"));
    EXPECT_FALSE(is_device_or_partition_of("/dev/nvme0n1", "/dev/nvme0n1px"));
    EXPECT_FALSE(is_device_or_partition_of("/dev/nvme0n1", "/dev/nvme0n1p"));
    EXPECT_FALSE(is_device_or_partition_of("/dev/nvme0n1", "/dev/nvme0n2"));
    EXPECT_FALSE(is_device_or_partition_of("/dev/nvme0n1", "/dev/nvme1n1"));
}

// ========== is_device_or_partition_of: MMC/SD ==========

TEST(DevicePathMatcher, MmcblkPartitions_Match) {
    EXPECT_TRUE(is_device_or_partition_of("/dev/mmcblk0", "/dev/mmcblk0"));
    EXPECT_TRUE(is_device_or_partition_of("/dev/mmcblk0", "/dev/mmcblk0p1"));
    EXPECT_TRUE(is_device_or_partition_of("/dev/mmcblk0", "/dev/mmcblk0p10"));
}

TEST(DevicePathMatcher, MmcblkSiblings_DoNotMatch) {
    EXPECT_FALSE(is_device_or_partition_of("/dev/mmcblk0", "/dev/mmcblk0p"));
    EXPECT_FALSE(is_device_or_partition_of("/dev/mmcblk0", "/dev/mmcblk00"));
    EXPECT_FALSE(is_device_or_partition_of("/dev/mmcblk0", "/dev/mmcblk1"));
    EXPECT_FALSE(is_device_or_partition_of("/dev/mmcblk0", "/dev/mmcblk1p1"));
}

// ========== Edge cases ==========

TEST(DevicePathMatcher, EmptyInputs_DoNotMatch) {
    EXPECT_FALSE(is_device_or_partition_of("", "/dev/sda"));
    EXPECT_FALSE(is_device_or_partition_of("/dev/sda", ""));
    EXPECT_FALSE(is_device_or_partition_of("", ""));
}

TEST(DevicePathMatcher, ConstexprUsable) {
    // The helpers are constexpr; this constrains future edits to keep them so.
    constexpr bool ok = is_device_or_partition_of("/dev/sda", "/dev/sda1");
    constexpr bool bad = is_device_or_partition_of("/dev/sda", "/dev/sdaa1");
    static_assert(ok);
    static_assert(!bad);
}
