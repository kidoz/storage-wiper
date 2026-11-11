#include "viewmodels/MainViewModel.hpp"

#include "util/WipePlanDisplay.hpp"

#include <glibmm/main.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
#include <thread>
#include <utility>

#include "config.h"

namespace {

void queue_main(std::function<bool()> callback) {
    auto* data = new std::function<bool()>(std::move(callback));
    g_idle_add_full(
        G_PRIORITY_DEFAULT_IDLE,
        [](gpointer user_data) -> gboolean {
            const auto* callback = static_cast<const std::function<bool()>*>(user_data);
            return (*callback)() ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
        },
        data, [](gpointer user_data) { delete static_cast<std::function<bool()>*>(user_data); });
}

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
    refresh_command = std::make_shared<mvvm::RelayCommand>(
        [this]() { load_disks(); },
        [this]() { return is_connected.get() && !is_operation_pending.get(); });

    wipe_command = std::make_shared<mvvm::RelayCommand>([this]() { start_wipe(); },
                                                        [this]() { return can_wipe.get(); });

    cancel_command = std::make_shared<mvvm::RelayCommand>(
        [this]() {
            const auto path = selected_disk_path.get();
            const auto activity = active_wipes_.find(path);
            if (activity == active_wipes_.end() || !activity->second.plan.firmware.cancellable)
                return;
            const auto plan_id = activity->second.plan.id;
            show_message(MessageInfo::Type::INFO, "Cancelling",
                         "Wipe operation on " + path + " is being cancelled...");
            auto weak_self = weak_from_this();
            auto wipe_service = wipe_service_;
            std::thread([weak_self, wipe_service, plan_id]() {
                const auto cancelled = wipe_service->cancel_prepared_wipe(plan_id);
                if (!cancelled) {
                    queue_main([weak_self]() {
                        if (auto vm = weak_self.lock(); vm && !vm->cleaned_up_.load()) {
                            vm->show_message(MessageInfo::Type::ERROR, "Cancel Failed",
                                             "The helper could not cancel the wipe operation.");
                        }
                        return false;
                    });
                }
            }).detach();
        },
        [this]() {
            const auto path = selected_disk_path.get();
            const auto activity = active_wipes_.find(path);
            return activity != active_wipes_.end() && activity->second.plan.firmware.cancellable;
        });

    // Subscribe to property changes that affect can_wipe
    selected_disk_subscription_id_ = selected_disk_path.subscribe([this](const std::string& path) {
        update_can_wipe();
        update_algorithm_state();
        // The single progress area follows the selected device
        const auto& progresses = wipe_progresses.get();
        if (const auto it = progresses.find(path); it != progresses.end()) {
            wipe_progress.set(it->second);
        } else {
            wipe_progress.set(WipeProgress{});
        }
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
    if (cleaned_up_.exchange(true)) {
        return;
    }
    ++preparation_generation_;
    ++connection_generation_;
    std::vector<std::string> plans;
    if (pending_plan_) {
        plans.push_back(pending_plan_->id);
        pending_plan_.reset();
    }
    for (const auto& [path, activity] : active_wipes_) {
        plans.push_back(activity.plan.id);
    }
    if (!plans.empty()) {
        auto service = wipe_service_;
        std::thread([service, plans = std::move(plans)]() {
            for (const auto& id : plans)
                service->discard_wipe_plan(id);
        }).detach();
    }
    // Unsubscribe from observables
    selected_disk_path.unsubscribe(selected_disk_subscription_id_);
    selected_algorithm.unsubscribe(selected_algorithm_subscription_id_);
    is_wipe_in_progress.unsubscribe(wipe_in_progress_subscription_id_);
    is_operation_pending.unsubscribe(operation_pending_subscription_id_);
    is_connected.unsubscribe(connection_subscription_id_);
}

auto MainViewModel::has_pending_or_active_operations() const -> bool {
    return is_operation_pending.get() || !active_wipes_.empty();
}

auto MainViewModel::has_uncancellable_operations() const -> bool {
    return std::ranges::any_of(
        active_wipes_, [](const auto& entry) { return !entry.second.plan.firmware.cancellable; });
}

void MainViewModel::stop_for_close() {
    if (cleaned_up_.load())
        return;
    ++preparation_generation_;
    std::vector<std::pair<WipePlan, bool>> plans;
    if (pending_plan_) {
        plans.emplace_back(*pending_plan_, false);
        pending_plan_.reset();
        is_operation_pending.set(false);
    }
    if (unmount_confirmation_pending_) {
        unmount_confirmation_pending_ = false;
        is_operation_pending.set(false);
    }
    for (const auto& [path, activity] : active_wipes_)
        plans.emplace_back(activity.plan, true);
    auto service = wipe_service_;
    auto weak_self = weak_from_this();
    std::thread([service, weak_self, plans = std::move(plans)]() {
        bool failed = false;
        for (const auto& [plan, active] : plans) {
            service->discard_wipe_plan(plan.id);
            if (active && plan.firmware.cancellable) {
                failed = !service->cancel_prepared_wipe(plan.id) || failed;
            }
        }
        if (failed) {
            queue_main([weak_self]() {
                if (auto vm = weak_self.lock();
                    vm && !vm->cleaned_up_.load() && vm->has_pending_or_active_operations()) {
                    vm->show_message(MessageInfo::Type::ERROR, "Cancellation Not Confirmed",
                                     "Some work could not be cancelled. Keep monitoring until the "
                                     "helper reports its final outcome.");
                }
                return false;
            });
        }
    }).detach();
}

void MainViewModel::select_disk(const std::string& disk_path) {
    selected_disk_path.set(disk_path);
}

void MainViewModel::select_algorithm(WipeAlgorithm algorithm) {
    selected_algorithm.set(algorithm);
}

void MainViewModel::restore_wipe_jobs() {
    auto weak_self = weak_from_this();
    auto service = wipe_service_;
    const auto generation = connection_generation_;
    std::thread([weak_self, service, generation]() {
        auto jobs = service->get_wipe_jobs();
        queue_main([weak_self, service, generation, jobs = std::move(jobs)]() {
            auto vm = weak_self.lock();
            if (!vm || vm->cleaned_up_.load() || !vm->is_connected.get() ||
                vm->connection_generation_ != generation)
                return false;
            if (!jobs) {
                vm->show_message(MessageInfo::Type::ERROR, "Cannot Restore Wipe Jobs",
                                 jobs.error().message);
                return false;
            }
            for (const auto& job : *jobs) {
                if (vm->completed_job_ids_.contains(job.plan.id))
                    continue;
                const auto& path = job.plan.device.disk.path;
                WipeActivity activity;
                activity.plan = job.plan;
                activity.algorithm = job.plan.algorithm;
                activity.verify = job.plan.verify;
                activity.started_at = std::chrono::system_clock::time_point{
                    std::chrono::seconds{job.started_at_unix}};
                const auto elapsed =
                    std::max(std::chrono::system_clock::duration::zero(),
                             std::chrono::system_clock::now() - activity.started_at);
                activity.started_steady = std::chrono::steady_clock::now() - elapsed;
                if (job.completed_at_unix > 0)
                    activity.completed_at = std::chrono::system_clock::time_point{
                        std::chrono::seconds{job.completed_at_unix}};
                activity.peak_speed = job.progress.speed_bytes_per_sec;
                const auto active = vm->active_wipes_.find(path);
                if (job.progress.is_complete) {
                    // History must never occupy a reusable path and hide a newer active job.
                    if (active != vm->active_wipes_.end() &&
                        active->second.plan.id == job.plan.id) {
                        active->second.completed_at = activity.completed_at;
                        vm->finish_wipe(path, job.progress);
                    } else {
                        vm->completed_job_ids_.insert(job.plan.id);
                        vm->handle_wipe_completion(path, activity, job.progress);
                    }
                    continue;
                }
                if (active != vm->active_wipes_.end())
                    continue;
                vm->active_wipes_.emplace(path, std::move(activity));
                vm->is_wipe_in_progress.set(true);
                vm->cancel_command->raise_can_execute_changed();
                // Queue the snapshot before attaching: buffered newer signals must win.
                vm->handle_wipe_progress(path, job.plan.id, job.progress);
                if (!job.progress.is_complete) {
                    if (!service->observe_wipe_job(job.plan, [weak_self, path, id = job.plan.id](
                                                                 const WipeProgress& progress) {
                            if (auto owner = weak_self.lock(); owner && !owner->cleaned_up_.load())
                                owner->handle_wipe_progress(path, id, progress);
                        })) {
                        WipeProgress failure;
                        failure.is_complete = true;
                        failure.has_error = true;
                        failure.outcome_known = false;
                        failure.error_message =
                            "Could not attach to the helper job; its outcome is unknown.";
                        vm->handle_wipe_progress(path, job.plan.id, failure);
                    }
                }
            }
            return false;
        });
    }).detach();
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
            queue_main([weak_self, result = std::move(result)]() {
                if (auto vm = weak_self.lock(); vm && !vm->cleaned_up_.load()) {
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
                          .is_ssd_compatible = wipe_service_->is_ssd_compatible(algo),
                          .nist_category = wipe_service_->get_nist_category(algo)});
    }

    algorithms.set(algo_list);
}

void MainViewModel::update_can_wipe() {
    const auto path = selected_disk_path.get();
    // Other devices may be wiping in parallel; only the selected device being
    // wiped disables its own wipe button.
    bool can = is_connected.get() && !path.empty() && !is_operation_pending.get() &&
               active_wipes_.count(path) == 0;

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
            "SSDs. Prefer Hardware Secure Erase, Zero Fill, or Random Fill when supported.");
    } else {
        algorithm_warning.set("");
    }
}

void MainViewModel::start_wipe() {
    if (cleaned_up_.load()) {
        return;
    }
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
    if (disk_info->is_partition && algorithm == WipeAlgorithm::ATA_SECURE_ERASE) {
        show_message(MessageInfo::Type::ERROR, "Whole Disk Required",
                     "Hardware secure erase cannot target a partition. Select a whole disk "
                     "or use an overwrite algorithm.");
        return;
    }
    const auto algorithm_name = wipe_service_->get_algorithm_name(algorithm);
    const auto disk_summary = build_disk_summary(disk_info, path);
    // Check if disk is mounted - offer to unmount first
    if (disk_info && disk_info->is_mounted) {
        std::ostringstream mounted_message;
        mounted_message << "The selected device is currently mounted and must be unmounted before "
                           "wiping.\n\n";
        mounted_message << disk_summary << "\n\n";
        mounted_message << "Algorithm: " << algorithm_name << "\n";
        mounted_message << "Verification: " << (verify ? "enabled" : "disabled") << "\n\n";
        mounted_message << "Unmounting may close access to mounted filesystems. Wiping will "
                           "permanently destroy ALL data.";

        auto weak_self = weak_from_this();
        const auto generation = preparation_generation_;
        unmount_confirmation_pending_ = true;
        is_operation_pending.set(true);
        show_message(MessageInfo::Type::CONFIRMATION, "Unmount and Wipe?", mounted_message.str(),
                     [weak_self, path, algorithm, verify, generation](bool confirmed) {
                         if (auto vm = weak_self.lock();
                             vm && !vm->cleaned_up_.load() &&
                             vm->preparation_generation_ == generation &&
                             vm->unmount_confirmation_pending_) {
                             vm->unmount_confirmation_pending_ = false;
                             if (confirmed)
                                 vm->unmount_and_wipe(path, algorithm, verify);
                             else
                                 vm->is_operation_pending.set(false);
                         }
                     });
        return;
    }

    prepare_wipe(path, algorithm, verify);
}

void MainViewModel::prepare_wipe(const std::string& path, WipeAlgorithm algorithm, bool verify) {
    if (cleaned_up_.load()) {
        return;
    }
    is_operation_pending.set(true);
    auto weak_self = weak_from_this();
    auto service = wipe_service_;
    const auto generation = preparation_generation_;
    std::thread([weak_self, service, path, algorithm, verify, generation]() {
        auto result = service->prepare_wipe(path, algorithm, verify);
        queue_main([weak_self, service, generation, result = std::move(result)]() mutable {
            auto vm = weak_self.lock();
            if (!vm || vm->cleaned_up_.load() || vm->preparation_generation_ != generation) {
                if (vm && !vm->cleaned_up_.load())
                    vm->is_operation_pending.set(false);
                if (result) {
                    std::thread([service, id = result->id]() {
                        service->discard_wipe_plan(id);
                    }).detach();
                }
                return false;
            }
            if (!result) {
                vm->is_operation_pending.set(false);
                vm->show_message(MessageInfo::Type::ERROR, "Cannot Prepare Wipe",
                                 result.error().message);
                return false;
            }
            const auto plan = *result;
            vm->pending_plan_ = plan;
            std::ostringstream message;
            message << vm->build_disk_summary(plan.device.disk, plan.device.disk.path) << "\n\n";
            message << "Algorithm: " << service->get_algorithm_name(plan.algorithm) << "\n";
            message << "Method: " << util::wipe_method_label(plan) << "\n";
            message << "Scope: " << util::wipe_scope_label(plan) << "\n";
            if (!plan.firmware.cancellable) {
                message << "This operation cannot be stopped after the device accepts it.\n";
            }
            message << "Verification: " << (plan.verify ? "enabled" : "disabled") << "\n";
            message << "Affected devices:\n";
            for (const auto& target : plan.targets) {
                message << vm->build_disk_summary(target.disk, target.disk.path) << "\n";
            }
            if (plan.device.disk.is_lvm_pv) {
                message
                    << "WARNING: This device contains LVM physical-volume metadata. Logical "
                       "volumes and volume groups may span other disks; their data may be lost.\n";
            } else if (plan.device.disk.has_dm_holders) {
                message << "WARNING: This is a device-mapper backing device (for example, an "
                           "encrypted volume). Dependent volumes may lose data.\n";
            }
            message << "\nWARNING: This will permanently destroy ALL data in the scope "
                       "above!\nThis action cannot be undone!";
            vm->show_message(MessageInfo::Type::CONFIRMATION, "Confirm Disk Wipe", message.str(),
                             [weak_self, service, plan](bool confirmed) {
                                 if (auto owner = weak_self.lock();
                                     owner && !owner->cleaned_up_.load() && owner->pending_plan_ &&
                                     owner->pending_plan_->id == plan.id) {
                                     owner->pending_plan_.reset();
                                     owner->is_operation_pending.set(false);
                                     if (confirmed) {
                                         owner->confirm_wipe_for(plan);
                                         return;
                                     }
                                 }
                                 std::thread([service, id = plan.id]() {
                                     service->discard_wipe_plan(id);
                                 }).detach();
                             });
            return false;
        });
    }).detach();
}

auto MainViewModel::build_scope_note(const DiskInfo& disk,
                                     const std::vector<std::string>& child_partitions)
    -> std::string {
    if (disk.is_partition) {
        if (disk.parent_disk.empty()) {
            return {};
        }
        return std::format("Scope: only this partition is erased. Other partitions and the "
                           "partition table on {} are not touched.",
                           disk.parent_disk);
    }
    if (child_partitions.empty()) {
        return {};
    }
    std::string joined;
    for (size_t i = 0; i < child_partitions.size(); ++i) {
        if (i > 0) {
            joined += ", ";
        }
        joined += child_partitions[i];
    }
    return std::format("Scope: the whole device is erased, including its partitions ({}) and "
                       "the partition table.",
                       joined);
}

void MainViewModel::confirm_wipe() {
    if (cleaned_up_.load() || !pending_plan_) {
        return;
    }
    const auto plan = *pending_plan_;
    pending_plan_.reset();
    is_operation_pending.set(false);
    confirm_wipe_for(plan);
}

void MainViewModel::confirm_wipe_for(const WipePlan& plan) {
    if (cleaned_up_.load()) {
        auto service = wipe_service_;
        std::thread([service, id = plan.id]() { service->discard_wipe_plan(id); }).detach();
        return;
    }
    const auto& path = plan.device.disk.path;
    const auto algorithm = plan.algorithm;
    const auto verify = plan.verify;
    WipeActivity activity;
    activity.plan = plan;
    activity.algorithm = algorithm;
    activity.verify = verify;
    activity.started_at = std::chrono::system_clock::now();
    activity.started_steady = std::chrono::steady_clock::now();
    activity.peak_speed = 0;
    if (!active_wipes_.try_emplace(path, activity).second) {
        auto service = wipe_service_;
        std::thread([service, id = plan.id]() { service->discard_wipe_plan(id); }).detach();
        return;
    }
    is_wipe_in_progress.set(true);
    update_can_wipe();
    cancel_command->raise_can_execute_changed();

    auto weak_self = weak_from_this();
    auto progress_callback = [weak_self, path, plan_id = plan.id](const WipeProgress& progress) {
        if (auto vm = weak_self.lock(); vm && !vm->cleaned_up_.load()) {
            vm->handle_wipe_progress(path, plan_id, progress);
        }
    };

    auto wipe_service = wipe_service_;
    std::thread([weak_self, wipe_service, path, algorithm, plan,
                 progress_callback = std::move(progress_callback), verify]() mutable {
        {
            auto owner = weak_self.lock();
            if (!owner || owner->cleaned_up_.load()) {
                wipe_service->discard_wipe_plan(plan.id);
                return;
            }
        }
        const bool started = wipe_service->start_prepared_wipe(plan, std::move(progress_callback));

        if (started) {
            queue_main([weak_self, algorithm, verify]() {
                if (auto vm = weak_self.lock();
                    vm && !vm->cleaned_up_.load() && vm->settings_save_callback_) {
                    vm->settings_save_callback_(
                        util::AppSettingsData{.algorithm_id = static_cast<int>(algorithm),
                                              .verification_enabled = verify});
                }
                return false;
            });
        } else {
            wipe_service->discard_wipe_plan(plan.id);
            queue_main([weak_self, path]() {
                if (auto vm = weak_self.lock(); vm && !vm->cleaned_up_.load()) {
                    if (vm->active_wipes_.erase(path) == 0) {
                        return false;
                    }
                    vm->is_wipe_in_progress.set(!vm->active_wipes_.empty());
                    vm->update_can_wipe();
                    vm->cancel_command->raise_can_execute_changed();
                    vm->show_message(
                        MessageInfo::Type::ERROR, "Failed to Start",
                        "Could not start the wipe operation on " + path +
                            ". The helper may have rejected the device, "
                            "authorization may have failed, the prepared plan may have expired, "
                            "or a wipe may already be running on it. Prepare and confirm a new "
                            "wipe to retry.");
                }
                return false;
            });
        }
    }).detach();
}

void MainViewModel::unmount_and_wipe(const std::string& path, WipeAlgorithm algorithm,
                                     bool verify) {
    if (cleaned_up_.load()) {
        return;
    }
    is_operation_pending.set(true);
    update_can_wipe();

    auto weak_self = weak_from_this();
    auto disk_service = disk_service_;
    const auto generation = preparation_generation_;

    std::thread([weak_self, disk_service, path, algorithm, verify, generation]() {
        auto unmount_result = disk_service->unmount_disk(path);

        queue_main([weak_self, path, algorithm, verify, generation,
                    unmount_result = std::move(unmount_result)]() mutable {
            if (auto vm = weak_self.lock(); vm && !vm->cleaned_up_.load()) {
                vm->is_operation_pending.set(false);
                vm->update_can_wipe();
                if (vm->preparation_generation_ != generation)
                    return false;

                if (!unmount_result) {
                    std::ostringstream error_msg;
                    error_msg << "Failed to unmount the disk.\n\n";
                    error_msg << "Error: " << unmount_result.error().message << "\n\n";
                    error_msg << "Close applications using the disk and try again, or manually "
                                 "unmount it before wiping.";

                    vm->show_message(MessageInfo::Type::ERROR, "Unmount Failed", error_msg.str());
                    return false;
                }

                vm->prepare_wipe(path, algorithm, verify);
            }
            return false;
        });
    }).detach();
}

void MainViewModel::handle_wipe_progress(const std::string& path, const std::string& plan_id,
                                         const WipeProgress& progress) {
    // Schedule UI update on main thread using Glib::signal_idle
    auto weak_self = weak_from_this();
    queue_main([weak_self, path, plan_id, progress]() {
        if (auto vm = weak_self.lock(); vm && !vm->cleaned_up_.load()) {
            const auto activity = vm->active_wipes_.find(path);
            if (activity == vm->active_wipes_.end() || activity->second.plan.id != plan_id) {
                return false;
            }
            activity->second.peak_speed =
                std::max(activity->second.peak_speed, progress.speed_bytes_per_sec);
            auto progresses = vm->wipe_progresses.get();
            progresses[path] = progress;
            vm->wipe_progresses.set(std::move(progresses));

            if (path == vm->selected_disk_path.get()) {
                vm->wipe_progress.set(progress);
            }

            if (progress.is_complete) {
                vm->finish_wipe(path, progress);
            }
        }
        return false;  // G_SOURCE_REMOVE
    });
}

void MainViewModel::finish_wipe(const std::string& path, const WipeProgress& progress) {
    const auto activity_it = active_wipes_.find(path);
    const WipeActivity activity =
        activity_it != active_wipes_.end() ? activity_it->second : WipeActivity{};
    active_wipes_.erase(path);
    if (progress.outcome_known)
        completed_job_ids_.insert(activity.plan.id);

    auto progresses = wipe_progresses.get();
    progresses.erase(path);
    wipe_progresses.set(std::move(progresses));

    is_wipe_in_progress.set(!active_wipes_.empty());
    update_can_wipe();
    cancel_command->raise_can_execute_changed();

    handle_wipe_completion(path, activity, progress);
}

void MainViewModel::handle_wipe_completion(const std::string& path, const WipeActivity& activity,
                                           const WipeProgress& progress) {
    const auto& disk_info = activity.plan.device.disk;
    const auto disk_summary = build_disk_summary(disk_info, path);
    const auto algorithm_name = wipe_service_->get_algorithm_name(activity.algorithm);

    if (wipe_succeeded(progress)) {
        std::ostringstream message;
        message << "Wipe of " << path << " completed successfully.\n\n";
        message << disk_summary << "\n\n";
        message << "Method: " << util::wipe_method_label(activity.plan) << "\n";
        message << "Scope: " << util::wipe_scope_label(activity.plan) << "\n";
        message << "Algorithm: " << algorithm_name << "\n";
        message << "Verification: ";
        // What the helper reported, not what was requested: an algorithm that
        // cannot verify reports it back as disabled.
        if (progress.verification_enabled) {
            message << (progress.verification_passed ? "completed successfully" : "FAILED");
        } else {
            message << "not enabled";
        }
        if (progress.bad_block_count > 0) {
            message << "\n\nWarning: " << progress.bad_block_count
                    << " bad sectors could not be overwritten; data in them may survive.";
        }

        if (!certificate_directory_.empty()) {
            // The plan UUID names one audit record across reconnects. Restrict the
            // filename component even for alternate service implementations.
            auto record_id = activity.plan.id;
            for (auto& character : record_id) {
                if (!std::isalnum(static_cast<unsigned char>(character)) && character != '-' &&
                    character != '_')
                    character = '_';
            }
            const auto data = build_certificate_data(path, activity, progress);
            const auto certificate =
                record_id.empty()
                    ? util::write_wipe_certificate(certificate_directory_, data)
                    : util::write_certificate_to_base(
                          std::filesystem::path{certificate_directory_} / ("wipe-" + record_id),
                          data);
            if (certificate) {
                message << "\n\nCertificate saved to:\n" << certificate->string() << ".txt";
            } else {
                message << "\n\nCertificate could not be written: " << certificate.error();
            }
        }

        show_message(MessageInfo::Type::INFO, "Wipe Complete", message.str());

        // Send desktop notification for successful completion
        if (notification_callback_) {
            notification_callback_("Wipe Complete", "Finished wiping " + path, false);
        }
    } else {
        std::string message = "Wipe of " + path + " failed.";
        if (!progress.error_message.empty()) {
            message += "\n\nError: " + progress.error_message;
        }
        show_message(MessageInfo::Type::ERROR, "Wipe Failed", message);

        // Send desktop notification for failure
        if (notification_callback_) {
            notification_callback_("Wipe Failed",
                                   progress.error_message.empty() ? "Wipe operation failed"
                                                                  : progress.error_message,
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
    if (disk_info->is_partition) {
        summary << "\nPartition of: " << disk_info->parent_disk;
    }
    if (!disk_info->filesystem.empty()) {
        summary << "\nFilesystem: " << disk_info->filesystem;
    }
    if (disk_info->is_mounted) {
        summary << "\nMount: "
                << (disk_info->mount_point.empty() ? "mounted" : disk_info->mount_point);
    }
    if (disk_info->is_lvm_pv) {
        summary << "\nLVM physical volume (or containing disk)";
    } else if (disk_info->has_dm_holders) {
        summary << "\nDevice-mapper backing device";
    }

    return summary.str();
}

void MainViewModel::set_connection_state(bool connected, const std::string& error_message) {
    if (cleaned_up_.load())
        return;
    ++connection_generation_;
    is_connected.set(connected);
    connection_error.set(error_message);

    if (connected) {
        // Restore helper-owned jobs as well as the current disk list.
        load_disks();
        restore_wipe_jobs();
    } else {
        is_disk_refreshing.set(false);
        disks.set({});
        selected_disk_path.set("");
    }
}

void MainViewModel::set_notification_callback(NotificationCallback callback) {
    notification_callback_ = std::move(callback);
}

void MainViewModel::set_certificate_directory(const std::string& directory) {
    certificate_directory_ = directory;
}

void MainViewModel::restore_settings(const util::AppSettingsData& settings) {
    if (settings.algorithm_id < 0 || settings.algorithm_id > util::AppSettings::MAX_ALGORITHM_ID) {
        return;
    }
    selected_algorithm.set(static_cast<WipeAlgorithm>(settings.algorithm_id));
    verification_enabled.set(settings.verification_enabled);
    update_algorithm_state();
}

auto MainViewModel::current_settings() const -> util::AppSettingsData {
    return util::AppSettingsData{.algorithm_id = static_cast<int>(selected_algorithm.get()),
                                 .verification_enabled = verification_enabled.get()};
}

auto MainViewModel::build_certificate_data(const std::string& path, const WipeActivity& activity,
                                           const WipeProgress& progress) const
    -> util::WipeCertificateData {
    const auto& disk_info = activity.plan.device.disk;

    util::WipeCertificateData data{};
    util::apply_wipe_plan(data, activity.plan);
    data.device_path = path;
    data.model = disk_info.model;
    data.serial = disk_info.serial;
    data.size_bytes = disk_info.size_bytes;
    data.algorithm_name = wipe_service_->get_algorithm_name(activity.algorithm);
    data.nist_category = wipe_service_->get_nist_category(activity.algorithm);
    data.total_passes = progress.total_passes > 0
                            ? progress.total_passes
                            : wipe_service_->get_pass_count(activity.algorithm);
    data.started_at = util::iso8601_utc(activity.started_at);
    data.completed_at =
        util::iso8601_utc(activity.completed_at.value_or(std::chrono::system_clock::now()));
    data.duration_seconds = static_cast<uint64_t>(
        std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::steady_clock::now() - activity.started_steady)
                                 .count()));
    if (activity.completed_at) {
        data.duration_seconds = static_cast<uint64_t>(
            std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::seconds>(
                                     *activity.completed_at - activity.started_at)
                                     .count()));
    }
    data.peak_speed_bytes_per_sec = activity.peak_speed;
    data.verification_enabled = progress.verification_enabled;
    data.verification_passed = progress.verification_passed;
    data.is_partition = disk_info.is_partition;
    data.parent_disk = disk_info.parent_disk;
    data.bad_block_count = progress.bad_block_count;
    data.success = wipe_succeeded(progress);
    data.tool_version = PROJECT_VERSION;
    return data;
}

void MainViewModel::set_settings_save_callback(SettingsSaveCallback callback) {
    settings_save_callback_ = std::move(callback);
}
