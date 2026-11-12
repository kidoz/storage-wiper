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
#include <filesystem>
#include <fstream>
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
        ON_CALL(*mock_wipe_service, prepare_wipe(_, _, _))
            .WillByDefault([](const std::string& path, WipeAlgorithm algorithm,
                              bool verify) -> std::expected<WipePlan, util::Error> {
                WipePlan plan;
                plan.id = "test-plan-" + path;
                plan.device.disk = MockDiskService::CreateTestDisk(path);
                plan.device.device_number = 2'048;
                plan.device.disk_sequence = 17;
                plan.algorithm = algorithm;
                plan.verify = verify;
                plan.firmware = {"Software overwrite", "Selected device", true};
                plan.targets = {plan.device};
                return plan;
            });
        // Existing progress tests share their fake execution through this adapter.
        ON_CALL(*mock_wipe_service, start_prepared_wipe(_, _))
            .WillByDefault([service = mock_wipe_service.get()](const WipePlan& plan,
                                                               ProgressCallback callback) {
                return service->wipe_disk(plan.device.disk.path, plan.algorithm,
                                          std::move(callback), plan.verify);
            });
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

    void PrepareAndConfirm() {
        const auto path = view_model->selected_disk_path.get();
        auto current_disks = view_model->disks.get();
        if (std::ranges::none_of(current_disks,
                                 [&](const auto& disk) { return disk.path == path; })) {
            current_disks.push_back(MockDiskService::CreateTestDisk(path));
            view_model->disks.set(current_disks);
        }
        view_model->is_connected.set(true);
        view_model->current_message.set({});
        view_model->wipe_command->execute();
        ASSERT_TRUE(wait_for([&] {
            return view_model->current_message.get().type == MessageInfo::Type::CONFIRMATION;
        }));
        view_model->confirm_wipe();
    }

    auto wait_for(const std::function<bool()>& predicate) -> bool {
        return ThreadingTestHelper::WaitUntil([&] {
            PumpMainLoop();
            return predicate();
        });
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
    ON_CALL(*mock_wipe_service, wipe_disk(testing::_, testing::_, testing::_, testing::_))
        .WillByDefault(
            [&release](const std::string&, WipeAlgorithm, ProgressCallback callback, bool) {
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
    ASSERT_TRUE(wait_for(
        [&] { return view_model->current_message.get().type == MessageInfo::Type::CONFIRMATION; }));
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

TEST_F(MainViewModelTest, SavesPresetForEveryParallelStartUsingConfirmedSettings) {
    EXPECT_CALL(*mock_wipe_service, wipe_disk(_, _, _, _)).Times(2).WillRepeatedly(Return(true));
    ON_CALL(*mock_wipe_service, supports_verification(_)).WillByDefault(Return(true));
    std::vector<util::AppSettingsData> saved;
    view_model->set_settings_save_callback(
        [&](const auto& settings) { saved.push_back(settings); });
    view_model->select_disk("/dev/sda");
    view_model->select_algorithm(WipeAlgorithm::ZERO_FILL);
    PrepareAndConfirm();
    ASSERT_TRUE(wait_for([&] { return saved.size() == 1; }));
    EXPECT_TRUE(view_model->is_wipe_in_progress.get());

    view_model->select_disk("/dev/sdb");
    view_model->select_algorithm(WipeAlgorithm::DOD_5220_22_M);
    view_model->verification_enabled.set(true);
    PrepareAndConfirm();
    // The selection can change while authorization/start is still in flight.
    view_model->select_algorithm(WipeAlgorithm::GUTMANN);
    ASSERT_TRUE(wait_for([&] { return saved.size() == 2; }));
    EXPECT_EQ(saved[0].algorithm_id, static_cast<int>(WipeAlgorithm::ZERO_FILL));
    EXPECT_EQ(saved[1].algorithm_id, static_cast<int>(WipeAlgorithm::DOD_5220_22_M));
    EXPECT_TRUE(saved[1].verification_enabled);
}

TEST_F(MainViewModelTest, RejectedStartDoesNotSavePreset) {
    int saves = 0;
    view_model->set_settings_save_callback([&](const auto&) { ++saves; });
    ON_CALL(*mock_wipe_service, wipe_disk(_, _, _, _)).WillByDefault(Return(false));
    view_model->select_disk("/dev/sda");
    PrepareAndConfirm();
    ASSERT_TRUE(wait_for([&] { return !view_model->is_wipe_in_progress.get(); }));
    EXPECT_EQ(saves, 0);
}

TEST_F(MainViewModelTest, HardwareEraseOfPartitionIsRejectedBeforeConfirmation) {
    auto partition = MockDiskService::CreateTestDisk("/dev/nvme999n1p1");
    partition.is_partition = true;
    partition.parent_disk = "/dev/nvme999n1";
    SimulateConnected();
    view_model->disks.set({partition});
    view_model->select_disk(partition.path);
    view_model->select_algorithm(WipeAlgorithm::ATA_SECURE_ERASE);
    EXPECT_CALL(*mock_wipe_service, wipe_disk(_, _, _, _)).Times(0);
    view_model->wipe_command->execute();
    EXPECT_EQ(view_model->current_message.get().type, MessageInfo::Type::ERROR);
    EXPECT_NE(view_model->current_message.get().message.find("partition"), std::string::npos);
}

TEST_F(MainViewModelTest, CertificateRetainsMultiPassCount) {
    TempTestFile root;
    ASSERT_TRUE(root.valid());
    const auto directory = root.path() + "-certificates";
    struct Cleanup {
        std::string path;
        ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{directory};
    view_model->set_certificate_directory(directory);
    view_model->disks.set({MockDiskService::CreateTestDisk("/dev/sda")});
    view_model->select_disk("/dev/sda");
    view_model->select_algorithm(WipeAlgorithm::VSITR);
    ON_CALL(*mock_wipe_service, get_pass_count(WipeAlgorithm::VSITR)).WillByDefault(Return(7));
    EXPECT_CALL(*mock_wipe_service, wipe_disk(_, WipeAlgorithm::VSITR, _, _))
        .WillOnce([](const auto&, auto, auto callback, bool) {
            WipeProgress complete{};
            complete.is_complete = true;
            complete.total_passes = 0;
            callback(complete);
            return true;
        });
    PrepareAndConfirm();
    ASSERT_TRUE(wait_for([&] { return !view_model->is_wipe_in_progress.get(); }));
    int certificates = 0;
    for (const auto& file : std::filesystem::directory_iterator(directory)) {
        if (file.path().extension() == ".json") {
            ++certificates;
            std::ifstream input(file.path());
            std::string json{std::istreambuf_iterator<char>{input}, {}};
            EXPECT_NE(json.find("\"passes\": 7"), std::string::npos);
            EXPECT_NE(json.find("\"bad_block_count\": 0"), std::string::npos);
        }
    }
    EXPECT_EQ(certificates, 1);
}

TEST_F(MainViewModelTest, BadSectorsCannotProduceSuccessCertificateWithInconsistentProgress) {
    TempTestFile root;
    ASSERT_TRUE(root.valid());
    const auto directory = root.path() + "-bad-sector-certificates";
    struct Cleanup {
        std::string path;
        ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{directory};
    view_model->set_certificate_directory(directory);
    view_model->disks.set({MockDiskService::CreateTestDisk("/dev/sda")});
    view_model->select_disk("/dev/sda");
    EXPECT_CALL(*mock_wipe_service, wipe_disk(_, _, _, _))
        .WillOnce([](const auto&, auto, auto callback, bool) {
            WipeProgress complete{};
            complete.is_complete = true;
            complete.bad_block_count = 4;
            callback(complete);
            return true;
        });
    PrepareAndConfirm();
    ASSERT_TRUE(wait_for([&] { return !view_model->is_wipe_in_progress.get(); }));
    EXPECT_FALSE(std::filesystem::exists(directory));
    EXPECT_EQ(view_model->current_message.get().type, MessageInfo::Type::ERROR);
}

TEST_F(MainViewModelTest, DisconnectFailureClearsActiveWipeWithoutWritingCertificate) {
    TempTestFile root;
    ASSERT_TRUE(root.valid());
    const auto directory = root.path() + "-failed-certificates";
    struct Cleanup {
        std::string path;
        ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{directory};
    view_model->set_certificate_directory(directory);
    SimulateConnected();
    view_model->select_disk("/dev/sda");
    EXPECT_CALL(*mock_wipe_service, wipe_disk(_, _, _, _))
        .WillOnce([](const auto&, auto, auto callback, bool) {
            WipeProgress failure{};
            failure.is_complete = true;
            failure.has_error = true;
            failure.error_message = "Helper service stopped; wipe outcome is unknown.";
            callback(failure);
            return true;
        });
    PrepareAndConfirm();
    ASSERT_TRUE(wait_for([&] { return !view_model->is_wipe_in_progress.get(); }));
    EXPECT_TRUE(view_model->wipe_progresses.get().empty());
    EXPECT_FALSE(std::filesystem::exists(directory));
    EXPECT_EQ(view_model->current_message.get().type, MessageInfo::Type::ERROR);
    view_model->select_disk("/dev/sda");
    EXPECT_TRUE(view_model->can_wipe.get());
}

TEST_F(MainViewModelTest, ForwardsVerificationToWipeService) {
    std::promise<bool> requested;
    auto result = requested.get_future();
    EXPECT_CALL(*mock_wipe_service, wipe_disk("/dev/sda", WipeAlgorithm::ZERO_FILL, _, true))
        .WillOnce([&](const auto&, auto, auto, bool verify) {
            requested.set_value(verify);
            return true;
        });
    view_model->select_disk("/dev/sda");
    view_model->verification_available.set(true);
    view_model->verification_enabled.set(true);
    PrepareAndConfirm();
    ASSERT_EQ(result.wait_for(std::chrono::seconds{2}), std::future_status::ready);
    EXPECT_TRUE(result.get());
}

TEST_F(MainViewModelTest, DuplicateConfirmationPreservesRunningWipeAndCompletion) {
    ON_CALL(*mock_disk_service, get_available_disks(_)).WillByDefault([](auto callback) {
        callback(std::vector<DiskInfo>{MockDiskService::CreateTestDisk("/dev/sda")});
    });
    std::promise<ProgressCallback> started;
    auto result = started.get_future();
    EXPECT_CALL(*mock_wipe_service, wipe_disk("/dev/sda", _, _, _))
        .Times(1)
        .WillOnce([&](const auto&, auto, auto callback, bool) {
            started.set_value(std::move(callback));
            return true;
        });
    view_model->select_disk("/dev/sda");
    PrepareAndConfirm();
    ASSERT_EQ(result.wait_for(std::chrono::seconds{2}), std::future_status::ready);
    auto progress_callback = result.get();
    view_model->confirm_wipe();
    EXPECT_TRUE(view_model->is_wipe_in_progress.get());
    WipeProgress complete{};
    complete.is_complete = true;
    complete.percentage = 100.0;
    progress_callback(complete);
    ASSERT_TRUE(wait_for([&] { return !view_model->is_wipe_in_progress.get(); }));
    EXPECT_TRUE(view_model->wipe_progress.get().is_complete);
}

TEST_F(MainViewModelTest, PreparationFailureNeverOffersConfirmationOrStartsWipe) {
    SimulateConnected();
    auto disk = MockDiskService::CreateTestDisk("/dev/sda");
    view_model->disks.set({disk});
    view_model->select_disk(disk.path);
    EXPECT_CALL(*mock_wipe_service, prepare_wipe(_, _, _))
        .WillOnce(Return(std::unexpected(util::Error{"Device identity unavailable"})));
    EXPECT_CALL(*mock_wipe_service, start_prepared_wipe(_, _)).Times(0);
    view_model->wipe_command->execute();
    ASSERT_TRUE(wait_for([&] { return !view_model->is_operation_pending.get(); }));
    EXPECT_EQ(view_model->current_message.get().type, MessageInfo::Type::ERROR);
    EXPECT_NE(view_model->current_message.get().message.find("identity"), std::string::npos);
    view_model->confirm_wipe();
}

TEST_F(MainViewModelTest, ConfirmationUsesPreparedScopeAndCannotFollowSelectionChanges) {
    SimulateConnected();
    WipePlan plan;
    plan.id = "confirmed-plan";
    plan.device.disk = MockDiskService::CreateTestDisk("/dev/nvme0n1");
    plan.device.disk.model = "Prepared model";
    plan.device.disk.serial = "ORIGINAL-SERIAL";
    plan.device.disk_sequence = 101;
    plan.algorithm = WipeAlgorithm::ATA_SECURE_ERASE;
    plan.firmware = {"nvme-format-crypto", "device", false};
    plan.targets = {plan.device};
    view_model->disks.set({plan.device.disk});
    view_model->select_disk(plan.device.disk.path);
    view_model->select_algorithm(plan.algorithm);
    EXPECT_CALL(*mock_wipe_service, prepare_wipe(plan.device.disk.path, plan.algorithm, false))
        .WillOnce(Return(plan));
    std::promise<WipePlan> started;
    auto result = started.get_future();
    EXPECT_CALL(*mock_wipe_service, start_prepared_wipe(_, _))
        .WillOnce([&](const WipePlan& confirmed, auto) {
            started.set_value(confirmed);
            return true;
        });
    view_model->wipe_command->execute();
    ASSERT_TRUE(wait_for(
        [&] { return view_model->current_message.get().type == MessageInfo::Type::CONFIRMATION; }));
    const auto message = view_model->current_message.get();
    EXPECT_NE(message.message.find("NVMe Format NVM (cryptographic erase)"), std::string::npos);
    EXPECT_NE(message.message.find("Only the selected device"), std::string::npos);
    EXPECT_NE(message.message.find("ORIGINAL-SERIAL"), std::string::npos);
    EXPECT_EQ(message.message.find("EVERY namespace"), std::string::npos);
    view_model->select_disk("/dev/sdb");
    view_model->select_algorithm(WipeAlgorithm::ZERO_FILL);
    message.confirmation_callback(true);
    ASSERT_EQ(result.wait_for(std::chrono::seconds{2}), std::future_status::ready);
    const auto accepted = result.get();
    EXPECT_EQ(accepted.id, plan.id);
    EXPECT_EQ(accepted.device, plan.device);
    EXPECT_EQ(accepted.algorithm, plan.algorithm);
    EXPECT_EQ(accepted.firmware, plan.firmware);
}

TEST_F(MainViewModelTest, RejectedConfirmationDiscardsPreparedPlan) {
    SimulateConnected();
    auto disk = MockDiskService::CreateTestDisk("/dev/sda");
    view_model->disks.set({disk});
    view_model->select_disk(disk.path);
    std::promise<std::string> discarded;
    auto result = discarded.get_future();
    EXPECT_CALL(*mock_wipe_service, start_prepared_wipe(_, _)).Times(0);
    EXPECT_CALL(*mock_wipe_service, discard_wipe_plan(_)).WillOnce([&](const auto& id) {
        discarded.set_value(id);
    });
    view_model->wipe_command->execute();
    ASSERT_TRUE(wait_for(
        [&] { return view_model->current_message.get().type == MessageInfo::Type::CONFIRMATION; }));
    view_model->current_message.get().confirmation_callback(false);
    ASSERT_EQ(result.wait_for(std::chrono::seconds{2}), std::future_status::ready);
    EXPECT_EQ(result.get(), "test-plan-/dev/sda");
    EXPECT_FALSE(view_model->is_operation_pending.get());
}

TEST_F(MainViewModelTest, CertificateUsesConfirmedSnapshotAfterDeviceEnumerationChanges) {
    TempTestFile root;
    ASSERT_TRUE(root.valid());
    const auto directory = root.path() + "-snapshot-certificate";
    struct Cleanup {
        std::string path;
        ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{directory};
    view_model->set_certificate_directory(directory);
    WipePlan plan;
    plan.id = "snapshot-plan";
    plan.device.disk = MockDiskService::CreateTestDisk("/dev/sda");
    plan.device.disk.serial = "CONFIRMED-SERIAL";
    plan.device.disk_sequence = 123'456;
    plan.device.device_number = 2'048;
    plan.firmware = {"Software overwrite", "Selected device and its partitions", true};
    plan.targets = {plan.device};
    EXPECT_CALL(*mock_wipe_service, prepare_wipe(_, _, _)).WillOnce(Return(plan));
    std::promise<ProgressCallback> started;
    auto result = started.get_future();
    EXPECT_CALL(*mock_wipe_service, start_prepared_wipe(_, _))
        .WillOnce([&](const auto&, ProgressCallback callback) {
            started.set_value(std::move(callback));
            return true;
        });
    view_model->select_disk(plan.device.disk.path);
    PrepareAndConfirm();
    ASSERT_EQ(result.wait_for(std::chrono::seconds{2}), std::future_status::ready);
    auto replacement = plan.device.disk;
    replacement.serial = "REPLACEMENT-SERIAL";
    view_model->disks.set({replacement});
    WipeProgress complete{};
    complete.is_complete = true;
    result.get()(complete);
    ASSERT_TRUE(wait_for([&] { return !view_model->is_wipe_in_progress.get(); }));
    int certificates = 0;
    for (const auto& file : std::filesystem::directory_iterator(directory)) {
        if (file.path().extension() == ".json") {
            ++certificates;
            std::ifstream input(file.path());
            const std::string json{std::istreambuf_iterator<char>{input}, {}};
            EXPECT_NE(json.find("CONFIRMED-SERIAL"), std::string::npos);
            EXPECT_EQ(json.find("REPLACEMENT-SERIAL"), std::string::npos);
            EXPECT_NE(json.find("123456"), std::string::npos);
            EXPECT_NE(json.find(plan.firmware.scope), std::string::npos);
            EXPECT_NE(json.find(plan.firmware.method), std::string::npos);
        }
    }
    EXPECT_EQ(certificates, 1);
}

TEST_F(MainViewModelTest, UnmountConfirmationPreservesSettingsBeforePreparingFinalPlan) {
    SimulateConnected();
    auto disk = MockDiskService::CreateTestDisk("/dev/sda");
    disk.is_mounted = true;
    view_model->disks.set({disk});
    view_model->select_disk(disk.path);
    ON_CALL(*mock_wipe_service, supports_verification(_)).WillByDefault(Return(true));
    view_model->select_algorithm(WipeAlgorithm::DOD_5220_22_M);
    view_model->verification_enabled.set(true);
    EXPECT_CALL(*mock_disk_service, unmount_disk(disk.path))
        .WillOnce(Return(std::expected<void, util::Error>{}));
    EXPECT_CALL(*mock_wipe_service, prepare_wipe(disk.path, WipeAlgorithm::DOD_5220_22_M, true));
    EXPECT_CALL(*mock_wipe_service, start_prepared_wipe(_, _)).Times(0);
    view_model->wipe_command->execute();
    const auto unmount_message = view_model->current_message.get();
    ASSERT_EQ(unmount_message.title, "Unmount and Wipe?");
    view_model->select_disk("/dev/sdb");
    view_model->select_algorithm(WipeAlgorithm::GUTMANN);
    unmount_message.confirmation_callback(true);
    ASSERT_TRUE(
        wait_for([&] { return view_model->current_message.get().title == "Confirm Disk Wipe"; }));
    const auto confirmation = view_model->current_message.get();
    EXPECT_NE(confirmation.message.find(disk.path), std::string::npos);
    EXPECT_EQ(confirmation.message.find("/dev/sdb"), std::string::npos);
    EXPECT_NE(confirmation.message.find("Verification: enabled"), std::string::npos);
    std::promise<void> discarded;
    auto result = discarded.get_future();
    EXPECT_CALL(*mock_wipe_service, discard_wipe_plan(_)).WillOnce([&](const auto&) {
        discarded.set_value();
    });
    confirmation.confirmation_callback(false);
    ASSERT_EQ(result.wait_for(std::chrono::seconds{2}), std::future_status::ready);
}

TEST_F(MainViewModelTest, CleanupDiscardsLatePreparedPlanWithoutOfferingConfirmation) {
    SimulateConnected();
    auto disk = MockDiskService::CreateTestDisk("/dev/sda");
    view_model->disks.set({disk});
    view_model->select_disk(disk.path);
    std::promise<void> preparing;
    auto entered = preparing.get_future();
    std::promise<void> release;
    auto allowed = release.get_future();
    EXPECT_CALL(*mock_wipe_service, prepare_wipe(_, _, _))
        .WillOnce([&](const auto&, auto, bool) -> std::expected<WipePlan, util::Error> {
            preparing.set_value();
            allowed.wait();
            WipePlan plan;
            plan.id = "late-plan";
            plan.device.disk = disk;
            return plan;
        });
    std::promise<void> discarded;
    auto result = discarded.get_future();
    EXPECT_CALL(*mock_wipe_service, discard_wipe_plan("late-plan")).WillOnce([&](const auto&) {
        discarded.set_value();
    });
    EXPECT_CALL(*mock_wipe_service, start_prepared_wipe(_, _)).Times(0);
    view_model->wipe_command->execute();
    ASSERT_EQ(entered.wait_for(std::chrono::seconds{2}), std::future_status::ready);
    view_model->cleanup();
    release.set_value();
    ASSERT_TRUE(wait_for([&] {
        return result.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready;
    }));
    EXPECT_NE(view_model->current_message.get().type, MessageInfo::Type::CONFIRMATION);
    view_model->confirm_wipe();
}

TEST_F(MainViewModelTest, CleanupDetachesWithoutImplicitCancellation) {
    view_model->select_disk("/dev/sda");
    std::promise<void> started;
    auto result = started.get_future();
    EXPECT_CALL(*mock_wipe_service, start_prepared_wipe(_, _)).WillOnce([&](const auto&, auto) {
        started.set_value();
        return true;
    });
    EXPECT_CALL(*mock_wipe_service, cancel_operation(_)).Times(0);
    EXPECT_CALL(*mock_wipe_service, cancel_prepared_wipe(_)).Times(0);
    std::promise<void> discarded;
    auto abandoned = discarded.get_future();
    EXPECT_CALL(*mock_wipe_service, discard_wipe_plan(_)).Times(1).WillOnce([&](const auto&) {
        discarded.set_value();
    });
    PrepareAndConfirm();
    ASSERT_EQ(result.wait_for(std::chrono::seconds{2}), std::future_status::ready);
    view_model->cleanup();
    view_model->cleanup();
    ASSERT_EQ(abandoned.wait_for(std::chrono::seconds{2}), std::future_status::ready);
    view_model.reset();
}

namespace {
auto reconnect_job(const std::string& id, bool complete = false) -> WipeJobSnapshot {
    WipeJobSnapshot job;
    job.plan.id = id;
    job.plan.device.disk = MockDiskService::CreateTestDisk("/dev/sda");
    job.plan.device.disk_sequence = 88;
    job.plan.firmware = {"overwrite", "device", true};
    job.plan.targets = {job.plan.device};
    job.progress.percentage = complete ? 100 : 35;
    job.progress.is_complete = complete;
    job.started_at_unix = 1'700'000'000;
    job.completed_at_unix = complete ? 1'700'000'060 : 0;
    return job;
}
}  // namespace

TEST_F(MainViewModelTest, RestoredCompletedJobWritesCertificateWithSnapshotPeakSpeed) {
    TempTestFile root;
    ASSERT_TRUE(root.valid());
    const auto directory = root.path() + "-restored-peak-certificate";
    struct Cleanup {
        std::string path;
        ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{directory};
    view_model->set_certificate_directory(directory);

    auto job = reconnect_job("restored-peak", true);
    job.peak_speed_bytes_per_sec = 123'456'789;
    EXPECT_CALL(*mock_wipe_service, get_wipe_jobs())
        .WillOnce(Return(std::vector<WipeJobSnapshot>{job}));
    SimulateConnected();

    ASSERT_TRUE(wait_for([&] {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
            if (entry.path().extension() == ".json")
                return true;
        }
        return false;
    }));
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() != ".json")
            continue;
        std::ifstream input(entry.path());
        const std::string json{std::istreambuf_iterator<char>{input}, {}};
        EXPECT_NE(json.find("123456789"), std::string::npos);
    }
}

TEST_F(MainViewModelTest, ReconnectHistoryDoesNotHideNewActiveJobOnSamePath) {
    const auto old_job = reconnect_job("old-completed", true);
    const auto live_job = reconnect_job("new-active");
    EXPECT_CALL(*mock_wipe_service, get_wipe_jobs())
        .WillOnce(Return(std::vector<WipeJobSnapshot>{old_job, live_job}));
    EXPECT_CALL(*mock_wipe_service,
                observe_wipe_job(testing::Field(&WipePlan::id, "new-active"), _))
        .WillOnce(Return(true));
    SimulateConnected();
    ASSERT_TRUE(wait_for([&] { return view_model->is_wipe_in_progress.get(); }));
    ASSERT_TRUE(wait_for([&] { return view_model->wipe_progresses.get().contains("/dev/sda"); }));
    EXPECT_EQ(view_model->wipe_progresses.get().at("/dev/sda").percentage, 35);
    EXPECT_TRUE(view_model->has_pending_or_active_operations());
}

TEST_F(MainViewModelTest, UnknownTransportOutcomeCanReattachSameJobAfterReconnect) {
    const auto job = reconnect_job("still-running");
    EXPECT_CALL(*mock_wipe_service, get_wipe_jobs())
        .Times(2)
        .WillRepeatedly(Return(std::vector<WipeJobSnapshot>{job}));
    std::vector<ProgressCallback> observations;
    EXPECT_CALL(*mock_wipe_service, observe_wipe_job(_, _))
        .Times(2)
        .WillRepeatedly([&](const auto&, auto callback) {
            observations.push_back(std::move(callback));
            return true;
        });
    SimulateConnected();
    ASSERT_TRUE(wait_for([&] { return observations.size() == 1; }));
    WipeProgress loss;
    loss.is_complete = true;
    loss.has_error = true;
    loss.outcome_known = false;
    loss.error_message = "Transport unavailable; outcome unknown";
    observations.front()(loss);
    ASSERT_TRUE(wait_for([&] { return !view_model->is_wipe_in_progress.get(); }));
    view_model->set_connection_state(false);
    view_model->set_connection_state(true);
    ASSERT_TRUE(wait_for([&] { return observations.size() == 2; }));
    EXPECT_TRUE(view_model->is_wipe_in_progress.get());
}

TEST_F(MainViewModelTest, StopForCloseCancelsByUuidAndWaitsForTerminalProgress) {
    const auto job = reconnect_job("cancel-this-uuid");
    EXPECT_CALL(*mock_wipe_service, get_wipe_jobs())
        .WillOnce(Return(std::vector<WipeJobSnapshot>{job}));
    ProgressCallback progress;
    EXPECT_CALL(*mock_wipe_service, observe_wipe_job(_, _))
        .WillOnce([&](const auto&, auto callback) {
            progress = std::move(callback);
            return true;
        });
    std::promise<void> cancelled;
    auto result = cancelled.get_future();
    EXPECT_CALL(*mock_wipe_service, cancel_operation(_)).Times(0);
    EXPECT_CALL(*mock_wipe_service, cancel_prepared_wipe(job.plan.id)).WillOnce([&](const auto&) {
        cancelled.set_value();
        return true;
    });
    SimulateConnected();
    ASSERT_TRUE(wait_for([&] { return bool(progress); }));
    view_model->stop_for_close();
    ASSERT_EQ(result.wait_for(std::chrono::seconds{2}), std::future_status::ready);
    EXPECT_TRUE(view_model->has_pending_or_active_operations());
    auto done = job.progress;
    done.is_complete = true;
    done.has_error = true;
    done.error_message = "Cancelled";
    progress(done);
    ASSERT_TRUE(wait_for([&] { return !view_model->has_pending_or_active_operations(); }));
}

TEST_F(MainViewModelTest, OldConnectionQueryCannotRestoreGhostJobsAfterReconnect) {
    const auto baseline_references = mock_wipe_service.use_count();
    std::promise<void> entered;
    auto querying = entered.get_future();
    std::promise<void> release;
    auto old_reply_allowed = release.get_future();
    const auto old_job = reconnect_job("old-helper-job");
    EXPECT_CALL(*mock_wipe_service, get_wipe_jobs())
        .WillOnce([&]() -> std::expected<std::vector<WipeJobSnapshot>, util::Error> {
            entered.set_value();
            old_reply_allowed.wait();
            return std::vector<WipeJobSnapshot>{old_job};
        })
        .WillOnce(Return(std::vector<WipeJobSnapshot>{}));
    EXPECT_CALL(*mock_wipe_service, observe_wipe_job(_, _)).Times(0);
    view_model->set_connection_state(true);
    ASSERT_EQ(querying.wait_for(std::chrono::seconds{2}), std::future_status::ready);
    view_model->set_connection_state(false);
    view_model->set_connection_state(true);
    release.set_value();
    // Worker and idle closures own service references until both replies are consumed.
    ASSERT_TRUE(wait_for([&] { return mock_wipe_service.use_count() == baseline_references; }));
    EXPECT_FALSE(view_model->is_wipe_in_progress.get());
}

TEST_F(MainViewModelTest, StopForCloseAbandonsPreparationBeforeFinalConfirmation) {
    SimulateConnected();
    const auto disk = MockDiskService::CreateTestDisk("/dev/sda");
    view_model->disks.set({disk});
    view_model->select_disk(disk.path);
    std::promise<void> entered;
    auto preparing = entered.get_future();
    std::promise<void> release;
    auto allowed = release.get_future();
    EXPECT_CALL(*mock_wipe_service, prepare_wipe(_, _, _))
        .WillOnce([&](const auto&, auto, bool) -> std::expected<WipePlan, util::Error> {
            entered.set_value();
            allowed.wait();
            auto job = reconnect_job("abandoned-preparation");
            return job.plan;
        });
    std::promise<void> discarded;
    auto result = discarded.get_future();
    EXPECT_CALL(*mock_wipe_service, discard_wipe_plan("abandoned-preparation"))
        .WillOnce([&](const auto&) { discarded.set_value(); });
    EXPECT_CALL(*mock_wipe_service, start_prepared_wipe(_, _)).Times(0);
    view_model->wipe_command->execute();
    ASSERT_EQ(preparing.wait_for(std::chrono::seconds{2}), std::future_status::ready);
    view_model->stop_for_close();
    EXPECT_TRUE(view_model->has_pending_or_active_operations());
    release.set_value();
    ASSERT_TRUE(wait_for([&] {
        return result.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready;
    }));
    EXPECT_FALSE(view_model->has_pending_or_active_operations());
    EXPECT_NE(view_model->current_message.get().type, MessageInfo::Type::CONFIRMATION);
}

TEST_F(MainViewModelTest, StopForCloseInvalidatesUnmountConfirmation) {
    SimulateConnected();
    auto disk = MockDiskService::CreateTestDisk("/dev/sda");
    disk.is_mounted = true;
    view_model->disks.set({disk});
    view_model->select_disk(disk.path);
    EXPECT_CALL(*mock_disk_service, unmount_disk(_)).Times(0);
    EXPECT_CALL(*mock_wipe_service, prepare_wipe(_, _, _)).Times(0);
    view_model->wipe_command->execute();
    const auto confirmation = view_model->current_message.get();
    ASSERT_TRUE(confirmation.confirmation_callback);
    EXPECT_TRUE(view_model->has_pending_or_active_operations());
    view_model->stop_for_close();
    confirmation.confirmation_callback(true);
    EXPECT_FALSE(view_model->has_pending_or_active_operations());
}

TEST_F(MainViewModelTest, StopForCloseWaitsForPendingStartToBeRejected) {
    view_model->select_disk("/dev/sda");
    std::promise<void> entered;
    auto starting = entered.get_future();
    std::promise<void> release;
    auto allowed = release.get_future();
    EXPECT_CALL(*mock_wipe_service, start_prepared_wipe(_, _)).WillOnce([&](const auto&, auto) {
        entered.set_value();
        allowed.wait();
        return false;
    });
    std::promise<void> discarded;
    auto abandoned = discarded.get_future();
    EXPECT_CALL(*mock_wipe_service, discard_wipe_plan(_))
        .WillOnce([&](const auto&) { discarded.set_value(); })
        .WillOnce([](const auto&) {});
    ON_CALL(*mock_wipe_service, cancel_prepared_wipe(_)).WillByDefault(Return(false));
    PrepareAndConfirm();
    ASSERT_EQ(starting.wait_for(std::chrono::seconds{2}), std::future_status::ready);
    view_model->stop_for_close();
    ASSERT_EQ(abandoned.wait_for(std::chrono::seconds{2}), std::future_status::ready);
    EXPECT_TRUE(view_model->has_pending_or_active_operations());
    release.set_value();
    ASSERT_TRUE(wait_for([&] { return !view_model->has_pending_or_active_operations(); }));
}
