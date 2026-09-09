#include "views/MainWindowContent.hpp"

#include "views/AlgorithmRow.hpp"
#include "views/DiskRow.hpp"

#include <cstdint>
#include <format>

namespace {

/**
 * @brief Format bytes per second as human-readable speed
 */
auto format_speed(uint64_t bytes_per_sec) -> std::string {
    constexpr uint64_t KB = 1'024;
    constexpr uint64_t MB = KB * 1'024;
    constexpr uint64_t GB = MB * 1'024;

    if (bytes_per_sec >= GB) {
        return std::format("{:.1f} GB/s",
                           static_cast<double>(bytes_per_sec) / static_cast<double>(GB));
    } else if (bytes_per_sec >= MB) {
        return std::format("{:.1f} MB/s",
                           static_cast<double>(bytes_per_sec) / static_cast<double>(MB));
    } else if (bytes_per_sec >= KB) {
        return std::format("{:.1f} KB/s",
                           static_cast<double>(bytes_per_sec) / static_cast<double>(KB));
    } else {
        return std::format("{} B/s", bytes_per_sec);
    }
}

/**
 * @brief Format seconds as human-readable time (HH:MM:SS or MM:SS)
 */
auto format_time(int64_t seconds) -> std::string {
    if (seconds < 0) {
        return "calculating...";
    }

    int64_t hours = seconds / 3'600;
    int64_t minutes = (seconds % 3'600) / 60;
    int64_t secs = seconds % 60;

    if (hours > 0) {
        return std::format("{}:{:02d}:{:02d}", hours, minutes, secs);
    } else {
        return std::format("{}:{:02d}", minutes, secs);
    }
}

}  // namespace

MainWindowContent::MainWindowContent() : Gtk::Box(Gtk::Orientation::VERTICAL, 0) {
    setup_from_builder();
    setup_dispatcher();
    connect_signals();
}

MainWindowContent::~MainWindowContent() {
    // Note: subscriptions are automatically cleaned when ViewModel is destroyed
    // The subscriptions_ vector is kept for potential explicit cleanup if needed
    subscriptions_.clear();
}

void MainWindowContent::setup_from_builder() {
    // Load UI from GResource
    auto builder = Gtk::Builder::create_from_resource("/org/storage/wiper/ui/main-window.ui");

    // Get the main content box from the builder
    auto* main_content = builder->get_widget<Gtk::Box>("main_content");
    if (!main_content) {
        throw std::runtime_error("Failed to load main-window.ui: main_content not found");
    }

    // Reparent children from the loaded widget to this widget
    while (auto* child = main_content->get_first_child()) {
        main_content->remove(*child);
        append(*child);
    }

    // Get references to child widgets we need to interact with
    disk_list_ = builder->get_widget<Gtk::ListBox>("disk_list");
    options_box_ = builder->get_widget<Gtk::Box>("options_box");
    progress_bar_ = builder->get_widget<Gtk::ProgressBar>("progress_bar");
    progress_label_ = builder->get_widget<Gtk::Label>("progress_label");
    wipe_button_ = builder->get_widget<Gtk::Button>("wipe_button");
    cancel_button_ = builder->get_widget<Gtk::Button>("cancel_button");
    verification_check_ = builder->get_widget<Gtk::CheckButton>("verification_check");
    status_box_ = builder->get_widget<Gtk::Box>("status_box");
    status_spinner_ = builder->get_widget<Gtk::Spinner>("status_spinner");
    status_icon_ = builder->get_widget<Gtk::Image>("status_icon");
    status_title_label_ = builder->get_widget<Gtk::Label>("status_title_label");
    status_detail_label_ = builder->get_widget<Gtk::Label>("status_detail_label");
    algorithm_warning_box_ = builder->get_widget<Gtk::Box>("algorithm_warning_box");
    algorithm_warning_label_ = builder->get_widget<Gtk::Label>("algorithm_warning_label");

    if (!disk_list_ || !options_box_ || !progress_bar_ || !progress_label_ || !wipe_button_ ||
        !cancel_button_ || !verification_check_ || !status_box_ || !status_spinner_ ||
        !status_icon_ || !status_title_label_ || !status_detail_label_ || !algorithm_warning_box_ ||
        !algorithm_warning_label_) {
        throw std::runtime_error("Failed to load main-window.ui: required widgets not found");
    }
}

void MainWindowContent::setup_dispatcher() {
    // Connect dispatcher to process pending UI updates on main thread
    dispatcher_.connect(sigc::mem_fun(*this, &MainWindowContent::process_pending_tasks));
}

void MainWindowContent::connect_signals() {
    // Disk list selection
    disk_list_->signal_row_selected().connect(
        sigc::mem_fun(*this, &MainWindowContent::on_disk_selected));

    // Button clicks
    wipe_button_->signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindowContent::on_wipe_clicked));
    cancel_button_->signal_clicked().connect(
        sigc::mem_fun(*this, &MainWindowContent::on_cancel_clicked));
    verification_check_->signal_toggled().connect(
        sigc::mem_fun(*this, &MainWindowContent::on_verification_toggled));
}

void MainWindowContent::bind(std::shared_ptr<MainViewModel> view_model) {
    view_model_ = std::move(view_model);

    // Set up data bindings
    bind_disks();
    bind_algorithms();
    bind_progress();
    bind_can_wipe();
    bind_status();
    bind_operation_state();
    bind_verification();
    bind_algorithm_warning();
}

void MainWindowContent::post_ui_update(std::function<void()> task) {
    {
        std::lock_guard lock(task_mutex_);
        pending_tasks_.push(std::move(task));
    }
    dispatcher_.emit();
}

void MainWindowContent::process_pending_tasks() {
    std::function<void()> task;
    while (true) {
        {
            std::lock_guard lock(task_mutex_);
            if (pending_tasks_.empty()) {
                return;
            }
            task = std::move(pending_tasks_.front());
            pending_tasks_.pop();
        }
        task();
    }
}

void MainWindowContent::bind_disks() {
    if (!view_model_)
        return;

    auto id = view_model_->disks.subscribe([this](const std::vector<DiskInfo>& disks) {
        auto disks_copy = disks;
        post_ui_update(
            [this, disks_copy = std::move(disks_copy)]() { update_disk_list(disks_copy); });
    });
    subscriptions_.push_back(id);

    // Initialize with current value (subscribe doesn't call callback with existing value)
    update_disk_list(view_model_->disks.get());
}

void MainWindowContent::bind_algorithms() {
    if (!view_model_)
        return;

    auto id =
        view_model_->algorithms.subscribe([this](const std::vector<AlgorithmInfo>& algorithms) {
            auto algos_copy = algorithms;
            post_ui_update([this, algos_copy = std::move(algos_copy)]() {
                update_algorithm_list(algos_copy);
            });
        });
    subscriptions_.push_back(id);

    // Initialize with current value (subscribe doesn't call callback with existing value)
    update_algorithm_list(view_model_->algorithms.get());
}

void MainWindowContent::bind_progress() {
    if (!view_model_)
        return;

    auto id = view_model_->wipe_progress.subscribe([this](const WipeProgress& progress) {
        auto progress_copy = progress;
        post_ui_update([this, progress_copy]() { update_progress(progress_copy); });
    });
    subscriptions_.push_back(id);
}

void MainWindowContent::bind_can_wipe() {
    if (!view_model_)
        return;

    auto id = view_model_->can_wipe.subscribe([this](bool can) {
        post_ui_update([this, can]() {
            if (wipe_button_) {
                wipe_button_->set_sensitive(can);
            }
        });
    });
    subscriptions_.push_back(id);

    // Initialize with current value (subscribe doesn't call callback with existing value)
    if (wipe_button_) {
        wipe_button_->set_sensitive(view_model_->can_wipe.get());
    }
}

void MainWindowContent::bind_status() {
    if (!view_model_)
        return;

    auto update = [this]() {
        post_ui_update([this]() { update_status_message(); });
    };

    subscriptions_.push_back(view_model_->disks.subscribe([update](const auto&) { update(); }));
    subscriptions_.push_back(view_model_->is_connected.subscribe([update](bool) { update(); }));
    subscriptions_.push_back(
        view_model_->connection_error.subscribe([update](const auto&) { update(); }));
    subscriptions_.push_back(
        view_model_->is_disk_refreshing.subscribe([update](bool) { update(); }));
    subscriptions_.push_back(
        view_model_->is_operation_pending.subscribe([update](bool) { update(); }));

    update_status_message();
}

void MainWindowContent::bind_operation_state() {
    if (!view_model_)
        return;

    auto update = [this]() {
        post_ui_update([this]() { update_operation_controls(); });
    };

    subscriptions_.push_back(view_model_->is_connected.subscribe([update](bool) { update(); }));
    subscriptions_.push_back(
        view_model_->is_wipe_in_progress.subscribe([update](bool) { update(); }));
    subscriptions_.push_back(
        view_model_->is_operation_pending.subscribe([update](bool) { update(); }));

    update_operation_controls();
}

void MainWindowContent::bind_verification() {
    if (!view_model_)
        return;

    auto update = [this]() {
        post_ui_update([this]() { update_verification_control(); });
    };

    subscriptions_.push_back(
        view_model_->verification_enabled.subscribe([update](bool) { update(); }));
    subscriptions_.push_back(
        view_model_->verification_available.subscribe([update](bool) { update(); }));
    subscriptions_.push_back(
        view_model_->is_wipe_in_progress.subscribe([update](bool) { update(); }));
    subscriptions_.push_back(
        view_model_->is_operation_pending.subscribe([update](bool) { update(); }));

    update_verification_control();
}

void MainWindowContent::bind_algorithm_warning() {
    if (!view_model_)
        return;

    auto id = view_model_->algorithm_warning.subscribe([this](const std::string& warning) {
        post_ui_update([this, warning]() {
            if (algorithm_warning_label_) {
                algorithm_warning_label_->set_text(warning);
                algorithm_warning_box_->set_visible(!warning.empty());
            }
        });
    });
    subscriptions_.push_back(id);

    const auto warning = view_model_->algorithm_warning.get();
    algorithm_warning_label_->set_text(warning);
    algorithm_warning_box_->set_visible(!warning.empty());
}

void MainWindowContent::update_disk_list(const std::vector<DiskInfo>& disks) {
    if (!disk_list_)
        return;

    // Save the selected path before clearing (clearing triggers row-selected with nullptr)
    std::string selected_path;
    if (view_model_) {
        selected_path = view_model_->selected_disk_path.get();
    }

    // Set flag to ignore selection changes during list rebuild
    updating_disk_list_ = true;

    // Clear existing rows
    while (auto* child = disk_list_->get_first_child()) {
        disk_list_->remove(*child);
    }

    // Add new rows
    for (const auto& disk : disks) {
        auto* row = Gtk::make_managed<DiskRow>(disk);
        disk_list_->append(*row);
    }

    // Restore selection if a disk was previously selected
    if (!selected_path.empty()) {
        for (int i = 0;; ++i) {
            auto* row = disk_list_->get_row_at_index(i);
            if (!row)
                break;

            auto* disk_row = dynamic_cast<DiskRow*>(row);
            if (disk_row && disk_row->get_disk_path() == selected_path) {
                disk_list_->select_row(*row);
                break;
            }
        }
    }

    // Re-enable selection handling
    updating_disk_list_ = false;

    // Manually trigger selection update to ensure ViewModel state is correct
    if (!selected_path.empty() && view_model_) {
        view_model_->select_disk(selected_path);
    }
}

void MainWindowContent::update_algorithm_list(const std::vector<AlgorithmInfo>& algorithms) {
    if (!options_box_)
        return;

    // Clear existing rows
    algorithm_rows_.clear();
    first_radio_ = nullptr;
    while (auto* child = options_box_->get_first_child()) {
        options_box_->remove(*child);
    }

    AlgorithmRow* selected_row = nullptr;
    const auto selected_algorithm =
        view_model_ ? view_model_->selected_algorithm.get() : WipeAlgorithm::ZERO_FILL;

    // Add new algorithm rows
    for (const auto& algo : algorithms) {
        auto* row = Gtk::make_managed<AlgorithmRow>(algo, first_radio_);
        algorithm_rows_.push_back(row);

        // First radio button becomes the group leader
        if (!first_radio_) {
            first_radio_ = row->get_radio_button();
        }

        if (algo.algorithm == selected_algorithm) {
            selected_row = row;
        }

        // Connect toggled signal
        row->signal_toggled().connect([this, algo]() {
            if (view_model_) {
                view_model_->select_algorithm(algo.algorithm);
            }
        });

        options_box_->append(*row);
    }

    if (selected_row) {
        selected_row->set_active(true);
    } else if (!algorithm_rows_.empty()) {
        algorithm_rows_.front()->set_active(true);
    }
}

void MainWindowContent::update_progress(const WipeProgress& progress) {
    if (progress_bar_) {
        progress_bar_->set_fraction(progress.percentage / 100.0);
    }

    if (progress_label_) {
        std::ostringstream status;
        status << progress.status;
        if (progress.current_pass > 0 && progress.total_passes > 1) {
            status << " (Pass " << progress.current_pass << "/" << progress.total_passes << ")";
        }
        if (progress.verification_in_progress) {
            status << " (Verification " << static_cast<int>(progress.verification_percentage)
                   << "%)";
        }
        status << " - " << static_cast<int>(progress.percentage) << "%";

        // Add speed display
        if (progress.speed_bytes_per_sec > 0) {
            status << " @ " << format_speed(progress.speed_bytes_per_sec);
        }

        // Add ETA display
        if (progress.estimated_seconds_remaining >= 0) {
            status << " - ETA: " << format_time(progress.estimated_seconds_remaining);
        }

        progress_label_->set_text(status.str());
    }

    // Show/hide progress elements. A progress with no total bytes is a
    // placeholder (selected device not wiping) and stays hidden.
    update_progress_visibility(!progress.is_complete && progress.total_bytes > 0);

    // Show/hide cancel button during operation
    if (cancel_button_) {
        cancel_button_->set_visible(!progress.is_complete && !progress.has_error &&
                                    progress.total_bytes > 0);
    }
}

void MainWindowContent::update_progress_visibility(bool visible) {
    if (progress_bar_) {
        progress_bar_->set_visible(visible);
    }
    if (progress_label_) {
        progress_label_->set_visible(visible);
    }
}

void MainWindowContent::update_status_message() {
    if (!view_model_ || !status_box_ || !status_spinner_ || !status_icon_ || !status_title_label_ ||
        !status_detail_label_) {
        return;
    }

    bool visible = true;
    bool spinning = false;
    std::string title;
    std::string detail;
    std::string icon_name;

    if (!view_model_->is_connected.get()) {
        title = "Helper service unavailable";
        icon_name = "dialog-error-symbolic";
        detail = view_model_->connection_error.get();
        if (detail.empty()) {
            detail = "Install and start the privileged helper, then refresh the disk list.";
        }
    } else if (view_model_->is_operation_pending.get()) {
        title = "Preparing wipe operation";
        icon_name = "security-high-symbolic";
        detail = "Waiting for the privileged helper to finish the requested operation.";
        spinning = true;
    } else if (view_model_->is_disk_refreshing.get()) {
        title = "Loading storage devices";
        icon_name = "view-refresh-symbolic";
        detail = "Reading block devices and SMART health information.";
        spinning = true;
    } else if (view_model_->disks.get().empty()) {
        title = "No storage devices found";
        icon_name = "drive-harddisk-symbolic";
        detail = "Attach a supported disk or refresh after installing the helper service.";
    } else {
        visible = false;
    }

    status_box_->set_visible(visible);
    status_spinner_->set_visible(spinning);
    status_icon_->set_visible(!spinning);
    if (!icon_name.empty()) {
        status_icon_->set_from_icon_name(icon_name);
    }
    if (spinning) {
        status_spinner_->start();
    } else {
        status_spinner_->stop();
    }
    status_title_label_->set_text(title);
    status_detail_label_->set_text(detail);
}

void MainWindowContent::update_operation_controls() {
    if (!view_model_)
        return;

    // Wipes running on other devices must not lock the UI: the disk list and
    // options stay interactive so further devices can be selected and wiped
    // in parallel. The wipe button itself follows the per-device can_wipe.
    const bool controls_enabled =
        view_model_->is_connected.get() && !view_model_->is_operation_pending.get();

    if (disk_list_) {
        disk_list_->set_sensitive(controls_enabled);
    }
    if (options_box_) {
        options_box_->set_sensitive(controls_enabled);
    }
    if (verification_check_) {
        verification_check_->set_sensitive(controls_enabled &&
                                           view_model_->verification_available.get());
    }
}

void MainWindowContent::update_verification_control() {
    if (!view_model_ || !verification_check_)
        return;

    const bool enabled = view_model_->verification_enabled.get();
    if (verification_check_->get_active() != enabled) {
        verification_check_->set_active(enabled);
    }

    const bool controls_enabled =
        view_model_->is_connected.get() && !view_model_->is_operation_pending.get();
    const bool available = view_model_->verification_available.get();
    verification_check_->set_sensitive(controls_enabled && available);
    verification_check_->set_tooltip_text(
        available ? "Read the device after wiping and verify supported algorithms. This makes the "
                    "operation take longer."
                  : "The selected algorithm does not support post-wipe verification.");
}

void MainWindowContent::on_disk_selected(Gtk::ListBoxRow* row) {
    if (!view_model_)
        return;

    // Ignore selection changes during list updates (e.g., when clearing rows)
    if (updating_disk_list_)
        return;

    if (row) {
        auto* disk_row = dynamic_cast<DiskRow*>(row);
        if (disk_row) {
            view_model_->select_disk(disk_row->get_disk_path());
        }
    } else {
        view_model_->select_disk("");
    }
}

void MainWindowContent::on_wipe_clicked() {
    if (view_model_ && view_model_->wipe_command) {
        view_model_->wipe_command->execute();
    }
}

void MainWindowContent::on_cancel_clicked() {
    if (view_model_ && view_model_->cancel_command) {
        view_model_->cancel_command->execute();
    }
}

void MainWindowContent::on_verification_toggled() {
    if (view_model_ && verification_check_) {
        view_model_->verification_enabled.set(verification_check_->get_active());
    }
}

auto MainWindowContent::get_selected_disk_path() const -> std::string {
    if (view_model_) {
        return view_model_->selected_disk_path.get();
    }
    return "";
}
