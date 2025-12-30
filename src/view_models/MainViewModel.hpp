/**
 * @file MainViewModel.hpp
 * @brief ViewModel for the main application window
 *
 * Implements the MVVM pattern providing observable properties and commands
 * for the main window UI.
 */

#pragma once

#include "core/Command.hpp"
#include "core/Observable.hpp"
#include "models/ViewTypes.hpp"
#include "services/IDiskService.hpp"
#include "services/IWipeService.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

/**
 * @class MainViewModel
 * @brief ViewModel for the main application window
 *
 * Exposes observable properties and commands that the View binds to.
 * Handles all business logic and coordinates with services.
 */
class MainViewModel : public mvvm::ObservableObject {
public:
    MainViewModel(std::shared_ptr<IDiskService> disk_service,
                  std::shared_ptr<IWipeService> wipe_service);
    ~MainViewModel();

    // Prevent copying and moving
    MainViewModel(const MainViewModel&) = delete;
    MainViewModel& operator=(const MainViewModel&) = delete;
    MainViewModel(MainViewModel&&) = delete;
    MainViewModel& operator=(MainViewModel&&) = delete;

    // ========== Observable Properties ==========

    /**
     * @brief List of available disks
     */
    mvvm::Observable<std::vector<DiskInfo>> disks{{}};

    /**
     * @brief List of available wipe algorithms
     */
    mvvm::Observable<std::vector<AlgorithmInfo>> algorithms{{}};

    /**
     * @brief Currently selected disk path
     */
    mvvm::Observable<std::string> selected_disk_path{""};

    /**
     * @brief Currently selected algorithm
     */
    mvvm::Observable<WipeAlgorithm> selected_algorithm{WipeAlgorithm::ZERO_FILL};

    /**
     * @brief Whether a wipe operation is in progress
     */
    mvvm::Observable<bool> is_wipe_in_progress{false};

    /**
     * @brief Whether the wipe button should be enabled
     */
    mvvm::Observable<bool> can_wipe{false};

    /**
     * @brief Current wipe progress
     */
    mvvm::Observable<WipeProgress> wipe_progress{{}};

    /**
     * @brief Message to display to user (info, error, or confirmation)
     */
    mvvm::Observable<MessageInfo> current_message{{}};

    /**
     * @brief Whether connected to the D-Bus helper service
     */
    mvvm::Observable<bool> is_connected{false};

    /**
     * @brief Connection error message (empty if connected)
     */
    mvvm::Observable<std::string> connection_error{""};

    // ========== Commands ==========

    /**
     * @brief Command to refresh the disk list
     */
    std::shared_ptr<mvvm::RelayCommand> refresh_command;

    /**
     * @brief Command to start the wipe operation
     */
    std::shared_ptr<mvvm::RelayCommand> wipe_command;

    /**
     * @brief Command to cancel the current wipe operation
     */
    std::shared_ptr<mvvm::RelayCommand> cancel_command;

    // ========== Methods ==========

    /**
     * @brief Initialize the ViewModel and load initial data
     */
    void initialize();

    /**
     * @brief Clean up resources
     */
    void cleanup();

    /**
     * @brief Set the selected disk
     * @param disk_path Path of the selected disk
     */
    void select_disk(const std::string& disk_path);

    /**
     * @brief Set the selected algorithm
     * @param algorithm The selected wipe algorithm
     */
    void select_algorithm(WipeAlgorithm algorithm);

    /**
     * @brief Confirm and start the wipe operation (called from confirmation dialog)
     */
    void confirm_wipe();

    /**
     * @brief Update connection state (called by Application when D-Bus state changes)
     * @param connected Whether connected to the helper service
     * @param error_message Error message if not connected
     */
    void set_connection_state(bool connected, const std::string& error_message = "");

private:
    std::shared_ptr<IDiskService> disk_service_;
    std::shared_ptr<IWipeService> wipe_service_;

    // Subscription IDs for cleanup
    size_t selected_disk_subscription_id_ = 0;
    size_t wipe_in_progress_subscription_id_ = 0;
    size_t connection_subscription_id_ = 0;

    void load_disks();
    void load_algorithms();
    void update_can_wipe();
    void start_wipe();
    void unmount_and_wipe(const std::string& path);
    void handle_wipe_progress(const WipeProgress& progress);
    void handle_wipe_completion(bool success, const std::string& error_message = "");
    void show_message(MessageInfo::Type type, const std::string& title, const std::string& message,
                      std::function<void(bool)> callback = nullptr);
    [[nodiscard]] auto find_disk_info(const std::string& path) const -> std::optional<DiskInfo>;
};
