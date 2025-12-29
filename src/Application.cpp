#include "Application.hpp"
#include "services/DBusClient.hpp"
#include "services/IDiskService.hpp"
#include "services/IWipeService.hpp"
#include "view_models/MainViewModel.hpp"
#include "views/MainWindow.hpp"
#include <gtkmm.h>
#include <gtkmm/init.h>
#include <iostream>

StorageWiperApp::StorageWiperApp()
    : app_(nullptr)
    , main_window_(nullptr) {

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
        std::cerr << "Error during application activation: " << e.what() << std::endl;
        g_application_quit(G_APPLICATION(app));
    }
}

void StorageWiperApp::configure_services() {
    // Create DBusClient and connect to the helper service
    auto dbus_client = std::make_shared<DBusClient>();
    if (!dbus_client->connect()) {
        throw std::runtime_error(
            "Failed to connect to storage-wiper-helper D-Bus service.\n"
            "Make sure the service is installed and running.");
    }

    // Register DBusClient as both disk and wipe service (it implements both interfaces)
    container_.register_instance<IDiskService>(
        std::static_pointer_cast<IDiskService>(dbus_client));
    container_.register_instance<IWipeService>(
        std::static_pointer_cast<IWipeService>(dbus_client));
}

void StorageWiperApp::setup_main_window() {
    // Resolve services from DI container
    auto disk_service = container_.resolve<IDiskService>();
    auto wipe_service = container_.resolve<IWipeService>();

    // Create ViewModel with injected dependencies
    view_model_ = std::make_shared<MainViewModel>(disk_service, wipe_service);

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