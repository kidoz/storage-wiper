#include "Application.hpp"
#include "views/MainWindow.hpp"
#include "view_models/MainViewModel.hpp"
#include "services/DiskService.hpp"
#include "services/WipeService.hpp"
#include "interfaces/IDiskService.hpp"
#include "interfaces/IWipeService.hpp"
#include <iostream>

StorageWiperApp::StorageWiperApp()
    : app_(nullptr)
    , main_window_(nullptr) {

    app_ = gtk_application_new("com.github.storage-wiper", G_APPLICATION_DEFAULT_FLAGS);
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
    adw_init();
}

void StorageWiperApp::on_activate(GtkApplication* app, gpointer user_data) {
    auto* self = static_cast<StorageWiperApp*>(user_data);

    try {
        // Create main window
        self->main_window_ = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
        gtk_window_set_title(GTK_WINDOW(self->main_window_), "Storage Wiper");
        gtk_window_set_default_size(GTK_WINDOW(self->main_window_), 800, 600);
        gtk_window_set_icon_name(GTK_WINDOW(self->main_window_), "com.github.storage-wiper");

        // Configure DI container with services
        self->configure_services();

        // Setup MVVM pattern (creates UI but doesn't load data yet)
        self->setup_main_window();

        // Show the window immediately for responsive startup
        gtk_window_present(GTK_WINDOW(self->main_window_));

        // Initialize ViewModel asynchronously after window is shown
        // This defers disk enumeration so the window appears instantly
        g_idle_add([](gpointer data) -> gboolean {
            auto* app_self = static_cast<StorageWiperApp*>(data);
            if (app_self->view_model_) {
                app_self->view_model_->initialize();
            }
            return G_SOURCE_REMOVE;
        }, self);

    } catch (const std::exception& e) {
        std::cerr << "Error during application activation: " << e.what() << std::endl;
        g_application_quit(G_APPLICATION(app));
    }
}

void StorageWiperApp::configure_services() {
    // Register services in DI container
    container_.register_type<IDiskService, DiskService>(di::Lifetime::SINGLETON);
    container_.register_type<IWipeService, WipeService>(di::Lifetime::SINGLETON);
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

    // Note: ViewModel initialization (disk loading) is deferred to on_activate
    // via g_idle_add() for responsive window appearance
}

void StorageWiperApp::cleanup() {
    if (view_model_) {
        view_model_->cleanup();
        view_model_.reset();
    }

    view_.reset();
    container_.clear();
}