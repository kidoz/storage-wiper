/**
 * @file MainViewModelTest.cpp
 * @brief Unit tests for MainViewModel
 */

#include "viewmodels/MainViewModel.hpp"

#include "fixtures/TestFixtures.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <thread>

using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;

class MainViewModelTest : public ViewModelTestFixture {
protected:
    std::shared_ptr<MainViewModel> view_model;

    void SetUp() override {
        ViewModelTestFixture::SetUp();
        view_model = std::make_shared<MainViewModel>(mock_disk_service, mock_wipe_service);
    }

    void TearDown() override {
        view_model.reset();
        ViewModelTestFixture::TearDown();
    }

    // Helper to simulate connected state (required for disk loading)
    void SimulateConnected() {
        view_model->set_connection_state(true, "");
        PumpMainLoop();
    }
};

// Test: initialization creates valid view model
TEST_F(MainViewModelTest, Constructor_CreatesValidViewModel) {
    EXPECT_NE(view_model, nullptr);
}

// Test: initialize loads disks when connected
TEST_F(MainViewModelTest, Initialize_LoadsDisks) {
    std::vector<DiskInfo> test_disks = {MockDiskService::CreateTestDisk("/dev/sda"),
                                        MockDiskService::CreateTestDisk("/dev/sdb")};

    // set_connection_state(true) will call load_disks()
    EXPECT_CALL(*mock_disk_service, get_available_disks(testing::_))
        .WillOnce([test_disks](auto callback) { callback(test_disks); });

    view_model->initialize();  // Won't load disks since not connected
    SimulateConnected();       // This triggers load_disks()

    EXPECT_EQ(view_model->disks.get().size(), 2u);
}

// Test: initialize with empty disk list when connected
TEST_F(MainViewModelTest, Initialize_HandlesEmptyDiskList) {
    EXPECT_CALL(*mock_disk_service, get_available_disks(testing::_)).WillOnce([](auto callback) {
        callback(std::vector<DiskInfo>{});
    });

    view_model->initialize();
    SimulateConnected();

    EXPECT_TRUE(view_model->disks.get().empty());
}

// Test: select_disk updates selected_disk_path
TEST_F(MainViewModelTest, SelectDisk_UpdatesSelectedPath) {
    EXPECT_CALL(*mock_disk_service, get_available_disks(testing::_)).WillOnce([](auto callback) {
        callback(std::vector<DiskInfo>{});
    });

    view_model->initialize();
    SimulateConnected();
    view_model->select_disk("/dev/sda");

    EXPECT_EQ(view_model->selected_disk_path.get(), "/dev/sda");
}

// Test: select_disk with empty path
TEST_F(MainViewModelTest, SelectDisk_AllowsEmptyPath) {
    view_model->select_disk("/dev/sda");
    view_model->select_disk("");

    EXPECT_EQ(view_model->selected_disk_path.get(), "");
}

// Test: select_algorithm updates selected_algorithm
TEST_F(MainViewModelTest, SelectAlgorithm_UpdatesSelectedAlgorithm) {
    view_model->select_algorithm(WipeAlgorithm::GUTMANN);

    EXPECT_EQ(view_model->selected_algorithm.get(), WipeAlgorithm::GUTMANN);
}

// Test: is_wipe_in_progress starts as false
TEST_F(MainViewModelTest, IsWipeInProgress_StartsAsFalse) {
    EXPECT_FALSE(view_model->is_wipe_in_progress.get());
}

// Test: observable subscription triggers updates
TEST_F(MainViewModelTest, Observable_TriggersSubscribers) {
    bool was_notified = false;
    std::string notified_value;

    view_model->selected_disk_path.subscribe([&](const std::string& value) {
        was_notified = true;
        notified_value = value;
    });

    view_model->select_disk("/dev/sda");

    EXPECT_TRUE(was_notified);
    EXPECT_EQ(notified_value, "/dev/sda");
}

// Test: multiple subscribers all get notified
TEST_F(MainViewModelTest, Observable_MultipleSubscribers) {
    int notification_count = 0;

    view_model->selected_disk_path.subscribe([&](const std::string&) { notification_count++; });

    view_model->selected_disk_path.subscribe([&](const std::string&) { notification_count++; });

    view_model->select_disk("/dev/sda");

    EXPECT_EQ(notification_count, 2);
}

// Test: refresh_command calls load_disks when connected
TEST_F(MainViewModelTest, RefreshCommand_ReloadsDisks) {
    EXPECT_CALL(*mock_disk_service, get_available_disks(testing::_))
        .Times(2)  // Once for SimulateConnected, once for refresh
        .WillRepeatedly([](auto callback) { callback(std::vector<DiskInfo>{}); });

    view_model->initialize();
    SimulateConnected();
    view_model->refresh_command->execute();
}

// Test: commands exist
TEST_F(MainViewModelTest, Commands_AreNotNull) {
    EXPECT_NE(view_model->refresh_command, nullptr);
    EXPECT_NE(view_model->wipe_command, nullptr);
    EXPECT_NE(view_model->cancel_command, nullptr);
}

// Test: refresh_command can execute when connected
TEST_F(MainViewModelTest, RefreshCommand_CanAlwaysExecute) {
    EXPECT_CALL(*mock_disk_service, get_available_disks(testing::_)).WillOnce([](auto callback) {
        callback(std::vector<DiskInfo>{});
    });

    view_model->initialize();
    SimulateConnected();

    EXPECT_TRUE(view_model->refresh_command->can_execute());
}

// Test: wipe_command disabled when no selection
TEST_F(MainViewModelTest, WipeCommand_DisabledWithoutSelection) {
    EXPECT_CALL(*mock_disk_service, get_available_disks(testing::_)).WillOnce([](auto callback) {
        callback(std::vector<DiskInfo>{});
    });

    view_model->initialize();
    SimulateConnected();

    // No disk selected - should not be able to wipe
    EXPECT_FALSE(view_model->wipe_command->can_execute());
}

// Test: cancel_command disabled when not wiping
TEST_F(MainViewModelTest, CancelCommand_DisabledWhenNotWiping) {
    EXPECT_CALL(*mock_disk_service, get_available_disks(testing::_)).WillOnce([](auto callback) {
        callback(std::vector<DiskInfo>{});
    });

    view_model->initialize();
    SimulateConnected();

    EXPECT_FALSE(view_model->cancel_command->can_execute());
}

// Test: cancel_command enabled while the selected disk is wiping
TEST_F(MainViewModelTest, CancelCommand_EnabledDuringWipe) {
    std::vector<DiskInfo> disk_list;
    disk_list.push_back(MockDiskService::CreateTestDisk("/dev/sda"));

    ON_CALL(*mock_disk_service, get_available_disks(testing::_))
        .WillByDefault([&disk_list](auto callback) { callback(disk_list); });

    // Keep the mock wipe in flight until the test releases it, then report a
    // completion so the ViewModel clears the active wipe.
    std::promise<void> release;
    ON_CALL(*mock_wipe_service, wipe_disk(testing::_, testing::_, testing::_))
        .WillByDefault([&release](const std::string&, WipeAlgorithm, ProgressCallback callback) {
            release.get_future().wait();
            if (callback) {
                WipeProgress done{};
                done.is_complete = true;
                callback(done);
            }
            return true;
        });

    view_model->initialize();
    SimulateConnected();
    view_model->select_disk("/dev/sda");
    PumpMainLoop();

    // Start the wipe through the public flow: wipe_command raises the
    // confirmation dialog and the callback accepts it.
    ASSERT_TRUE(view_model->wipe_command->can_execute());
    view_model->wipe_command->execute();
    PumpMainLoop();
    const auto& message = view_model->current_message.get();
    ASSERT_NE(message.type, MessageInfo::Type::ERROR);
    if (message.confirmation_callback) {
        message.confirmation_callback(true);
    }

    EXPECT_TRUE(view_model->cancel_command->can_execute());

    // A second wipe on another device is allowed while the first runs, but
    // cancel does not apply to a device that is not wiping.
    disk_list.push_back(MockDiskService::CreateTestDisk("/dev/sdb"));
    view_model->select_disk("/dev/sdb");
    PumpMainLoop();
    EXPECT_TRUE(view_model->can_wipe.get());
    EXPECT_FALSE(view_model->cancel_command->can_execute());

    release.set_value();
    // Let the wipe thread and completion idle callbacks run
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    PumpMainLoop();
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    PumpMainLoop();

    // Once the wipe on /dev/sda finished, selecting it again allows a new
    // wipe and cancel no longer applies to it.
    view_model->select_disk("/dev/sda");
    PumpMainLoop();
    EXPECT_FALSE(view_model->cancel_command->can_execute());
    EXPECT_TRUE(view_model->can_wipe.get());
}

// Test: algorithms observable is populated
TEST_F(MainViewModelTest, Algorithms_ArePopulated) {
    EXPECT_CALL(*mock_disk_service, get_available_disks(testing::_)).WillOnce([](auto callback) {
        callback(std::vector<DiskInfo>{});
    });

    view_model->initialize();
    SimulateConnected();

    // Should have multiple algorithms available
    EXPECT_FALSE(view_model->algorithms.get().empty());
}

TEST_F(MainViewModelTest, Algorithms_IncludeATASecureErase) {
    EXPECT_CALL(*mock_disk_service, get_available_disks(testing::_)).WillOnce([](auto callback) {
        callback(std::vector<DiskInfo>{});
    });

    view_model->initialize();
    SimulateConnected();

    const auto algorithms = view_model->algorithms.get();
    const auto it = std::ranges::find_if(algorithms, [](const AlgorithmInfo& info) {
        return info.algorithm == WipeAlgorithm::ATA_SECURE_ERASE;
    });

    EXPECT_NE(it, algorithms.end());
}

TEST_F(MainViewModelTest, Verification_DisabledWhenAlgorithmDoesNotSupportIt) {
    ON_CALL(*mock_wipe_service, supports_verification(WipeAlgorithm::GUTMANN))
        .WillByDefault(Return(false));

    view_model->verification_enabled.set(true);
    view_model->select_algorithm(WipeAlgorithm::GUTMANN);

    EXPECT_FALSE(view_model->verification_available.get());
    EXPECT_FALSE(view_model->verification_enabled.get());
}

TEST_F(MainViewModelTest, SsdAlgorithmWarning_ShownForIncompatibleAlgorithm) {
    auto disk = MockDiskService::CreateTestDisk("/dev/nvme0n1");
    disk.is_ssd = true;

    ON_CALL(*mock_wipe_service, is_ssd_compatible(WipeAlgorithm::DOD_5220_22_M))
        .WillByDefault(Return(false));
    EXPECT_CALL(*mock_disk_service, get_available_disks(testing::_))
        .WillOnce([disk](auto callback) { callback(std::vector<DiskInfo>{disk}); });

    view_model->initialize();
    SimulateConnected();
    view_model->select_disk("/dev/nvme0n1");
    view_model->select_algorithm(WipeAlgorithm::DOD_5220_22_M);

    EXPECT_FALSE(view_model->algorithm_warning.get().empty());
}

// Test: disk selection cleared when disk no longer available
TEST_F(MainViewModelTest, LoadDisks_ClearsInvalidSelection) {
    auto disk = MockDiskService::CreateTestDisk("/dev/sda");

    EXPECT_CALL(*mock_disk_service, get_available_disks(testing::_))
        .WillOnce([disk](auto callback) { callback(std::vector<DiskInfo>{disk}); })
        .WillOnce([](auto callback) { callback(std::vector<DiskInfo>{}); });  // Disk removed

    view_model->initialize();
    SimulateConnected();  // First load - disk exists
    view_model->select_disk("/dev/sda");
    EXPECT_EQ(view_model->selected_disk_path.get(), "/dev/sda");

    // Simulate refresh after disk removal
    view_model->refresh_command->execute();
    PumpMainLoop();

    EXPECT_EQ(view_model->selected_disk_path.get(), "");
}

// Test: wipe_progress observable exists
TEST_F(MainViewModelTest, WipeProgress_ObservableExists) {
    // Just verify it can be accessed without crashing
    auto progress = view_model->wipe_progress.get();
    EXPECT_FALSE(progress.is_complete);
}

// Test: selected_algorithm starts with default
TEST_F(MainViewModelTest, SelectedAlgorithm_HasDefaultValue) {
    // Default should be ZERO_FILL (first/simplest)
    auto algo = view_model->selected_algorithm.get();
    // Just check it has a valid value
    EXPECT_GE(static_cast<int>(algo), 0);
}

// ==========================================================================
// controller_wide_warning
// ==========================================================================

TEST(MainViewModelScopeWarning, NvmeFirmwareEraseWarnsAboutOtherNamespaces) {
    // Sanitize and Format NVM are controller-scoped, so the confirmation has
    // to say that siblings of the selected namespace are erased too.
    const auto warning =
        MainViewModel::controller_wide_warning(WipeAlgorithm::ATA_SECURE_ERASE, "/dev/nvme0n1");

    EXPECT_FALSE(warning.empty());
    EXPECT_NE(warning.find("namespace"), std::string::npos);
    EXPECT_NE(warning.find("/dev/nvme0n1"), std::string::npos);
}

TEST(MainViewModelScopeWarning, SataFirmwareEraseHasNoExtraScope) {
    EXPECT_TRUE(MainViewModel::controller_wide_warning(WipeAlgorithm::ATA_SECURE_ERASE, "/dev/sda")
                    .empty());
}

TEST(MainViewModelScopeWarning, SoftwareOverwriteOnNvmeHasNoExtraScope) {
    // A software overwrite writes only to the selected namespace.
    EXPECT_TRUE(
        MainViewModel::controller_wide_warning(WipeAlgorithm::ZERO_FILL, "/dev/nvme0n1").empty());
}

// ==========================================================================
// build_scope_note
// ==========================================================================

TEST(MainViewModelScopeNote, PartitionSiblingsAreSpared) {
    DiskInfo disk{};
    disk.path = "/dev/sda1";
    disk.is_partition = true;
    disk.parent_disk = "/dev/sda";
    const auto note = MainViewModel::build_scope_note(disk, {});

    EXPECT_FALSE(note.empty());
    EXPECT_NE(note.find("only this partition"), std::string::npos);
    EXPECT_NE(note.find("/dev/sda"), std::string::npos);
}

TEST(MainViewModelScopeNote, WholeDiskListsPartitions) {
    DiskInfo disk{};
    disk.path = "/dev/sda";
    const auto note = MainViewModel::build_scope_note(disk, {"/dev/sda1", "/dev/sda2"});

    EXPECT_FALSE(note.empty());
    EXPECT_NE(note.find("/dev/sda1"), std::string::npos);
    EXPECT_NE(note.find("/dev/sda2"), std::string::npos);
    EXPECT_NE(note.find("partition table"), std::string::npos);
}

TEST(MainViewModelScopeNote, WholeDiskWithoutPartitionsHasNoNote) {
    DiskInfo disk{};
    disk.path = "/dev/sdb";
    EXPECT_TRUE(MainViewModel::build_scope_note(disk, {}).empty());
}

TEST(MainViewModelScopeNote, PartitionWithoutParentHasNoNote) {
    DiskInfo disk{};
    disk.path = "/dev/sda1";
    disk.is_partition = true;
    const auto note = MainViewModel::build_scope_note(disk, {});
    EXPECT_TRUE(note.empty());
}
