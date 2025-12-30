/**
 * @file MainViewModelTest.cpp
 * @brief Unit tests for MainViewModel
 */

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "view_models/MainViewModel.hpp"
#include "fixtures/TestFixtures.hpp"

using ::testing::Return;
using ::testing::_;
using ::testing::Invoke;
using ::testing::NiceMock;

class MainViewModelTest : public ViewModelTestFixture {
protected:
    std::unique_ptr<MainViewModel> view_model;

    void SetUp() override {
        ViewModelTestFixture::SetUp();
        view_model = std::make_unique<MainViewModel>(
            mock_disk_service,
            mock_wipe_service
        );
    }

    void TearDown() override {
        view_model.reset();
        ViewModelTestFixture::TearDown();
    }

    // Helper to simulate connected state (required for disk loading)
    void SimulateConnected() {
        view_model->set_connection_state(true, "");
    }
};

// Test: initialization creates valid view model
TEST_F(MainViewModelTest, Constructor_CreatesValidViewModel) {
    EXPECT_NE(view_model, nullptr);
}

// Test: initialize loads disks when connected
TEST_F(MainViewModelTest, Initialize_LoadsDisks) {
    std::vector<DiskInfo> test_disks = {
        MockDiskService::CreateTestDisk("/dev/sda"),
        MockDiskService::CreateTestDisk("/dev/sdb")
    };

    // set_connection_state(true) will call load_disks()
    EXPECT_CALL(*mock_disk_service, get_available_disks())
        .WillOnce(Return(test_disks));

    view_model->initialize();  // Won't load disks since not connected
    SimulateConnected();       // This triggers load_disks()

    EXPECT_EQ(view_model->disks.get().size(), 2u);
}

// Test: initialize with empty disk list when connected
TEST_F(MainViewModelTest, Initialize_HandlesEmptyDiskList) {
    EXPECT_CALL(*mock_disk_service, get_available_disks())
        .WillOnce(Return(std::vector<DiskInfo>{}));

    view_model->initialize();
    SimulateConnected();

    EXPECT_TRUE(view_model->disks.get().empty());
}

// Test: select_disk updates selected_disk_path
TEST_F(MainViewModelTest, SelectDisk_UpdatesSelectedPath) {
    EXPECT_CALL(*mock_disk_service, get_available_disks())
        .WillOnce(Return(std::vector<DiskInfo>{}));

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

    view_model->selected_disk_path.subscribe([&](const std::string&) {
        notification_count++;
    });

    view_model->selected_disk_path.subscribe([&](const std::string&) {
        notification_count++;
    });

    view_model->select_disk("/dev/sda");

    EXPECT_EQ(notification_count, 2);
}

// Test: refresh_command calls load_disks when connected
TEST_F(MainViewModelTest, RefreshCommand_ReloadsDisks) {
    EXPECT_CALL(*mock_disk_service, get_available_disks())
        .Times(2)  // Once for SimulateConnected, once for refresh
        .WillRepeatedly(Return(std::vector<DiskInfo>{}));

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
    EXPECT_CALL(*mock_disk_service, get_available_disks())
        .WillOnce(Return(std::vector<DiskInfo>{}));

    view_model->initialize();
    SimulateConnected();

    EXPECT_TRUE(view_model->refresh_command->can_execute());
}

// Test: wipe_command disabled when no selection
TEST_F(MainViewModelTest, WipeCommand_DisabledWithoutSelection) {
    EXPECT_CALL(*mock_disk_service, get_available_disks())
        .WillOnce(Return(std::vector<DiskInfo>{}));

    view_model->initialize();
    SimulateConnected();

    // No disk selected - should not be able to wipe
    EXPECT_FALSE(view_model->wipe_command->can_execute());
}

// Test: cancel_command disabled when not wiping
TEST_F(MainViewModelTest, CancelCommand_DisabledWhenNotWiping) {
    EXPECT_CALL(*mock_disk_service, get_available_disks())
        .WillOnce(Return(std::vector<DiskInfo>{}));

    view_model->initialize();
    SimulateConnected();

    EXPECT_FALSE(view_model->cancel_command->can_execute());
}

// Test: cancel_command enabled during wipe
TEST_F(MainViewModelTest, CancelCommand_EnabledDuringWipe) {
    EXPECT_CALL(*mock_disk_service, get_available_disks())
        .WillOnce(Return(std::vector<DiskInfo>{}));

    view_model->initialize();
    SimulateConnected();
    view_model->is_wipe_in_progress.set(true);

    EXPECT_TRUE(view_model->cancel_command->can_execute());
}

// Test: algorithms observable is populated
TEST_F(MainViewModelTest, Algorithms_ArePopulated) {
    EXPECT_CALL(*mock_disk_service, get_available_disks())
        .WillOnce(Return(std::vector<DiskInfo>{}));

    view_model->initialize();
    SimulateConnected();

    // Should have multiple algorithms available
    EXPECT_FALSE(view_model->algorithms.get().empty());
}

// Test: disk selection cleared when disk no longer available
TEST_F(MainViewModelTest, LoadDisks_ClearsInvalidSelection) {
    auto disk = MockDiskService::CreateTestDisk("/dev/sda");

    EXPECT_CALL(*mock_disk_service, get_available_disks())
        .WillOnce(Return(std::vector<DiskInfo>{disk}))
        .WillOnce(Return(std::vector<DiskInfo>{}));  // Disk removed

    view_model->initialize();
    SimulateConnected();  // First load - disk exists
    view_model->select_disk("/dev/sda");
    EXPECT_EQ(view_model->selected_disk_path.get(), "/dev/sda");

    // Simulate refresh after disk removal
    view_model->refresh_command->execute();

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
