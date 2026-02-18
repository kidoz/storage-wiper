#include "Application.hpp"

#include "services/DBusClient.hpp"
#include "services/IDiskService.hpp"
#include "services/IWipeService.hpp"
#include "util/Logger.hpp"
#include "viewmodels/MainViewModel.hpp"
#include "views/MainWindow.hpp"

#include <glibmm/main.h>
#include <gtkmm/init.h>

#include <filesystem>
#include <format>

StorageWiperApp::StorageWiperApp() : app_(nullptr), main_window_(nullptr) {
    app_ = gtk_application_new("su.kidoz.storage_wiper", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app_, "activate", G_CALLBACK(on_activate), this);
    g_signal_connect(app_, "startup", G_CALLBACK(on_startup), this);
}

StorageWiperApp::~StorageWiperApp() {
    cleanup();

    if (app_) {
        g_object_unref(app_);
    }
}

auto StorageWiperApp::run(int argc, char* argv[]) -> int {
    return g_application_run(G_APPLICATION(app_), argc, argv);
}

auto StorageWiperApp::container() -> di::Container& {
    return container_;
}

void StorageWiperApp::on_startup(GtkApplication*, gpointer) {
    // Initialize Adwaita
    adw_init();

    // Initialize gtkmm type system for hybrid C API / gtkmm usage
    // This is required when using raw C GtkApplication with gtkmm widgets
    Gtk::init_gtkmm_internals();

    // Initialize logger for GUI application
    auto log_dir = std::filesystem::path(g_get_user_data_dir()) / "storage-wiper" / "logs";
    util::Logger::instance().initialize(log_dir, "storage-wiper");
}

void StorageWiperApp::on_activate(GtkApplication* app, gpointer user_data) {
    auto* self = static_cast<StorageWiperApp*>(user_data);

    try {
        // Create main window
        self->main_window_ = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
        gtk_window_set_title(GTK_WINDOW(self->main_window_), "Storage Wiper");
        gtk_window_set_default_size(GTK_WINDOW(self->main_window_), 800, 600);

        // Configure DI container with services
        self->configure_services();

        // Setup MVVM pattern
        self->setup_main_window();

        // Show the window
        gtk_window_present(GTK_WINDOW(self->main_window_));

    } catch (const std::exception& e) {
        LOG_ERROR("Application", std::format("Error during application activation: {}", e.what()));
        g_application_quit(G_APPLICATION(app));
    }
}

void StorageWiperApp::configure_services() {
    // Create DBusClient and connect to the helper service
    dbus_client_ = std::make_shared<DBusClient>();

    // Attempt to connect - if it fails, reconnection logic will retry automatically
    if (!dbus_client_->connect()) {
        LOG_WARNING("Application", "Initial connection to storage-wiper-helper failed. "
                                   "Will retry automatically when service becomes available.");
        // Don't throw - the reconnection logic will handle it
    }

    // Register DBusClient as both disk and wipe service (it implements both interfaces)
    container_.register_instance<IDiskService>(
        std::static_pointer_cast<IDiskService>(dbus_client_));
    container_.register_instance<IWipeService>(
        std::static_pointer_cast<IWipeService>(dbus_client_));
    container_.register_instance<DBusClient>(dbus_client_);
}

void StorageWiperApp::setup_main_window() {
    // Resolve services from DI container
    auto disk_service = container_.resolve<IDiskService>();
    auto wipe_service = container_.resolve<IWipeService>();

    // Create ViewModel with injected dependencies
    view_model_ = std::make_shared<MainViewModel>(disk_service, wipe_service);

    // Set up connection state callback
    // Use weak_ptr to avoid preventing ViewModel destruction
    std::weak_ptr<MainViewModel> weak_vm = view_model_;
    dbus_client_->set_connection_state_callback([weak_vm](ConnectionState state,
                                                          const std::string& error) {
        Glib::signal_idle().connect([weak_vm, state, error]() {
            if (auto vm = weak_vm.lock()) {
                bool connected = (state == ConnectionState::CONNECTED);
                vm->set_connection_state(connected, error);
            }
            return false; // G_SOURCE_REMOVE
        });
    });

    // Set initial connection state
    bool initial_connected = (dbus_client_->get_connection_state() == ConnectionState::CONNECTED);
    view_model_->set_connection_state(initial_connected, "");

    // Set up notification callback
    GApplication* g_app = G_APPLICATION(app_);
    view_model_->set_notification_callback(
        [g_app](const std::string& title, const std::string& body, bool is_error) {
            GNotification* notification = g_notification_new(title.c_str());
            g_notification_set_body(notification, body.c_str());

            // Set icon based on success/error
            if (is_error) {
                g_notification_set_icon(notification, g_themed_icon_new("dialog-error-symbolic"));
            } else {
                g_notification_set_icon(notification, g_themed_icon_new("emblem-ok-symbolic"));
            }

            // Send the notification
            g_application_send_notification(g_app, "wipe-complete", notification);
            g_object_unref(notification);
        });

    // Create View
    view_ = std::make_unique<MainWindow>(main_window_);

    // Setup UI first
    view_->setup_ui();

    // Bind View to ViewModel (sets up data bindings)
    view_->bind(view_model_);

    // Initialize ViewModel (loads initial data)
    view_model_->initialize();
}

void StorageWiperApp::cleanup() {
    if (view_model_) {
        view_model_->cleanup();
        view_model_.reset();
    }

    view_.reset();
    container_.clear();
}
