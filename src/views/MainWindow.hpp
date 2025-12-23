/**
 * @file MainWindow.hpp
 * @brief Main application window (Hybrid Adwaita/gtkmm MVVM View)
 *
 * Implements a hybrid View layer combining:
 * - AdwApplicationWindow and AdwHeaderBar (raw C API for Adwaita styling)
 * - MainWindowContent (pure gtkmm4 for content widgets)
 *
 * This approach is necessary because libadwaitamm (C++ bindings) is unstable.
 */

#pragma once

#include "view_models/MainViewModel.hpp"
#include "views/MainWindowContent.hpp"
#include <adwaita.h>
#include <gtkmm.h>
#include <memory>

/**
 * @class MainWindow
 * @brief Main application window using hybrid Adwaita/gtkmm4 approach
 *
 * The window structure:
 * - AdwApplicationWindow (C API) - outer window container
 *   - VBox (C API) - main vertical layout
 *     - AdwHeaderBar (C API) - title bar with buttons
 *     - MainWindowContent (gtkmm4) - main content area
 */
class MainWindow {
public:
    /**
     * @brief Construct the main window
     * @param window The AdwApplicationWindow to wrap
     */
    explicit MainWindow(AdwApplicationWindow* window);
    ~MainWindow();

    // Prevent copying
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;
    MainWindow(MainWindow&&) = delete;
    MainWindow& operator=(MainWindow&&) = delete;

    /**
     * @brief Bind this view to a ViewModel
     * @param view_model The ViewModel to bind to
     */
    void bind(std::shared_ptr<MainViewModel> view_model);

    /**
     * @brief Set up the UI components
     */
    void setup_ui();

    /**
     * @brief Show the window
     */
    void show();

private:
    AdwApplicationWindow* window_;
    std::shared_ptr<MainViewModel> view_model_;

    // gtkmm4 content widget
    std::unique_ptr<MainWindowContent> content_;

    // Adwaita widgets (C API)
    GtkWidget* header_bar_ = nullptr;

    // Message binding subscription
    size_t message_subscription_id_ = 0;

    // UI creation methods
    void create_header_bar();

    // Message dialog handling (kept as Adwaita for proper styling)
    void bind_messages();
    void show_message(const MessageInfo& message);
    void show_confirmation_dialog(const MessageInfo& message);
    void show_info_dialog(const MessageInfo& message);

    // Event handlers for header bar (Adwaita widgets use C callbacks)
    static void on_refresh_clicked(GtkWidget* widget, gpointer user_data);
    static void on_about_clicked(GtkWidget* widget, gpointer user_data);

    // About dialog
    void show_about_dialog();
};
