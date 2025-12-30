#include "views/MainWindowContent.hpp"
#include "views/DiskRow.hpp"
#include "views/AlgorithmRow.hpp"

#include <format>
#include <sstream>

MainWindowContent::MainWindowContent()
    : Gtk::Box(Gtk::Orientation::VERTICAL, 0)
{
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
    auto builder = Gtk::Builder::create_from_resource(
        "/org/storage/wiper/ui/main-window.ui");

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

    if (!disk_list_ || !options_box_ || !progress_bar_ ||
        !progress_label_ || !wipe_button_ || !cancel_button_) {
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
}

void MainWindowContent::bind(std::shared_ptr<MainViewModel> view_model) {
    view_model_ = std::move(view_model);

    // Set up data bindings
    bind_disks();
    bind_algorithms();
    bind_progress();
    bind_can_wipe();
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
    if (!view_model_) return;

    auto id = view_model_->disks.subscribe([this](const std::vector<DiskInfo>& disks) {
        auto disks_copy = disks;
        post_ui_update([this, disks_copy = std::move(disks_copy)]() {
            update_disk_list(disks_copy);
        });
    });
    subscriptions_.push_back(id);

    // Initialize with current value (subscribe doesn't call callback with existing value)
    update_disk_list(view_model_->disks.get());
}

void MainWindowContent::bind_algorithms() {
    if (!view_model_) return;

    auto id = view_model_->algorithms.subscribe([this](const std::vector<AlgorithmInfo>& algorithms) {
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
    if (!view_model_) return;

    auto id = view_model_->wipe_progress.subscribe([this](const WipeProgress& progress) {
        auto progress_copy = progress;
        post_ui_update([this, progress_copy]() {
            update_progress(progress_copy);
        });
    });
    subscriptions_.push_back(id);
}

void MainWindowContent::bind_can_wipe() {
    if (!view_model_) return;

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

void MainWindowContent::update_disk_list(const std::vector<DiskInfo>& disks) {
    if (!disk_list_) return;

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
        for (int i = 0; ; ++i) {
            auto* row = disk_list_->get_row_at_index(i);
            if (!row) break;

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
    if (!options_box_) return;

    // Clear existing rows
    algorithm_rows_.clear();
    first_radio_ = nullptr;
    while (auto* child = options_box_->get_first_child()) {
        options_box_->remove(*child);
    }

    // Add new algorithm rows
    for (const auto& algo : algorithms) {
        auto* row = Gtk::make_managed<AlgorithmRow>(algo, first_radio_);
        algorithm_rows_.push_back(row);

        // First radio button becomes the group leader
        if (!first_radio_) {
            first_radio_ = row->get_radio_button();
            row->set_active(true);  // Select first by default
        }

        // Connect toggled signal
        row->signal_toggled().connect([this, algo]() {
            if (view_model_) {
                view_model_->select_algorithm(algo.algorithm);
            }
        });

        options_box_->append(*row);
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
        status << " - " << static_cast<int>(progress.percentage) << "%";
        progress_label_->set_text(status.str());
    }

    // Show/hide progress elements
    update_progress_visibility(!progress.is_complete);

    // Show/hide cancel button during operation
    if (cancel_button_) {
        cancel_button_->set_visible(!progress.is_complete && !progress.has_error);
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

void MainWindowContent::on_disk_selected(Gtk::ListBoxRow* row) {
    if (!view_model_) return;

    // Ignore selection changes during list updates (e.g., when clearing rows)
    if (updating_disk_list_) return;

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

auto MainWindowContent::get_selected_disk_path() const -> std::string {
    if (view_model_) {
        return view_model_->selected_disk_path.get();
    }
    return "";
}
