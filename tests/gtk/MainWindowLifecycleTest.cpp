#include "mocks/MockDiskService.hpp"
#include "mocks/MockWipeService.hpp"
#include "views/MainWindow.hpp"

#include <gtest/gtest.h>

#include <gtkmm/init.h>

#include <chrono>
#include <future>
#include <thread>

using testing::_;
using testing::Return;

class MainWindowLifecycleTest : public testing::Test {
protected:
    inline static GtkApplication* app_ = nullptr;
    AdwApplicationWindow* window_ = nullptr;
    std::unique_ptr<MainWindow> view_;
    std::shared_ptr<MockDiskService> disks_;
    std::shared_ptr<MockWipeService> wipes_;
    std::shared_ptr<MainViewModel> vm_;
    bool destroyed_ = false;
    ProgressCallback progress_;

    static void SetUpTestSuite() {
        g_setenv("GTK_A11Y", "none", TRUE);
        g_setenv("GIO_USE_VFS", "local", TRUE);
        g_setenv("GSK_RENDERER", "cairo", TRUE);
        g_setenv("GDK_DISABLE", "gl,vulkan", TRUE);
        adw_init();
        Gtk::init_gtkmm_internals();
        app_ = gtk_application_new("su.kidoz.storage_wiper.gtk_test", G_APPLICATION_NON_UNIQUE);
        ASSERT_TRUE(g_application_register(G_APPLICATION(app_), nullptr, nullptr));
    }
    static void TearDownTestSuite() {
        g_object_unref(app_);
        app_ = nullptr;
    }
    void SetUp() override {
        window_ = ADW_APPLICATION_WINDOW(adw_application_window_new(app_));
        g_object_ref(window_);
        g_signal_connect(window_, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer data) {
                             *static_cast<bool*>(data) = true;
                         }),
                         &destroyed_);
        disks_ = std::make_shared<testing::NiceMock<MockDiskService>>();
        wipes_ = MockWipeService::CreateNiceMock();
        vm_ = std::make_shared<MainViewModel>(disks_, wipes_);
        view_ = std::make_unique<MainWindow>(window_);
        view_->setup_ui();
        view_->bind(vm_);
        gtk_window_present(GTK_WINDOW(window_));
        pump();
    }
    void TearDown() override {
        view_.reset();
        if (window_) {
            gtk_window_destroy(GTK_WINDOW(window_));
            g_object_unref(window_);
        }
        vm_.reset();
        ASSERT_TRUE(wait_for([this] { return wipes_.use_count() == 1; }));
        progress_ = {};
        pump();
    }
    auto closed() const -> bool {
        return !window_ || !gtk_window_get_application(GTK_WINDOW(window_));
    }
    static void pump() {
        while (g_main_context_pending(nullptr))
            g_main_context_iteration(nullptr, FALSE);
    }
    static auto wait_for(const std::function<bool()>& predicate) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
        do {
            pump();
            if (predicate())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }
    static auto find_button(GtkWidget* widget, const char* label) -> GtkWidget* {
        if (GTK_IS_BUTTON(widget) &&
            g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), label) == 0)
            return widget;
        for (auto* child = gtk_widget_get_first_child(widget); child;
             child = gtk_widget_get_next_sibling(child)) {
            if (auto* button = find_button(child, label))
                return button;
        }
        return nullptr;
    }
    static void respond(AdwAlertDialog* dialog, const char* label) {
        auto* button = find_button(GTK_WIDGET(dialog), label);
        ASSERT_NE(button, nullptr);
        g_signal_emit_by_name(button, "clicked");
    }
    void restore_job(bool cancellable) {
        WipeJobSnapshot job;
        job.plan.id = "test-gtk-job";
        job.plan.device.disk = MockDiskService::CreateTestDisk("/dev/sda");
        job.plan.firmware = {cancellable ? "overwrite" : "nvme-sanitize-block", "device",
                             cancellable};
        job.plan.targets = {job.plan.device};
        job.started_at_unix = 1'700'000'000;
        ON_CALL(*disks_, get_available_disks(_))
            .WillByDefault([disk = job.plan.device.disk](auto callback) {
                callback(std::vector<DiskInfo>{disk});
            });
        EXPECT_CALL(*wipes_, get_wipe_jobs()).WillOnce(Return(std::vector<WipeJobSnapshot>{job}));
        EXPECT_CALL(*wipes_, observe_wipe_job(_, _)).WillOnce([&](const auto&, auto callback) {
            progress_ = std::move(callback);
            return true;
        });
        vm_->set_connection_state(true);
        ASSERT_TRUE(wait_for([&] { return bool(progress_); }));
    }
    void queue_message() {
        vm_->current_message.set(MessageInfo{.type = MessageInfo::Type::INFO,
                                             .title = "Late completion",
                                             .message = "Completed",
                                             .confirmation_callback = {}});
    }
};

TEST_F(MainWindowLifecycleTest, QueuedMessageDoesNotOutliveCppView) {
    queue_message();
    view_.reset();
    pump();
    auto* dialogs = adw_application_window_get_dialogs(window_);
    EXPECT_EQ(g_list_model_get_n_items(dialogs), 0u);
    g_object_unref(dialogs);
}

TEST_F(MainWindowLifecycleTest, QueuedMessageDoesNotOutliveGtkWindow) {
    queue_message();
    gtk_window_destroy(GTK_WINDOW(window_));
    g_object_unref(window_);
    window_ = nullptr;
    ASSERT_TRUE(destroyed_);
    pump();
    vm_->current_message.set(MessageInfo{.type = MessageInfo::Type::INFO,
                                         .title = "Another late message",
                                         .message = "Completed",
                                         .confirmation_callback = {}});
    pump();
}

TEST_F(MainWindowLifecycleTest, RebindDropsQueuedMessagesFromPreviousModel) {
    queue_message();
    auto replacement = std::make_shared<MainViewModel>(disks_, wipes_);
    view_->bind(replacement);
    pump();
    auto* dialogs = adw_application_window_get_dialogs(window_);
    EXPECT_EQ(g_list_model_get_n_items(dialogs), 0u);
    g_object_unref(dialogs);
}

TEST_F(MainWindowLifecycleTest, PendingWorkCloseKeepsWindowOpenByDefault) {
    vm_->is_operation_pending.set(true);
    gtk_window_close(GTK_WINDOW(window_));
    pump();
    EXPECT_FALSE(closed());
    auto* dialog = ADW_ALERT_DIALOG(adw_application_window_get_visible_dialog(window_));
    ASSERT_NE(dialog, nullptr);
    EXPECT_STREQ(adw_alert_dialog_get_default_response(dialog), "keep");
    EXPECT_STREQ(adw_alert_dialog_get_close_response(dialog), "keep");
    respond(dialog, "Keep Open");
    pump();
    EXPECT_FALSE(closed());
    EXPECT_TRUE(vm_->is_operation_pending.get());
}

TEST_F(MainWindowLifecycleTest, ExplicitStopWaitsForPendingWorkToFinishBeforeClosing) {
    vm_->is_operation_pending.set(true);
    gtk_window_close(GTK_WINDOW(window_));
    pump();
    auto* dialog = ADW_ALERT_DIALOG(adw_application_window_get_visible_dialog(window_));
    ASSERT_NE(dialog, nullptr);
    respond(dialog, "Stop and Close When Finished");
    pump();
    EXPECT_FALSE(closed());
    vm_->is_operation_pending.set(false);
    ASSERT_TRUE(wait_for([&] { return closed(); }));
}

TEST_F(MainWindowLifecycleTest, FirmwareStopKeepsMonitoringUntilTerminalResult) {
    restore_job(false);
    EXPECT_CALL(*wipes_, cancel_prepared_wipe(_)).Times(0);
    EXPECT_CALL(*wipes_, cancel_operation(_)).Times(0);
    gtk_window_close(GTK_WINDOW(window_));
    pump();
    auto* dialog = ADW_ALERT_DIALOG(adw_application_window_get_visible_dialog(window_));
    ASSERT_NE(dialog, nullptr);
    EXPECT_NE(std::string{adw_alert_dialog_get_body(dialog)}.find("cannot be stopped"),
              std::string::npos);
    respond(dialog, "Stop and Close When Finished");
    pump();
    EXPECT_FALSE(closed());
    WipeProgress done;
    done.is_complete = true;
    progress_(done);
    ASSERT_TRUE(wait_for([&] { return closed(); }));
}

TEST_F(MainWindowLifecycleTest, ExplicitLeaveClosesWithoutCancellingStartedJob) {
    restore_job(true);
    EXPECT_CALL(*wipes_, cancel_prepared_wipe(_)).Times(0);
    EXPECT_CALL(*wipes_, cancel_operation(_)).Times(0);
    gtk_window_close(GTK_WINDOW(window_));
    pump();
    auto* dialog = ADW_ALERT_DIALOG(adw_application_window_get_visible_dialog(window_));
    ASSERT_NE(dialog, nullptr);
    respond(dialog, "Leave Started Jobs Running and Close");
    ASSERT_TRUE(wait_for([&] { return closed(); }));
    WipeProgress done;
    done.is_complete = true;
    progress_(done);
    pump();
}

TEST_F(MainWindowLifecycleTest, SoftwareWipeShowsEnabledCancelButton) {
    restore_job(true);
    vm_->select_disk("/dev/sda");
    WipeProgress update;
    update.total_bytes = 2'048;
    update.percentage = 25;
    progress_(update);
    pump();
    auto* cancel = find_button(GTK_WIDGET(window_), "Cancel");
    ASSERT_NE(cancel, nullptr);
    EXPECT_TRUE(gtk_widget_get_visible(cancel));
    EXPECT_TRUE(gtk_widget_get_sensitive(cancel));
}

TEST_F(MainWindowLifecycleTest, UncancellableFirmwareHidesCancelButton) {
    restore_job(false);
    vm_->select_disk("/dev/sda");
    WipeProgress update;
    update.total_bytes = 2'048;
    update.percentage = 25;
    progress_(update);
    pump();
    auto* cancel = find_button(GTK_WIDGET(window_), "Cancel");
    ASSERT_NE(cancel, nullptr);
    EXPECT_FALSE(gtk_widget_get_visible(cancel));
    EXPECT_FALSE(gtk_widget_get_sensitive(cancel));
}
