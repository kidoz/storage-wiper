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
#include "util/AppSettings.hpp"
#include "util/WipeCertificate.hpp"

#include <chrono>
#include <map>
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
class MainViewModel : public mvvm::ObservableObject,
                      public std::enable_shared_from_this<MainViewModel> {
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
     * @brief Whether a privileged operation is being prepared before wiping
     */
    mvvm::Observable<bool> is_operation_pending{false};

    /**
     * @brief Whether the disk list is currently being refreshed
     */
    mvvm::Observable<bool> is_disk_refreshing{false};

    /**
     * @brief Whether the wipe button should be enabled
     */
    mvvm::Observable<bool> can_wipe{false};

    /**
     * @brief Current wipe progress of the selected disk
     *
     * Mirrors the entry of the selected disk in wipe_progresses so the view
     * can keep a single progress area bound to it.
     */
    mvvm::Observable<WipeProgress> wipe_progress{{}};

    /**
     * @brief Progress of every wipe currently in flight, keyed by device path
     */
    mvvm::Observable<std::map<std::string, WipeProgress>> wipe_progresses{{}};

    /**
     * @brief Whether post-wipe verification is enabled for the next wipe
     */
    mvvm::Observable<bool> verification_enabled{false};

    /**
     * @brief Whether the selected algorithm supports post-wipe verification
     */
    mvvm::Observable<bool> verification_available{false};

    /**
     * @brief Warning text for the selected disk/algorithm combination
     */
    mvvm::Observable<std::string> algorithm_warning{""};

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

    /**
     * @brief Callback type for desktop notifications
     * @param title Notification title
     * @param body Notification body text
     * @param is_error True if this is an error notification
     */
    using NotificationCallback =
        std::function<void(const std::string& title, const std::string& body, bool is_error)>;

    /**
     * @brief Set the callback for sending desktop notifications
     * @param callback Function to call when a notification should be shown
     */
    void set_notification_callback(NotificationCallback callback);

    /**
     * @brief Set the directory wipe certificates are written to on success
     *
     * Certificate writing is disabled while no directory is configured (used
     * by unit tests to stay off the real filesystem).
     */
    void set_certificate_directory(const std::string& directory);

    /**
     * @brief Restore the last-used wipe preset
     *
     * Invalid algorithm ids are ignored; verification is dropped again when
     * the restored algorithm cannot verify.
     */
    void restore_settings(const util::AppSettingsData& settings);

    /**
     * @brief Snapshot the current wipe settings for persistence
     */
    [[nodiscard]] auto current_settings() const -> util::AppSettingsData;

    /**
     * @brief Warning shown when an erase reaches beyond the selected device
     *
     * An NVMe firmware erase is issued to the controller, so it destroys every
     * namespace the controller owns. Public so the wording is unit tested.
     *
     * @param algorithm Selected algorithm
     * @param path Selected device path
     * @return Warning text, or empty when the erase is limited to the device
     */
    [[nodiscard]] static auto controller_wide_warning(WipeAlgorithm algorithm,
                                                      const std::string& path) -> std::string;

    /**
     * @brief Scope statement shown in the wipe confirmation dialog
     *
     * Makes explicit whether the wipe covers the whole device (including
     * partitions and the partition table) or only one partition. Public so
     * the wording is unit tested.
     *
     * @param disk Selected device
     * @param child_partitions Partition device names of the disk (empty for
     *                         partitions themselves)
     * @return Scope text, or empty when there is nothing to call out
     */
    [[nodiscard]] static auto build_scope_note(const DiskInfo& disk,
                                               const std::vector<std::string>& child_partitions)
        -> std::string;

private:
    /**
     * @brief Per-device bookkeeping for a wipe started from this ViewModel
     */
    struct WipeActivity {
        WipeAlgorithm algorithm = WipeAlgorithm::ZERO_FILL;
        bool verify = false;
        std::chrono::system_clock::time_point started_at{};
        std::chrono::steady_clock::time_point started_steady{};
        uint64_t peak_speed = 0;
    };

    std::shared_ptr<IDiskService> disk_service_;
    std::shared_ptr<IWipeService> wipe_service_;
    NotificationCallback notification_callback_;

    // Subscription IDs for cleanup
    size_t selected_disk_subscription_id_ = 0;
    size_t selected_algorithm_subscription_id_ = 0;
    size_t wipe_in_progress_subscription_id_ = 0;
    size_t operation_pending_subscription_id_ = 0;
    size_t connection_subscription_id_ = 0;

    void load_disks();
    void load_algorithms();
    void update_can_wipe();
    void update_algorithm_state();
    void start_wipe();
    void confirm_wipe_for(const std::string& path, WipeAlgorithm algorithm, bool verify);
    void unmount_and_wipe(const std::string& path);
    void handle_wipe_progress(const std::string& path, const WipeProgress& progress);
    void finish_wipe(const std::string& path, const WipeProgress& progress);
    void handle_wipe_completion(const std::string& path, const WipeActivity& activity,
                                const WipeProgress& progress);
    void show_message(MessageInfo::Type type, const std::string& title, const std::string& message,
                      std::function<void(bool)> callback = nullptr);
    [[nodiscard]] auto find_disk_info(const std::string& path) const -> std::optional<DiskInfo>;
    [[nodiscard]] auto build_disk_summary(const std::optional<DiskInfo>& disk_info,
                                          const std::string& fallback_path) const -> std::string;
    [[nodiscard]] auto build_certificate_data(const std::string& path, const WipeActivity& activity,
                                              const WipeProgress& progress) const
        -> util::WipeCertificateData;

    // Wipes in flight, keyed by device path (main thread only)
    std::map<std::string, WipeActivity> active_wipes_;

    // Certificate output directory; empty disables certificate writing (tests)
    std::string certificate_directory_;
};
