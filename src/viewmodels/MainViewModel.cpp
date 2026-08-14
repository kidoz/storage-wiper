#include "viewmodels/MainViewModel.hpp"

#include <glibmm/main.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <format>
#include <thread>
#include <utility>

namespace {

auto format_size(uint64_t bytes) -> std::string {
    constexpr auto KIB = 1'024.0;
    constexpr auto MIB = KIB * 1'024.0;
    constexpr auto GIB = MIB * 1'024.0;
    constexpr auto TIB = GIB * 1'024.0;

    const auto size = static_cast<double>(bytes);
    if (size >= TIB) {
        return std::format("{:.2f} TB", size / TIB);
    }
    if (size >= GIB) {
        return std::format("{:.1f} GB", size / GIB);
    }
    if (size >= MIB) {
        return std::format("{:.1f} MB", size / MIB);
    }
    return std::format("{} bytes", bytes);
}

}  // namespace

MainViewModel::MainViewModel(std::shared_ptr<IDiskService> disk_service,
                             std::shared_ptr<IWipeService> wipe_service)
    : disk_service_(std::move(disk_service)), wipe_service_(std::move(wipe_service)) {
    // Initialize commands
    refresh_command = std::make_shared<mvvm::RelayCommand>([this]() { load_disks(); },
                                                           [this]() {
                                                               return is_connected.get() &&
                                                                      !is_wipe_in_progress.get() &&
                                                                      !is_operation_pending.get();
                                                           });

    wipe_command = std::make_shared<mvvm::RelayCommand>([this]() { start_wipe(); },
                                                        [this]() { return can_wipe.get(); });

    cancel_command = std::make_shared<mvvm::RelayCommand>(
        [this]() {
            show_message(MessageInfo::Type::INFO, "Cancelling",
                         "Wipe operation is being cancelled...");
            auto weak_self = weak_from_this();
            auto wipe_service = wipe_service_;
            std::thread([weak_self, wipe_service]() {
                const auto cancelled = wipe_service->cancel_current_operation();
                if (!cancelled) {
                    Glib::signal_idle().connect([weak_self]() {
                        if (auto vm = weak_self.lock()) {
                            vm->show_message(
                                MessageInfo::Type::ERROR, "Cancel Failed",
                                "The helper could not cancel the current wipe operation.");
                        }
                        return false;
                    });
                }
            }).detach();
        },
        [this]() { return is_wipe_in_progress.get(); });

    // Subscribe to property changes that affect can_wipe
    selected_disk_subscription_id_ = selected_disk_path.subscribe([this](const std::string&) {
        update_can_wipe();
        update_algorithm_state();
    });
    selected_algorithm_subscription_id_ =
        selected_algorithm.subscribe([this](WipeAlgorithm) { update_algorithm_state(); });
    wipe_in_progress_subscription_id_ = is_wipe_in_progress.subscribe([this](bool) {
        update_can_wipe();
        refresh_command->raise_can_execute_changed();
        wipe_command->raise_can_execute_changed();
        cancel_command->raise_can_execute_changed();
    });
    operation_pending_subscription_id_ = is_operation_pending.subscribe([this](bool) {
        update_can_wipe();
        refresh_command->raise_can_execute_changed();
        wipe_command->raise_can_execute_changed();
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
    update_algorithm_state();
    update_can_wipe();
}

void MainViewModel::cleanup() {
    // Unsubscribe from observables
    selected_disk_path.unsubscribe(selected_disk_subscription_id_);
    selected_algorithm.unsubscribe(selected_algorithm_subscription_id_);
    is_wipe_in_progress.unsubscribe(wipe_in_progress_subscription_id_);
    is_operation_pending.unsubscribe(operation_pending_subscription_id_);
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
        is_disk_refreshing.set(false);
        disks.set({});
        update_can_wipe();
        return;
    }

    is_disk_refreshing.set(true);
    auto weak_self = weak_from_this();
    disk_service_->get_available_disks(
        [weak_self](std::expected<std::vector<DiskInfo>, util::Error> result) {
            // Schedule update on main thread
            Glib::signal_idle().connect([weak_self, result = std::move(result)]() {
                if (auto vm = weak_self.lock()) {
                    vm->is_disk_refreshing.set(false);
                    if (result) {
                        vm->disks.set(*result);

                        // Clear selection if previously selected disk is no longer available
                        const auto current_selection = vm->selected_disk_path.get();
                        if (!current_selection.empty()) {
                            bool found = false;
                            for (const auto& disk : *result) {
                                if (disk.path == current_selection) {
                                    found = true;
                                    break;
                                }
                            }
                            if (!found) {
                                vm->selected_disk_path.set("");
                            }
                        }
                        vm->update_can_wipe();
                        vm->update_algorithm_state();
                    } else {
                        vm->disks.set({});
                        vm->show_message(MessageInfo::Type::ERROR, "Error",
                                         "Failed to refresh disk list: " + result.error().message);
                    }
                }
                return false;  // G_SOURCE_REMOVE
            });
        });
}

void MainViewModel::load_algorithms() {
    std::vector<AlgorithmInfo> algo_list;

    // Get algorithm info from WipeService
    constexpr std::array all_algorithms = {
        WipeAlgorithm::ZERO_FILL, WipeAlgorithm::RANDOM_FILL,     WipeAlgorithm::DOD_5220_22_M,
        WipeAlgorithm::SCHNEIER,  WipeAlgorithm::VSITR,           WipeAlgorithm::GOST_R_50739_95,
        WipeAlgorithm::GUTMANN,   WipeAlgorithm::ATA_SECURE_ERASE};

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
    const auto path = selected_disk_path.get();
    bool can = is_connected.get() && !path.empty() && !is_wipe_in_progress.get() &&
               !is_operation_pending.get();

    can_wipe.set(can);
    wipe_command->raise_can_execute_changed();
}

void MainViewModel::update_algorithm_state() {
    const auto algorithm = selected_algorithm.get();
    const bool supports_verification = wipe_service_->supports_verification(algorithm);
    verification_available.set(supports_verification);
    if (!supports_verification && verification_enabled.get()) {
        verification_enabled.set(false);
    }

    const auto selected_path = selected_disk_path.get();
    const auto disk_info = find_disk_info(selected_path);
    if (disk_info && disk_info->is_ssd && !wipe_service_->is_ssd_compatible(algorithm)) {
        algorithm_warning.set(
            "This multi-pass algorithm is designed for magnetic disks and is not recommended for "
            "SSDs. Prefer ATA Secure Erase, Zero Fill, or Random Fill when supported.");
    } else {
        algorithm_warning.set("");
    }
}

void MainViewModel::start_wipe() {
    const auto path = selected_disk_path.get();

    if (path.empty()) {
        show_message(MessageInfo::Type::ERROR, "No Disk Selected", "Please select a disk to wipe.");
        return;
    }

    auto disk_info = find_disk_info(path);
    if (!disk_info) {
        show_message(
            MessageInfo::Type::ERROR, "Disk Not Found",
            "The selected disk is no longer in the device list. Refresh and select it again.");
        return;
    }

    const auto algorithm = selected_algorithm.get();
    const auto verify = verification_enabled.get() && verification_available.get();
    const auto algorithm_name = wipe_service_->get_algorithm_name(algorithm);
    const auto algorithm_description = wipe_service_->get_algorithm_description(algorithm);
    const auto disk_summary = build_disk_summary(disk_info, path);
    const auto warning = algorithm_warning.get();

    // Check if disk is mounted - offer to unmount first
    if (disk_info && disk_info->is_mounted) {
        std::ostringstream mounted_message;
        mounted_message << "The selected device is currently mounted and must be unmounted before "
                           "wiping.\n\n";
        mounted_message << disk_summary << "\n\n";
        mounted_message << "Algorithm: " << algorithm_name << "\n";
        mounted_message << "Verification: " << (verify ? "enabled" : "disabled") << "\n\n";
        if (!warning.empty()) {
            mounted_message << warning << "\n\n";
        }
        mounted_message << "Unmounting may close access to mounted filesystems. Wiping will "
                           "permanently destroy ALL data.";

        auto weak_self = weak_from_this();
        show_message(MessageInfo::Type::CONFIRMATION, "Unmount and Wipe?", mounted_message.str(),
                     [weak_self, path](bool confirmed) {
                         if (confirmed) {
                             if (auto vm = weak_self.lock()) {
                                 vm->unmount_and_wipe(path);
                             }
                         }
                     });
        return;
    }

    // Show standard confirmation dialog for unmounted disks
    std::ostringstream message;
    message << "Are you sure you want to wipe this storage device?\n\n";
    message << disk_summary << "\n\n";
    message << "Algorithm: " << algorithm_name << "\n";
    message << "Description: " << algorithm_description << "\n";
    message << "Verification: " << (verify ? "enabled" : "disabled") << "\n\n";
    if (!warning.empty()) {
        message << warning << "\n\n";
    }
    message << "WARNING: This will permanently destroy ALL data on the disk!\n";
    message << "This action cannot be undone!";

    auto weak_self = weak_from_this();
    show_message(MessageInfo::Type::CONFIRMATION, "Confirm Disk Wipe", message.str(),
                 [weak_self, path, algorithm, verify](bool confirmed) {
                     if (confirmed) {
                         if (auto vm = weak_self.lock()) {
                             vm->confirm_wipe_for(path, algorithm, verify);
                         }
                     }
                 });
}

void MainViewModel::confirm_wipe() {
    confirm_wipe_for(selected_disk_path.get(), selected_algorithm.get(),
                     verification_enabled.get() && verification_available.get());
}

void MainViewModel::confirm_wipe_for(const std::string& path, WipeAlgorithm algorithm,
                                     bool verify) {
    active_wipe_disk_path_ = path;
    active_wipe_algorithm_ = algorithm;
    active_wipe_verification_enabled_ = verify;
    is_wipe_in_progress.set(true);
    update_can_wipe();

    auto weak_self = weak_from_this();
    auto progress_callback = [weak_self](const WipeProgress& progress) {
        if (auto vm = weak_self.lock()) {
            vm->handle_wipe_progress(progress);
        }
    };

    auto wipe_service = wipe_service_;
    std::thread([weak_self, wipe_service, path, algorithm,
                 progress_callback = std::move(progress_callback), verify]() mutable {
        const bool started =
            wipe_service->wipe_disk(path, algorithm, std::move(progress_callback), verify);

        if (!started) {
            Glib::signal_idle().connect([weak_self]() {
                if (auto vm = weak_self.lock()) {
                    vm->is_wipe_in_progress.set(false);
                    vm->update_can_wipe();
                    vm->show_message(
                        MessageInfo::Type::ERROR, "Failed to Start",
                        "Could not start the wipe operation. The helper may have rejected the "
                        "device, "
                        "authorization may have failed, or another operation may be in progress.");
                }
                return false;
            });
        }
    }).detach();
}

void MainViewModel::unmount_and_wipe(const std::string& path) {
    is_operation_pending.set(true);
    update_can_wipe();

    const auto algorithm = selected_algorithm.get();
    const auto verify = verification_enabled.get() && verification_available.get();
    auto weak_self = weak_from_this();
    auto disk_service = disk_service_;

    std::thread([weak_self, disk_service, path, algorithm, verify]() {
        auto unmount_result = disk_service->unmount_disk(path);

        Glib::signal_idle().connect([weak_self, path, algorithm, verify,
                                     unmount_result = std::move(unmount_result)]() mutable {
            if (auto vm = weak_self.lock()) {
                vm->is_operation_pending.set(false);
                vm->update_can_wipe();

                if (!unmount_result) {
                    std::ostringstream error_msg;
                    error_msg << "Failed to unmount the disk.\n\n";
                    error_msg << "Error: " << unmount_result.error().message << "\n\n";
                    error_msg << "Close applications using the disk and try again, or manually "
                                 "unmount it before wiping.";

                    vm->show_message(MessageInfo::Type::ERROR, "Unmount Failed", error_msg.str());
                    return false;
                }

                vm->load_disks();

                const auto disk_info = vm->find_disk_info(path);
                const auto disk_summary = vm->build_disk_summary(disk_info, path);
                const auto algorithm_name = vm->wipe_service_->get_algorithm_name(algorithm);
                const auto algorithm_description =
                    vm->wipe_service_->get_algorithm_description(algorithm);
                const auto warning = vm->algorithm_warning.get();

                std::ostringstream message;
                message << "Disk unmounted successfully.\n\n";
                message << disk_summary << "\n\n";
                message << "Algorithm: " << algorithm_name << "\n";
                message << "Description: " << algorithm_description << "\n";
                message << "Verification: " << (verify ? "enabled" : "disabled") << "\n\n";
                if (!warning.empty()) {
                    message << warning << "\n\n";
                }
                message << "WARNING: This will permanently destroy ALL data on the disk!\n";
                message << "This action cannot be undone!";

                vm->show_message(MessageInfo::Type::CONFIRMATION, "Confirm Disk Wipe",
                                 message.str(),
                                 [weak_self, path, algorithm, verify](bool confirmed) {
                                     if (confirmed) {
                                         if (auto callback_vm = weak_self.lock()) {
                                             callback_vm->confirm_wipe_for(path, algorithm, verify);
                                         }
                                     }
                                 });
            }
            return false;
        });
    }).detach();
}

void MainViewModel::handle_wipe_progress(const WipeProgress& progress) {
    // Schedule UI update on main thread using Glib::signal_idle
    auto weak_self = weak_from_this();
    Glib::signal_idle().connect([weak_self, progress]() {
        if (auto vm = weak_self.lock()) {
            vm->wipe_progress.set(progress);

            if (progress.is_complete) {
                vm->is_wipe_in_progress.set(false);
                vm->update_can_wipe();

                if (progress.has_error) {
                    vm->handle_wipe_completion(false, progress.error_message);
                } else {
                    vm->handle_wipe_completion(true);
                }
            }
        }
        return false;  // G_SOURCE_REMOVE
    });
}

void MainViewModel::handle_wipe_completion(bool success, const std::string& error_message) {
    const auto disk_info = find_disk_info(active_wipe_disk_path_);
    const auto disk_summary = build_disk_summary(disk_info, active_wipe_disk_path_);
    const auto algorithm_name = wipe_service_->get_algorithm_name(active_wipe_algorithm_);

    if (success) {
        std::ostringstream message;
        message << "Disk wipe operation completed successfully.\n\n";
        message << disk_summary << "\n\n";
        message << "Algorithm: " << algorithm_name << "\n";
        message << "Verification: "
                << (active_wipe_verification_enabled_ ? "completed successfully" : "not enabled");

        show_message(MessageInfo::Type::INFO, "Wipe Complete", message.str());

        // Send desktop notification for successful completion
        if (notification_callback_) {
            notification_callback_("Wipe Complete", "Finished wiping " + active_wipe_disk_path_,
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
    // The sequence makes consecutive identical dialogs compare unequal so
    // Observable::set() never swallows the notification (see MessageInfo).
    static std::atomic<uint64_t> next_sequence{1};
    current_message.set(MessageInfo{.type = type,
                                    .title = title,
                                    .message = message,
                                    .confirmation_callback = std::move(callback),
                                    .sequence = next_sequence.fetch_add(1)});
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

auto MainViewModel::build_disk_summary(const std::optional<DiskInfo>& disk_info,
                                       const std::string& fallback_path) const -> std::string {
    if (!disk_info) {
        return "Device: " + fallback_path;
    }

    std::ostringstream summary;
    summary << "Device: " << disk_info->path << "\n";
    summary << "Model: " << (disk_info->model.empty() ? "Unknown" : disk_info->model) << "\n";
    summary << "Serial: " << (disk_info->serial.empty() ? "Unknown" : disk_info->serial) << "\n";
    summary << "Size: " << format_size(disk_info->size_bytes) << "\n";
    summary << "Type: " << (disk_info->is_ssd ? "SSD" : "HDD");
    if (disk_info->is_removable) {
        summary << ", removable";
    }
    if (!disk_info->filesystem.empty()) {
        summary << "\nFilesystem: " << disk_info->filesystem;
    }
    if (disk_info->is_mounted) {
        summary << "\nMount: "
                << (disk_info->mount_point.empty() ? "mounted" : disk_info->mount_point);
    }
    if (disk_info->is_lvm_pv) {
        summary << "\nLVM: physical volume";
    }

    return summary.str();
}

void MainViewModel::set_connection_state(bool connected, const std::string& error_message) {
    is_connected.set(connected);
    connection_error.set(error_message);

    if (connected) {
        // Refresh disk list when we reconnect
        load_disks();
    } else {
        is_disk_refreshing.set(false);
        disks.set({});
        selected_disk_path.set("");
    }
}

void MainViewModel::set_notification_callback(NotificationCallback callback) {
    notification_callback_ = std::move(callback);
}
