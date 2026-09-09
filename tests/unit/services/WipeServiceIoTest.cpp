#include "algorithms/ATASecureEraseAlgorithm.hpp"
#include "fixtures/FakeDeviceIO.hpp"
#include "fixtures/TestFixtures.hpp"
#include "helper/services/WipeService.hpp"
#include "mocks/MockDiskService.hpp"
#include "util/WriteHelpers.hpp"

#include <future>

namespace {

constexpr auto TARGET = "/dev/nvme999n1";
constexpr auto SIBLING = "/dev/nvme999n2";
constexpr auto CONTROLLER = "/dev/nvme999";

class WipeServiceIoTest : public testing::Test {
protected:
    FakeDeviceIO io;
    std::shared_ptr<MockDiskService> disks = MockDiskService::CreateNiceMock();
    std::unique_ptr<WipeService> service;
    std::vector<DiskInfo> disk_list;
    std::vector<WipeProgress> progress;
    std::mutex progress_mutex;

    void SetUp() override {
        io.devices[TARGET] = {};
        io.devices[SIBLING] = {};
        io.devices[CONTROLLER] = {};
        disk_list = {MockDiskService::CreateTestDisk(TARGET, 2'048),
                     MockDiskService::CreateTestDisk(SIBLING, 2'048)};
        ON_CALL(*disks, get_available_disks_blocking()).WillByDefault([this] { return disk_list; });
        service = std::make_unique<WipeService>(disks);
    }

    auto callback() -> ProgressCallback {
        return [this](const WipeProgress& value) {
            std::lock_guard lock(progress_mutex);
            progress.push_back(value);
        };
    }

    void wait_for_completion() {
        ASSERT_TRUE(ThreadingTestHelper::WaitUntil(
            [this] { return !service->is_operation_in_progress(TARGET); }));
        service.reset();  // Join before inspecting simulated I/O or progress.
    }

    auto terminals() const -> size_t {
        return std::ranges::count_if(progress, [](const auto& p) { return p.is_complete; });
    }
};

TEST_F(WipeServiceIoTest, RejectsHardwareEraseOfPartitionBeforeDeviceAccess) {
    disk_list[0].path = "/dev/nvme999n1p1";
    disk_list[0].is_partition = true;
    disk_list[0].parent_disk = TARGET;
    EXPECT_FALSE(
        service->wipe_disk(disk_list[0].path, WipeAlgorithm::ATA_SECURE_ERASE, callback()));
    ASSERT_EQ(progress.size(), 1u);
    EXPECT_TRUE(progress.back().has_error);
    EXPECT_EQ(io.sanitize_commands, 0);
}

TEST_F(WipeServiceIoTest, SoftwarePartitionWipePreservesParentAndSiblingData) {
    constexpr auto PARTITION = "/dev/nvme999n1p1";
    io.devices[TARGET].bytes.assign(2'048, 0xAB);
    io.devices[SIBLING].bytes.assign(2'048, 0xCD);
    io.devices[PARTITION].bytes.assign(512, 0xEF);
    auto partition = MockDiskService::CreateTestDisk(PARTITION, 512);
    partition.is_partition = true;
    partition.parent_disk = TARGET;
    disk_list.push_back(partition);
    ASSERT_TRUE(service->wipe_disk(PARTITION, WipeAlgorithm::ZERO_FILL, callback(), true));
    ASSERT_TRUE(ThreadingTestHelper::WaitUntil(
        [&] { return !service->is_operation_in_progress(PARTITION); }));
    service.reset();
    EXPECT_EQ(io.devices[PARTITION].bytes, std::vector<uint8_t>(512, 0));
    EXPECT_EQ(io.devices[TARGET].bytes, std::vector<uint8_t>(2'048, 0xAB));
    EXPECT_EQ(io.devices[SIBLING].bytes, std::vector<uint8_t>(2'048, 0xCD));
    ASSERT_EQ(terminals(), 1u);
    EXPECT_TRUE(progress.back().verification_passed);
    EXPECT_FALSE(progress.back().has_error);
}

TEST_F(WipeServiceIoTest, RejectsMountedSiblingNamespace) {
    disk_list[1].is_mounted = true;
    EXPECT_FALSE(service->wipe_disk(TARGET, WipeAlgorithm::ATA_SECURE_ERASE, callback()));
    EXPECT_EQ(io.sanitize_commands, 0);
}

TEST_F(WipeServiceIoTest, RejectsMountedPartitionOfSiblingNamespace) {
    auto partition = MockDiskService::CreateTestDisk("/dev/nvme999n2p1", 512, true);
    partition.is_partition = true;
    partition.parent_disk = SIBLING;
    disk_list.push_back(partition);
    EXPECT_FALSE(service->wipe_disk(TARGET, WipeAlgorithm::ATA_SECURE_ERASE, callback()));
    EXPECT_EQ(io.sanitize_commands, 0);
}

TEST_F(WipeServiceIoTest, BusySiblingAbortsBeforeFirmwareCommandAndReleasesClaims) {
    io.devices[SIBLING].busy = true;
    ASSERT_TRUE(service->wipe_disk(TARGET, WipeAlgorithm::ATA_SECURE_ERASE, callback()));
    wait_for_completion();
    ASSERT_EQ(terminals(), 1u);
    EXPECT_TRUE(progress.back().has_error);
    EXPECT_EQ(io.sanitize_commands, 0);
    EXPECT_EQ(io.devices[TARGET].claims, 0);
}

TEST_F(WipeServiceIoTest, HoldsEveryNamespaceClaimUntilFirmwareCompletes) {
    ASSERT_TRUE(service->wipe_disk(TARGET, WipeAlgorithm::ATA_SECURE_ERASE, callback()));
    wait_for_completion();
    EXPECT_EQ(io.sanitize_commands, 1);
    EXPECT_TRUE(io.all_namespaces_claimed);
    EXPECT_EQ(terminals(), 1u);
    EXPECT_FALSE(progress.back().has_error);
    EXPECT_EQ(progress.back().total_passes, 1);
    EXPECT_EQ(io.devices[TARGET].claims, 0);
    EXPECT_EQ(io.devices[SIBLING].claims, 0);
}

TEST_F(WipeServiceIoTest, FormatFallbackUsesReturnedNamespaceIdAndPreservesLbaFormat) {
    io.format_only = true;
    ASSERT_TRUE(service->wipe_disk(TARGET, WipeAlgorithm::ATA_SECURE_ERASE, callback()));
    wait_for_completion();
    EXPECT_EQ(io.formatted_namespace, 7u);
    EXPECT_EQ(io.format_command, 3u | (2u << 9));
    EXPECT_EQ(io.sanitize_commands, 0);
    EXPECT_TRUE(io.all_namespaces_claimed);
    ASSERT_EQ(terminals(), 1u);
    EXPECT_FALSE(progress.back().has_error);
}

TEST_F(WipeServiceIoTest, InvalidNamespaceIdDoesNotIssueFormat) {
    io.format_only = true;
    io.namespace_id = -1;
    ASSERT_TRUE(service->wipe_disk(TARGET, WipeAlgorithm::ATA_SECURE_ERASE, callback()));
    wait_for_completion();
    EXPECT_EQ(io.formatted_namespace, 0u);
    EXPECT_EQ(terminals(), 1u);
    EXPECT_TRUE(progress.back().has_error);
}

TEST_F(WipeServiceIoTest, CancellationDuringFirmwarePreparationHasOnlyFailedTerminal) {
    io.before_identify = [this] {
        EXPECT_TRUE(service->cancel_operation(TARGET));
    };
    ASSERT_TRUE(service->wipe_disk(TARGET, WipeAlgorithm::ATA_SECURE_ERASE, callback()));
    wait_for_completion();
    EXPECT_EQ(io.sanitize_commands, 0);
    EXPECT_EQ(terminals(), 1u);
    EXPECT_TRUE(progress.back().has_error);
    EXPECT_NE(progress.back().error_message.find("cancel"), std::string::npos);
}

TEST_F(WipeServiceIoTest, VerificationPreservesUnwritableSectorCount) {
    // The media already contains zeros, so read-back passes even though every
    // new write fails. The certificate must still disclose all skipped sectors.
    io.devices[TARGET].fail_writes = true;
    ASSERT_TRUE(service->wipe_disk(TARGET, WipeAlgorithm::ZERO_FILL, callback(), true));
    wait_for_completion();
    ASSERT_EQ(terminals(), 1u);
    EXPECT_FALSE(progress.back().has_error);
    EXPECT_TRUE(progress.back().verification_passed);
    EXPECT_EQ(progress.back().bad_block_count, 4u);
    EXPECT_EQ(io.devices[TARGET].flushes, 1);
}

TEST_F(WipeServiceIoTest, MultiPassCompletionRetainsPassCountAndSize) {
    ASSERT_TRUE(service->wipe_disk(TARGET, WipeAlgorithm::VSITR, callback()));
    wait_for_completion();
    ASSERT_EQ(terminals(), 1u);
    EXPECT_FALSE(progress.back().has_error);
    EXPECT_EQ(progress.back().total_passes, 7);
    EXPECT_EQ(progress.back().current_pass, 7);
    EXPECT_EQ(progress.back().total_bytes, 2'048u);
    EXPECT_EQ(progress.back().bytes_written, 2'048u);
}

TEST_F(WipeServiceIoTest, DifferentDevicesRunTogetherAndControllerEraseCannotOverlap) {
    std::promise<void> release;
    auto ready = release.get_future().share();
    std::atomic<int> writers{0};
    io.before_write = [&] {
        ++writers;
        ready.wait();
    };
    const bool first = service->wipe_disk(TARGET, WipeAlgorithm::ZERO_FILL, callback());
    const bool second = service->wipe_disk(SIBLING, WipeAlgorithm::ZERO_FILL, callback());
    EXPECT_TRUE(first);
    EXPECT_TRUE(second);
    EXPECT_TRUE(ThreadingTestHelper::WaitUntil([&] { return writers.load() == 2; }));
    EXPECT_FALSE(service->wipe_disk(TARGET, WipeAlgorithm::ZERO_FILL, callback()));
    // A third namespace is idle but its firmware erase would overlap both writes.
    disk_list.push_back(MockDiskService::CreateTestDisk("/dev/nvme999n3", 2'048));
    EXPECT_FALSE(service->wipe_disk("/dev/nvme999n3", WipeAlgorithm::ATA_SECURE_ERASE, callback()));
    EXPECT_TRUE(service->cancel_operation(TARGET));
    release.set_value();
    EXPECT_TRUE(ThreadingTestHelper::WaitUntil([&] {
        return !service->is_operation_in_progress(TARGET) &&
               !service->is_operation_in_progress(SIBLING);
    }));
    service.reset();
    EXPECT_EQ(terminals(), 2u);
    EXPECT_EQ(std::ranges::count_if(progress,
                                    [](const auto& p) { return p.is_complete && !p.has_error; }),
              1);
}

TEST(WriteHelpersIoTest, ShortWriteThenErrorRetriesAtOriginalBufferOffset) {
    FakeDeviceIO io;
    io.devices[TARGET].bytes.assign(2'048, 0xAB);
    io.devices[TARGET].short_then_error = true;
    const int fd = open(TARGET, O_RDWR);
    ASSERT_GE(fd, 0);
    std::vector<uint8_t> zeros(2'048);
    uint64_t bad = 0;
    EXPECT_EQ(util::write_with_bad_sector_tolerance(fd, zeros, bad), 2'048u);
    EXPECT_EQ(lseek(fd, 0, SEEK_CUR), 2'048);
    EXPECT_EQ(io.devices[TARGET].bytes, zeros);
    EXPECT_EQ(io.devices[TARGET].retry_offsets, (std::vector<off_t>{512, 1'024, 1'536}));
    EXPECT_EQ(bad, 0u);
    close(fd);
}

}  // namespace
