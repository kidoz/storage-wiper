#include "viewmodels/MainViewModel.hpp"

#include <glib.h>

#include <algorithm>
#include <iostream>
#include <sstream>
#include <utility>

MainViewModel::MainViewModel(std::shared_ptr<IDiskService> disk_service,
                             std::shared_ptr<IWipeService> wipe_service)
    : disk_service_(std::move(disk_service)), wipe_service_(std::move(wipe_service)) {
    // Initialize commands
    refresh_command = std::make_shared<mvvm::RelayCommand>(
        [this]() { load_disks(); }, [this]() { return !is_wipe_in_progress.get(); });

    wipe_command = std::make_shared<mvvm::RelayCommand>([this]() { start_wipe(); },
                                                        [this]() { return can_wipe.get(); });

    cancel_command = std::make_shared<mvvm::RelayCommand>(
        [this]() {
            if (wipe_service_->cancel_current_operation()) {
                show_message(MessageInfo::Type::INFO, "Cancelling",
                             "Wipe operation is being cancelled...");
            }
        },
        [this]() { return is_wipe_in_progress.get(); });

    // Subscribe to property changes that affect can_wipe
    selected_disk_subscription_id_ =
        selected_disk_path.subscribe([this](const std::string&) { update_can_wipe(); });
    wipe_in_progress_subscription_id_ = is_wipe_in_progress.subscribe([this](bool) {
        update_can_wipe();
        refresh_command->raise_can_execute_changed();
        wipe_command->raise_can_execute_changed();
        cancel_command->raise_can_execute_changed();
    });
    connection_subscription_id_ = is_connected.subscribe([this](bool) {
        update_can_wipe();
        refresh_command->raise_can_execute_changed();
    });
}

MainViewModel::~MainViewModel() {
    cleanup();
}

void MainViewModel::initialize() {
    load_algorithms();
    load_disks();
    update_can_wipe();
}

void MainViewModel::cleanup() {
    // Unsubscribe from observables
    selected_disk_path.unsubscribe(selected_disk_subscription_id_);
    is_wipe_in_progress.unsubscribe(wipe_in_progress_subscription_id_);
    is_connected.unsubscribe(connection_subscription_id_);

    if (is_wipe_in_progress.get()) {
        wipe_service_->cancel_current_operation();
    }
}

void MainViewModel::select_disk(const std::string& disk_path) {
    selected_disk_path.set(disk_path);
}

void MainViewModel::select_algorithm(WipeAlgorithm algorithm) {
    selected_algorithm.set(algorithm);
}

void MainViewModel::load_disks() {
    // Don't try to load disks if not connected to D-Bus service
    if (!is_connected.get()) {
        disks.set({});
        update_can_wipe();
        return;
    }

    try {
        auto disk_list = disk_service_->get_available_disks();
        disks.set(disk_list);

        // Clear selection if previously selected disk is no longer available
        const auto& current_selection = selected_disk_path.get();
        if (!current_selection.empty()) {
            bool found = false;
            for (const auto& disk : disk_list) {
                if (disk.path == current_selection) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                selected_disk_path.set("");
            }
        }

        update_can_wipe();
    } catch (const std::exception& e) {
        show_message(MessageInfo::Type::ERROR, "Error",
                     "Failed to refresh disk list: " + std::string(e.what()));
    }
}

void MainViewModel::load_algorithms() {
    std::vector<AlgorithmInfo> algo_list;

    // Get algorithm info from WipeService
    constexpr std::array all_algorithms = {
        WipeAlgorithm::ZERO_FILL, WipeAlgorithm::RANDOM_FILL, WipeAlgorithm::DOD_5220_22_M,
        WipeAlgorithm::SCHNEIER,  WipeAlgorithm::VSITR,       WipeAlgorithm::GOST_R_50739_95,
        WipeAlgorithm::GUTMANN};

    for (auto algo : all_algorithms) {
        algo_list.push_back(
            AlgorithmInfo{.algorithm = algo,
                          .name = wipe_service_->get_algorithm_name(algo),
                          .description = wipe_service_->get_algorithm_description(algo),
                          .pass_count = wipe_service_->get_pass_count(algo),
                          .is_ssd_compatible = wipe_service_->is_ssd_compatible(algo)});
    }

    algorithms.set(algo_list);
}

void MainViewModel::update_can_wipe() {
    const auto& path = selected_disk_path.get();
    const auto valid = disk_service_->validate_device_path(path);
    // Allow wipe for mounted disks - we'll prompt to unmount in start_wipe()
    // Also require connection to the helper service
    bool can = is_connected.get() && !path.empty() && !is_wipe_in_progress.get() && valid;

    can_wipe.set(can);
    wipe_command->raise_can_execute_changed();
}

void MainViewModel::start_wipe() {
    const auto& path = selected_disk_path.get();

    if (path.empty()) {
        show_message(MessageInfo::Type::ERROR, "No Disk Selected", "Please select a disk to wipe.");
        return;
    }

    if (auto valid_path = disk_service_->validate_device_path(path); !valid_path) {
        auto message = std::string{"Selected device path is not valid or safe to wipe."};
        if (!valid_path.error().message.empty()) {
            message += "\n\nError: " + valid_path.error().message;
        }
        show_message(MessageInfo::Type::ERROR, "Invalid Device", message);
        return;
    }

    if (!disk_service_->is_disk_writable(path)) {
        show_message(MessageInfo::Type::ERROR, "Access Denied",
                     "Cannot write to selected disk. Make sure you have proper permissions.");
        return;
    }

    auto disk_info = find_disk_info(path);

    // Check if disk is mounted - offer to unmount first
    if (disk_info && disk_info->is_mounted) {
        std::ostringstream mounted_message;
        mounted_message << "The selected device is currently mounted";
        if (!disk_info->mount_point.empty()) {
            mounted_message << " at '" << disk_info->mount_point << "'";
        }
        if (disk_info->is_lvm_pv) {
            mounted_message << " (via LVM)";
        }
        mounted_message << ".\n\n";
        mounted_message << "Do you want to unmount the disk and proceed with wiping?\n\n";
        mounted_message << "Algorithm: "
                        << wipe_service_->get_algorithm_name(selected_algorithm.get()) << "\n";
        mounted_message << "WARNING: This will permanently destroy ALL data!";

        show_message(MessageInfo::Type::CONFIRMATION, "Unmount and Wipe?", mounted_message.str(),
                     [this, path](bool confirmed) {
                         if (confirmed) {
                             unmount_and_wipe(path);
                         }
                     });
        return;
    }

    // Show standard confirmation dialog for unmounted disks
    std::ostringstream message;
    message << "Are you sure you want to wipe '" << path << "'?\n\n";
    message << "Algorithm: " << wipe_service_->get_algorithm_name(selected_algorithm.get()) << "\n";
    message << "Description: " << wipe_service_->get_algorithm_description(selected_algorithm.get())
            << "\n\n";
    message << "WARNING: This will permanently destroy ALL data on the disk!\n";
    message << "This action cannot be undone!";

    show_message(MessageInfo::Type::CONFIRMATION, "Confirm Disk Wipe", message.str(),
                 [this](bool confirmed) {
                     if (confirmed) {
                         confirm_wipe();
                     }
                 });
}

void MainViewModel::confirm_wipe() {
    is_wipe_in_progress.set(true);
    update_can_wipe();

    auto progress_callback = [this](const WipeProgress& progress) {
        handle_wipe_progress(progress);
    };

    bool started = wipe_service_->wipe_disk(selected_disk_path.get(), selected_algorithm.get(),
                                            progress_callback);

    if (!started) {
        is_wipe_in_progress.set(false);
        update_can_wipe();
        show_message(MessageInfo::Type::ERROR, "Failed to Start",
                     "Could not start wipe operation. Another operation may be in progress.");
    }
}

void MainViewModel::unmount_and_wipe(const std::string& path) {
    // Try to unmount the disk first
    auto unmount_result = disk_service_->unmount_disk(path);

    if (!unmount_result) {
        std::ostringstream error_msg;
        error_msg << "Failed to unmount the disk.\n\n";
        error_msg << "Error: " << unmount_result.error().message << "\n\n";
        error_msg << "Please close any applications using the disk and try again,\n";
        error_msg << "or manually unmount using: sudo umount " << path;

        show_message(MessageInfo::Type::ERROR, "Unmount Failed", error_msg.str());
        return;
    }

    // Refresh disk list to update mount status
    load_disks();

    // Proceed with wipe confirmation
    std::ostringstream message;
    message << "Disk unmounted successfully!\n\n";
    message << "Are you sure you want to wipe '" << path << "'?\n\n";
    message << "Algorithm: " << wipe_service_->get_algorithm_name(selected_algorithm.get()) << "\n";
    message << "Description: " << wipe_service_->get_algorithm_description(selected_algorithm.get())
            << "\n\n";
    message << "WARNING: This will permanently destroy ALL data on the disk!\n";
    message << "This action cannot be undone!";

    show_message(MessageInfo::Type::CONFIRMATION, "Confirm Disk Wipe", message.str(),
                 [this](bool confirmed) {
                     if (confirmed) {
                         confirm_wipe();
                     }
                 });
}

void MainViewModel::handle_wipe_progress(const WipeProgress& progress) {
    // Schedule UI update on main thread using g_idle_add_full with destroy notify
    // to prevent memory leaks if the idle source is removed before execution
    auto weak_self = weak_from_this();
    auto* data = new std::pair<std::weak_ptr<MainViewModel>, WipeProgress*>(
        weak_self, new WipeProgress(progress));

    g_idle_add_full(
        G_PRIORITY_DEFAULT_IDLE,
        [](gpointer user_data) -> gboolean {
            auto* args =
                static_cast<std::pair<std::weak_ptr<MainViewModel>, WipeProgress*>*>(user_data);
            auto vm = args->first.lock();

            if (!vm) {
                return G_SOURCE_REMOVE;
            }

            vm->wipe_progress.set(*args->second);

            if (args->second->is_complete) {
                vm->is_wipe_in_progress.set(false);
                vm->update_can_wipe();

                if (args->second->has_error) {
                    vm->handle_wipe_completion(false, args->second->error_message);
                } else {
                    vm->handle_wipe_completion(true);
                }
            }

            return G_SOURCE_REMOVE;
        },
        data,
        [](gpointer user_data) {
            // Destroy notify - called when source is removed
            auto* args =
                static_cast<std::pair<std::weak_ptr<MainViewModel>, WipeProgress*>*>(user_data);
            delete args->second;
            delete args;
        });
}

void MainViewModel::handle_wipe_completion(bool success, const std::string& error_message) {
    if (success) {
        show_message(MessageInfo::Type::INFO, "Wipe Complete",
                     "Disk wipe operation completed successfully!");

        // Send desktop notification for successful completion
        if (notification_callback_) {
            notification_callback_("Wipe Complete", "Disk wipe operation completed successfully!",
                                   false);
        }
    } else {
        std::string message = "Wipe operation failed.";
        if (!error_message.empty()) {
            message += "\n\nError: " + error_message;
        }
        show_message(MessageInfo::Type::ERROR, "Wipe Failed", message);

        // Send desktop notification for failure
        if (notification_callback_) {
            notification_callback_("Wipe Failed",
                                   error_message.empty() ? "Wipe operation failed" : error_message,
                                   true);
        }
    }

    // Refresh disk list in case mount status changed
    load_disks();
}

void MainViewModel::show_message(MessageInfo::Type type, const std::string& title,
                                 const std::string& message, std::function<void(bool)> callback) {
    current_message.set(MessageInfo{.type = type,
                                    .title = title,
                                    .message = message,
                                    .confirmation_callback = std::move(callback)});
}

auto MainViewModel::find_disk_info(const std::string& path) const -> std::optional<DiskInfo> {
    auto disk_list = disks.get();
    auto it = std::find_if(disk_list.begin(), disk_list.end(),
                           [&path](const DiskInfo& disk) { return disk.path == path; });
    if (it != disk_list.end()) {
        return *it;
    }
    return std::nullopt;
}

void MainViewModel::set_connection_state(bool connected, const std::string& error_message) {
    is_connected.set(connected);
    connection_error.set(error_message);

    if (connected) {
        // Refresh disk list when we reconnect
        load_disks();
    }
}

void MainViewModel::set_notification_callback(NotificationCallback callback) {
    notification_callback_ = std::move(callback);
}
