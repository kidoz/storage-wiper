/**
 * @file MainWindowContent.hpp
 * @brief Main window content widget using gtkmm4
 *
 * Pure gtkmm4 composite widget that contains the main application content.
 * Used within the hybrid MainWindow that wraps AdwApplicationWindow.
 */

#pragma once

#include "viewmodels/MainViewModel.hpp"

#include <gtkmm.h>

#include <memory>
#include <mutex>
#include <queue>
#include <vector>

// Forward declarations
class DiskRow;
class AlgorithmRow;

/**
 * @class MainWindowContent
 * @brief Main content area widget using pure gtkmm4
 *
 * This widget contains:
 * - Disk selection list
 * - Algorithm selection options
 * - Progress indicators
 * - Action buttons (wipe, cancel)
 *
 * It binds to MainViewModel observable properties for MVVM data binding.
 */
class MainWindowContent : public Gtk::Box {
public:
    MainWindowContent();
    ~MainWindowContent() override;

    // Prevent copying
    MainWindowContent(const MainWindowContent&) = delete;
    MainWindowContent& operator=(const MainWindowContent&) = delete;
    MainWindowContent(MainWindowContent&&) = delete;
    MainWindowContent& operator=(MainWindowContent&&) = delete;

    /**
     * @brief Bind this content widget to a ViewModel
     * @param view_model The ViewModel to bind to
     */
    void bind(std::shared_ptr<MainViewModel> view_model);

    /**
     * @brief Get the currently selected disk path
     * @return Selected disk path or empty string if none selected
     */
    [[nodiscard]] auto get_selected_disk_path() const -> std::string;

private:
    std::shared_ptr<MainViewModel> view_model_;

    // Template child widgets (from UI file)
    Gtk::ListBox* disk_list_ = nullptr;
    Gtk::Box* options_box_ = nullptr;
    Gtk::ProgressBar* progress_bar_ = nullptr;
    Gtk::Label* progress_label_ = nullptr;
    Gtk::Button* wipe_button_ = nullptr;
    Gtk::Button* cancel_button_ = nullptr;

    // Algorithm radio button group
    std::vector<AlgorithmRow*> algorithm_rows_;
    Gtk::CheckButton* first_radio_ = nullptr;

    // Thread-safe UI dispatch using Glib::Dispatcher
    Glib::Dispatcher dispatcher_;
    std::queue<std::function<void()>> pending_tasks_;
    mutable std::mutex task_mutex_;

    // Flag to ignore selection changes during list updates
    bool updating_disk_list_ = false;

    // Subscription IDs for cleanup
    std::vector<size_t> subscriptions_;

    // Setup methods
    void setup_from_builder();
    void setup_dispatcher();
    void connect_signals();

    // Thread-safe UI update dispatch
    void post_ui_update(std::function<void()> task);
    void process_pending_tasks();

    // Data binding methods
    void bind_disks();
    void bind_algorithms();
    void bind_progress();
    void bind_can_wipe();

    // UI update methods (called from bindings via dispatcher)
    void update_disk_list(const std::vector<DiskInfo>& disks);
    void update_algorithm_list(const std::vector<AlgorithmInfo>& algorithms);
    void update_progress(const WipeProgress& progress);
    void update_progress_visibility(bool visible);

    // Signal handlers
    void on_disk_selected(Gtk::ListBoxRow* row);
    void on_wipe_clicked();
    void on_cancel_clicked();
};
