/**
 * @file Application.hpp
 * @brief Main application class using MVVM architecture
 *
 * This file defines the main application class that coordinates between
 * the view, viewmodel, and service layers using dependency injection.
 */

#pragma once

#include "di/Container.hpp"
#include <adwaita.h>
#include <memory>

// Forward declarations
class DBusClient;
class MainWindow;
class MainViewModel;

/**
 * @class StorageWiperApp
 * @brief Main application class implementing MVVM pattern
 *
 * This class serves as the application entry point and coordinates
 * between the MVVM layers. It handles GTK application lifecycle and
 * manages dependency injection for all components through a DI container.
 */
class StorageWiperApp {
public:
    StorageWiperApp();
    ~StorageWiperApp();

    /**
     * @brief Run the application
     * @param argc Command line argument count
     * @param argv Command line arguments
     * @return Exit code
     */
    auto run(int argc, char* argv[]) -> int;

    /**
     * @brief Get the DI container
     * @return Reference to the application's DI container
     */
    [[nodiscard]] auto container() -> di::Container&;

private:
    static void on_activate(GtkApplication* app, gpointer user_data);
    static void on_startup(GtkApplication* app, gpointer user_data);

    void configure_services();
    void setup_main_window();
    void cleanup();

    GtkApplication* app_;
    AdwApplicationWindow* main_window_;
    di::Container container_;

    // MVVM components
    std::unique_ptr<MainWindow> view_;
    std::shared_ptr<MainViewModel> view_model_;

    // D-Bus client (kept for lifetime management)
    std::shared_ptr<DBusClient> dbus_client_;
};