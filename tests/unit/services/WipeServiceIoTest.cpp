#include "algorithms/ATASecureEraseAlgorithm.hpp"
#include "fixtures/FakeDeviceIO.hpp"
#include "fixtures/TestFixtures.hpp"
#include "helper/services/DeviceIdentity.hpp"
#include "helper/services/WipeService.hpp"
#include "mocks/MockDiskService.hpp"
#include "util/WriteHelpers.hpp"

#include <fstream>
#include <future>

namespace {

constexpr auto TARGET = "/dev/nvme999n1";
constexpr auto SIBLING = "/dev/nvme999n2";
constexpr auto CONTROLLER = "/dev/nvme999";
constexpr auto ATA_TARGET = "/dev/sdz";

class FirmwareTopologyTest : public testing::Test {
protected:
    TempTestFile unique_path;
    std::filesystem::path root = unique_path.path() + "-sysfs";

    void SetUp() override {
        namespace fs = std::filesystem;
        fs::create_directories(root / "class/nvme");
        fs::create_directories(root / "class/block");
        fs::create_directories(root / "devices/nvme999/nvme999n1");
        std::ofstream{root / "devices/nvme999/subsysnqn"} << "nqn.test.subsystem\n";
        std::ofstream{root / "devices/nvme999/nvme999n1/nsid"} << "1\n";
        fs::create_directory_symlink(root / "devices/nvme999", root / "class/nvme/nvme999");
        fs::create_directory_symlink(root / "devices/nvme999/nvme999n1",
                                     root / "class/block/nvme999n1");
    }

    void TearDown() override { std::filesystem::remove_all(root); }
};

TEST_F(FirmwareTopologyTest, AcceptsSingleControllerAndNamespace) {
    EXPECT_TRUE(device_identity::validate_firmware_topology(TARGET, root));
}

TEST_F(FirmwareTopologyTest, RejectsSecondControllerInSameSubsystem) {
    std::filesystem::create_directories(root / "class/nvme/nvme998");
    std::ofstream{root / "class/nvme/nvme998/subsysnqn"} << "nqn.test.subsystem\n";
    EXPECT_FALSE(device_identity::validate_firmware_topology(TARGET, root));
}

TEST_F(FirmwareTopologyTest, RejectsAdditionalNamespace) {
    std::filesystem::create_directories(root / "devices/nvme999/nvme999n2");
    std::ofstream{root / "devices/nvme999/nvme999n2/nsid"} << "2\n";
    EXPECT_FALSE(device_identity::validate_firmware_topology(TARGET, root));
}

TEST_F(FirmwareTopologyTest, RejectsMultipathHeadAndUnavailableTopology) {
    namespace fs = std::filesystem;
    fs::remove(root / "class/block/nvme999n1");
    fs::create_directories(root / "devices/virtual/nvme-subsystem/nvme999n1");
    fs::create_directory_symlink(root / "devices/virtual/nvme-subsystem/nvme999n1",
                                 root / "class/block/nvme999n1");
    EXPECT_FALSE(device_identity::validate_firmware_topology(TARGET, root));
    EXPECT_FALSE(device_identity::validate_firmware_topology(TARGET, root / "missing"));
}

class WipeServiceIoTest : public testing::Test {
protected:
    FakeDeviceIO io;
    std::shared_ptr<MockDiskService> disks = MockDiskService::CreateNiceMock();
    std::unique_ptr<WipeService> service;
    std::vector<DiskInfo> disk_list;
    std::vector<WipeProgress> progress;
    std::mutex progress_mutex;

    void SetUp() override {
        io.namespace_id = 1;
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

    void wait_for_ata_completion() {
        ASSERT_TRUE(ThreadingTestHelper::WaitUntil(
            [this] { return !service->is_operation_in_progress(ATA_TARGET); }));
        service.reset();
    }

    auto prepare_ata_plan() -> WipePlan {
        io.devices[ATA_TARGET] = {};
        disk_list = {MockDiskService::CreateTestDisk(ATA_TARGET, 2'048)};
        auto plan = service->prepare_plan(ATA_TARGET, WipeAlgorithm::ATA_SECURE_ERASE, false);
        EXPECT_TRUE(plan) << plan.error().message;
        return plan.value_or(WipePlan{});
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

TEST_F(WipeServiceIoTest, ExpiredStartDeadlineCannotLaunchWorker) {
    auto plan = service->prepare_plan(TARGET, WipeAlgorithm::ZERO_FILL, false);
    ASSERT_TRUE(plan);
    EXPECT_FALSE(service->start_prepared(
        *plan, callback(), std::chrono::steady_clock::now() - std::chrono::seconds{1}));
    EXPECT_FALSE(service->is_operation_in_progress(TARGET));
    EXPECT_EQ(io.devices[TARGET].writes, 0);
}

TEST_F(WipeServiceIoTest, PreparedWipeRetainsIdentityAndVerifiesOriginalDevice) {
    io.devices[TARGET].bytes.assign(2'048, 0xAB);
    auto plan = service->prepare_plan(TARGET, WipeAlgorithm::ZERO_FILL, true);
    ASSERT_TRUE(plan) << plan.error().message;
    EXPECT_EQ(plan->device.disk.path, TARGET);
    EXPECT_EQ(plan->device.disk_sequence, 1u);
    EXPECT_EQ(plan->firmware.method, "overwrite");
    EXPECT_EQ(io.devices[TARGET].writes, 0);
    ASSERT_TRUE(service->start_prepared(*plan, callback()));
    wait_for_completion();
    EXPECT_EQ(io.devices[TARGET].bytes, std::vector<uint8_t>(2'048, 0));
    ASSERT_EQ(terminals(), 1u);
    EXPECT_TRUE(progress.back().verification_passed);
}

TEST_F(WipeServiceIoTest, PreparedWipeRejectsReusedDeviceNumberWithNewDiskSequence) {
    auto plan = service->prepare_plan(TARGET, WipeAlgorithm::ZERO_FILL, false);
    ASSERT_TRUE(plan);
    ++io.devices[TARGET].disk_sequence;
    ASSERT_TRUE(service->start_prepared(*plan, callback()));
    wait_for_completion();
    EXPECT_EQ(io.devices[TARGET].writes, 0);
    EXPECT_EQ(io.devices[TARGET].claims, 0);
    ASSERT_EQ(terminals(), 1u);
    EXPECT_TRUE(progress.back().has_error);
    EXPECT_NE(progress.back().error_message.find("identity changed"), std::string::npos);
}

TEST_F(WipeServiceIoTest, PreparedWipeRejectsChangedDeviceNumber) {
    auto plan = service->prepare_plan(TARGET, WipeAlgorithm::ZERO_FILL, false);
    ASSERT_TRUE(plan);
    ++io.devices[TARGET].device_number;
    ASSERT_TRUE(service->start_prepared(*plan, callback()));
    wait_for_completion();
    EXPECT_EQ(io.devices[TARGET].writes, 0);
    ASSERT_EQ(terminals(), 1u);
    EXPECT_TRUE(progress.back().has_error);
}

TEST_F(WipeServiceIoTest, PreparedWipeRejectsChangedSize) {
    auto plan = service->prepare_plan(TARGET, WipeAlgorithm::ZERO_FILL, false);
    ASSERT_TRUE(plan);
    io.devices[TARGET].bytes.resize(4'096);
    ASSERT_TRUE(service->start_prepared(*plan, callback()));
    wait_for_completion();
    EXPECT_EQ(io.devices[TARGET].writes, 0);
    ASSERT_EQ(terminals(), 1u);
    EXPECT_TRUE(progress.back().has_error);
    EXPECT_NE(progress.back().error_message.find("size changed"), std::string::npos);
}

TEST_F(WipeServiceIoTest, PrepareFailsClosedWhenKernelCannotSupplyDiskSequence) {
    io.devices[TARGET].disk_sequence_supported = false;
    const auto plan = service->prepare_plan(TARGET, WipeAlgorithm::ZERO_FILL, false);
    ASSERT_FALSE(plan);
    EXPECT_NE(plan.error().message.find("BLKGETDISKSEQ"), std::string::npos);
    EXPECT_EQ(io.devices[TARGET].writes, 0);
    EXPECT_EQ(io.devices[TARGET].claims, 0);
}

TEST_F(WipeServiceIoTest, PreparedWipeRechecksMountStateBeforeOpeningForWrite) {
    auto plan = service->prepare_plan(TARGET, WipeAlgorithm::ZERO_FILL, false);
    ASSERT_TRUE(plan);
    disk_list.front().is_mounted = true;
    EXPECT_FALSE(service->start_prepared(*plan, callback()));
    EXPECT_EQ(io.devices[TARGET].writes, 0);
    EXPECT_EQ(io.devices[TARGET].claims, 0);
}

TEST_F(WipeServiceIoTest, PrepareRejectsReplacementWhileReadingDisplayedMetadata) {
    int enumerations = 0;
    ON_CALL(*disks, get_available_disks_blocking()).WillByDefault([&] {
        if (++enumerations == 2) {
            ++io.devices[TARGET].disk_sequence;
            disk_list.front().serial = "replacement";
        }
        return disk_list;
    });
    const auto plan = service->prepare_plan(TARGET, WipeAlgorithm::ZERO_FILL, false);
    ASSERT_FALSE(plan);
    EXPECT_NE(plan.error().message.find("identity changed"), std::string::npos);
    EXPECT_EQ(io.devices[TARGET].writes, 0);
}

TEST_F(WipeServiceIoTest, PreparedNamespaceFormatLeavesMountedSiblingOutsideScope) {
    io.format_only = true;
    io.namespace_id = 7;
    disk_list[1].is_mounted = true;
    auto plan = service->prepare_plan(TARGET, WipeAlgorithm::ATA_SECURE_ERASE, false);
    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_EQ(plan->targets.size(), 1u);
    EXPECT_EQ(plan->firmware.scope, "device");
    ASSERT_TRUE(service->start_prepared(*plan, callback()));
    wait_for_completion();
    EXPECT_EQ(io.formatted_namespace, 7u);
    ASSERT_EQ(terminals(), 1u);
    EXPECT_FALSE(progress.back().has_error);
    // Two read-only prepare handles and one retained write handle; firmware
    // discovery and execution never reopen the namespace or its controller.
    EXPECT_EQ(io.opens, 3);
    EXPECT_EQ(io.devices[SIBLING].claims, 0);
}

TEST_F(WipeServiceIoTest, PreparedFirmwareRejectsChangedMethodWithoutIssuingCommand) {
    io.format_only = true;
    auto plan = service->prepare_plan(TARGET, WipeAlgorithm::ATA_SECURE_ERASE, false);
    ASSERT_TRUE(plan);
    io.format_only = false;
    ASSERT_TRUE(service->start_prepared(*plan, callback()));
    wait_for_completion();
    EXPECT_EQ(io.sanitize_commands, 0);
    EXPECT_EQ(io.formatted_namespace, 0u);
    ASSERT_EQ(terminals(), 1u);
    EXPECT_TRUE(progress.back().has_error);
}

TEST_F(WipeServiceIoTest, PreparedFirmwareRejectsWidenedFormatScope) {
    io.format_only = true;
    auto plan = service->prepare_plan(TARGET, WipeAlgorithm::ATA_SECURE_ERASE, false);
    ASSERT_TRUE(plan);
    io.format_attributes |= 2;
    ASSERT_TRUE(service->start_prepared(*plan, callback()));
    wait_for_completion();
    EXPECT_EQ(io.sanitize_commands, 0);
    EXPECT_EQ(io.formatted_namespace, 0u);
    ASSERT_EQ(terminals(), 1u);
    EXPECT_TRUE(progress.back().has_error);
}

TEST_F(WipeServiceIoTest, CompletedNoncancellableFirmwareRemainsSuccessfulAfterCancelRequest) {
    io.format_only = true;
    auto plan = service->prepare_plan(TARGET, WipeAlgorithm::ATA_SECURE_ERASE, false);
    ASSERT_TRUE(plan);
    EXPECT_FALSE(plan->firmware.cancellable);
    io.during_firmware_command = [this] {
        EXPECT_TRUE(service->cancel_operation(TARGET));
    };
    ASSERT_TRUE(service->start_prepared(*plan, callback()));
    wait_for_completion();
    EXPECT_EQ(io.formatted_namespace, 1u);
    ASSERT_EQ(terminals(), 1u);
    EXPECT_FALSE(progress.back().has_error);
    EXPECT_NE(progress.back().status.find("successfully"), std::string::npos);
}

TEST_F(WipeServiceIoTest, PreparedFirmwareCancellationBeforeLaunchRemainsFailure) {
    io.format_only = true;
    auto plan = service->prepare_plan(TARGET, WipeAlgorithm::ATA_SECURE_ERASE, false);
    ASSERT_TRUE(plan);
    io.before_identify = [this] {
        EXPECT_TRUE(service->cancel_operation(TARGET));
    };
    ASSERT_TRUE(service->start_prepared(*plan, callback()));
    wait_for_completion();
    EXPECT_EQ(io.formatted_namespace, 0u);
    ASSERT_EQ(terminals(), 1u);
    EXPECT_TRUE(progress.back().has_error);
}

TEST_F(WipeServiceIoTest, RejectsHiddenOrSharedSubsystemFirmwareScope) {
    io.namespace_count = 2;
    auto hidden = service->prepare_plan(TARGET, WipeAlgorithm::ATA_SECURE_ERASE, false);
    ASSERT_FALSE(hidden);
    EXPECT_NE(hidden.error().message.find("exactly one"), std::string::npos);
    io.namespace_count = 1;
    io.controller_multipath = 2;
    auto multiple = service->prepare_plan(TARGET, WipeAlgorithm::ATA_SECURE_ERASE, false);
    ASSERT_FALSE(multiple);
    EXPECT_NE(multiple.error().message.find("multi-controller"), std::string::npos);
    io.controller_multipath = 0;
    io.namespace_sharing = 1;
    auto shared = service->prepare_plan(TARGET, WipeAlgorithm::ATA_SECURE_ERASE, false);
    ASSERT_FALSE(shared);
    EXPECT_NE(shared.error().message.find("unshared"), std::string::npos);
    EXPECT_EQ(io.sanitize_commands, 0);
    EXPECT_EQ(io.formatted_namespace, 0u);
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
    io.namespace_id = 7;
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

TEST_F(WipeServiceIoTest, AtaCancellationAfterPasswordSetReportsCleanupFailure) {
    io.fail_ata_disable_password = true;
    io.during_ata_command = [this](uint8_t command) {
        if (command == 0xF1) {
            EXPECT_TRUE(service->cancel_operation(ATA_TARGET));
        }
    };
    const auto plan = prepare_ata_plan();
    ASSERT_TRUE(service->start_prepared(plan, callback()));
    wait_for_ata_completion();
    EXPECT_EQ(io.ata_disable_attempts, 1);
    EXPECT_TRUE(io.ata_password_enabled);
    EXPECT_TRUE(progress.back().has_error);
    EXPECT_NE(progress.back().error_message.find("may remain security-enabled"), std::string::npos);
    EXPECT_NE(progress.back().error_message.find("StorageWiper"), std::string::npos);
}

TEST_F(WipeServiceIoTest, AtaPrepareFailureReportsPasswordCleanupFailure) {
    io.fail_ata_prepare = true;
    io.fail_ata_disable_password = true;
    const auto plan = prepare_ata_plan();
    ASSERT_TRUE(service->start_prepared(plan, callback()));
    wait_for_ata_completion();
    EXPECT_EQ(io.ata_disable_attempts, 1);
    EXPECT_TRUE(io.ata_password_enabled);
    EXPECT_TRUE(progress.back().has_error);
    EXPECT_NE(progress.back().error_message.find("SECURITY ERASE PREPARE"), std::string::npos);
    EXPECT_NE(progress.back().error_message.find("may remain security-enabled"), std::string::npos);
}

TEST_F(WipeServiceIoTest, AtaCancellationAfterPrepareReportsCleanupFailure) {
    io.fail_ata_disable_password = true;
    io.during_ata_command = [this](uint8_t command) {
        if (command == 0xF3) {
            EXPECT_TRUE(service->cancel_operation(ATA_TARGET));
        }
    };
    const auto plan = prepare_ata_plan();
    ASSERT_TRUE(service->start_prepared(plan, callback()));
    wait_for_ata_completion();
    EXPECT_EQ(io.ata_disable_attempts, 1);
    EXPECT_TRUE(io.ata_password_enabled);
    EXPECT_TRUE(progress.back().has_error);
    EXPECT_NE(progress.back().error_message.find("cancelled"), std::string::npos);
    EXPECT_NE(progress.back().error_message.find("may remain security-enabled"), std::string::npos);
}

TEST_F(WipeServiceIoTest, AtaEraseFailureReportsPasswordCleanupFailure) {
    io.fail_ata_erase = true;
    io.fail_ata_disable_password = true;
    const auto plan = prepare_ata_plan();
    ASSERT_TRUE(service->start_prepared(plan, callback()));
    wait_for_ata_completion();
    EXPECT_EQ(io.ata_disable_attempts, 1);
    EXPECT_TRUE(io.ata_password_enabled);
    EXPECT_TRUE(progress.back().has_error);
    EXPECT_NE(progress.back().error_message.find("Secure erase command failed"), std::string::npos);
    EXPECT_NE(progress.back().error_message.find("may remain security-enabled"), std::string::npos);
}

TEST_F(WipeServiceIoTest, AtaCancellationConfirmsPasswordCleanup) {
    io.during_ata_command = [this](uint8_t command) {
        if (command == 0xF1) {
            EXPECT_TRUE(service->cancel_operation(ATA_TARGET));
        }
    };
    const auto plan = prepare_ata_plan();
    ASSERT_TRUE(service->start_prepared(plan, callback()));
    wait_for_ata_completion();
    EXPECT_EQ(io.ata_disable_attempts, 1);
    EXPECT_FALSE(io.ata_password_enabled);
    EXPECT_TRUE(progress.back().has_error);
    EXPECT_EQ(progress.back().error_message.find("may remain security-enabled"), std::string::npos);
}

TEST_F(WipeServiceIoTest, UnknownInitialSanitizeStatusCannotYieldStaleSuccess) {
    io.fail_first_sanitize_log_read = true;
    io.previous_sanitize_succeeded = true;
    ATASecureEraseAlgorithm algorithm;
    const std::atomic<bool> cancelled = false;
    EXPECT_FALSE(algorithm.execute_on_device(TARGET, 2'048, callback(), cancelled));
    EXPECT_EQ(io.sanitize_log_reads, 1);
    EXPECT_EQ(io.sanitize_commands, 0);
    ASSERT_EQ(terminals(), 1u);
    EXPECT_TRUE(progress.back().has_error);
    EXPECT_NE(progress.back().error_message.find("initial NVMe sanitize status"),
              std::string::npos);
}

TEST_F(WipeServiceIoTest, FormatDoesNotRequireSanitizeLogAccess) {
    io.format_only = true;
    io.fail_first_sanitize_log_read = true;
    auto plan = service->prepare_plan(TARGET, WipeAlgorithm::ATA_SECURE_ERASE, false);
    ASSERT_TRUE(plan);
    ASSERT_TRUE(service->start_prepared(*plan, callback()));
    wait_for_completion();
    EXPECT_EQ(io.formatted_namespace, 1u);
    ASSERT_EQ(terminals(), 1u);
    EXPECT_FALSE(progress.back().has_error);
}

TEST_F(WipeServiceIoTest, UnwritableSectorsFailEvenWhenReadBackMatches) {
    // The media already contains zeros, so read-back passes even though every
    // new write fails. The terminal result must still disclose the failed wipe.
    io.devices[TARGET].fail_writes = true;
    ASSERT_TRUE(service->wipe_disk(TARGET, WipeAlgorithm::ZERO_FILL, callback(), true));
    wait_for_completion();
    ASSERT_EQ(terminals(), 1u);
    EXPECT_TRUE(progress.back().has_error);
    EXPECT_TRUE(progress.back().verification_passed);
    EXPECT_EQ(progress.back().bad_block_count, 4u);
    EXPECT_NE(progress.back().error_message.find("4 bad sectors"), std::string::npos);
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

TEST(WriteHelpersIoTest, FailedPositionRestoreAbortsWrite) {
    FakeDeviceIO io;
    io.devices[TARGET].short_then_error = true;
    io.devices[TARGET].fail_position_restore = true;
    const int fd = open(TARGET, O_RDWR);
    ASSERT_GE(fd, 0);
    std::vector<uint8_t> zeros(2'048);
    uint64_t bad = 0;
    EXPECT_EQ(util::write_with_bad_sector_tolerance(fd, zeros, bad), 0u);
    close(fd);
}

TEST_F(WipeServiceIoTest, ThreadCreationFailureReleasesSlotAndAllowsRetry) {
    class FailingWorkerService : public WipeService {
    public:
        using WipeService::WipeService;
        bool fail = true;

    protected:
        auto create_worker_thread(std::function<void()> worker) -> std::thread override {
            if (fail) {
                throw std::system_error(
                    std::make_error_code(std::errc::resource_unavailable_try_again));
            }
            return WipeService::create_worker_thread(std::move(worker));
        }
    } instance{disks};
    const auto rejected = instance.start_wipe(TARGET, WipeAlgorithm::ZERO_FILL, callback(), false);
    ASSERT_FALSE(rejected);
    EXPECT_NE(rejected.error().message.find("Failed to start wipe worker"), std::string::npos);
    EXPECT_FALSE(instance.is_operation_in_progress(TARGET));
    ASSERT_EQ(progress.size(), 1u);
    EXPECT_TRUE(progress.back().is_complete);
    EXPECT_TRUE(progress.back().has_error);
    instance.fail = false;
    ASSERT_TRUE(instance.wipe_disk(TARGET, WipeAlgorithm::ZERO_FILL, callback()));
    EXPECT_TRUE(
        ThreadingTestHelper::WaitUntil([&] { return !instance.is_operation_in_progress(TARGET); }));
}

}  // namespace
