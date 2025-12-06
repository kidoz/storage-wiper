/**
 * @file MainWindow.hpp
 * @brief Main application window (MVVM View)
 *
 * Implements the View layer of MVVM, binding to MainViewModel properties
 * and executing ViewModel commands.
 */

#pragma once

#include "view_models/MainViewModel.hpp"
#include <adwaita.h>
#include <string>
#include <memory>
#include <vector>
#include <functional>
#include <mutex>

/**
 * @class MainWindow
 * @brief Main application window using MVVM data binding
 *
 * The View binds to ViewModel observable properties and updates UI automatically.
 * User actions trigger ViewModel commands.
 */
class MainWindow {
public:
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

    // Subscription IDs for property change notifications
    std::vector<size_t> subscriptions_;

    // UI components
    GtkWidget* header_bar_ = nullptr;
    GtkWidget* disk_list_ = nullptr;
    GtkWidget* options_box_ = nullptr;
    GtkWidget* action_bar_ = nullptr;
    GtkWidget* wipe_button_ = nullptr;
    GtkWidget* progress_bar_ = nullptr;
    GtkWidget* progress_label_ = nullptr;
    GtkWidget* cancel_button_ = nullptr;

    // Algorithm radio buttons
    std::vector<GtkWidget*> algorithm_buttons_;
    bool destroyed_ = false;
    std::vector<guint> idle_sources_;
    std::mutex idle_mutex_;

    // UI creation methods
    void create_header_bar();
    void create_disk_list_view();
    void create_wipe_options();
    void create_action_bar();
    void create_progress_view();

    // Data binding methods
    void bind_disks();
    void bind_algorithms();
    void bind_wipe_progress();
    void bind_can_wipe();
    void bind_messages();

    // UI update methods (called from bindings)
    void update_disk_list(const std::vector<DiskInfo>& disks);
    void update_algorithm_list(const std::vector<AlgorithmInfo>& algorithms);
    void update_wipe_progress(const WipeProgress& progress);
    void update_progress_visibility(bool visible);
    void show_message(const MessageInfo& message);

    // Event handlers (trigger ViewModel commands/methods)
    static void on_disk_row_selected(GtkListBox* list_box, GtkListBoxRow* row, gpointer user_data);
    static void on_refresh_clicked(GtkWidget* widget, gpointer user_data);
    static void on_wipe_clicked(GtkWidget* widget, gpointer user_data);
    static void on_algorithm_toggled(GtkWidget* widget, gpointer user_data);
    static void on_cancel_wipe_clicked(GtkWidget* widget, gpointer user_data);
    static void on_about_clicked(GtkWidget* widget, gpointer user_data);

    // Helper methods
    void clear_disk_list();
    [[nodiscard]] auto create_disk_row(const DiskInfo& disk) -> GtkWidget*;
    void post_idle(std::function<void()> task);
    void register_idle_source(guint source_id);
    void unregister_idle_source(guint source_id);
};
