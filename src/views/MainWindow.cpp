#include "views/MainWindow.hpp"
#include <sstream>
#include <format>
#include <utility>
#include <algorithm>

namespace {
    // Countdown timer state for confirmation dialog
    struct CountdownData {
        AdwAlertDialog* dialog = nullptr;
        int seconds_remaining = 0;
        guint timer_id = 0;
    };

    constexpr int CONFIRMATION_COUNTDOWN_SECONDS = 15;

    auto on_countdown_tick(gpointer user_data) -> gboolean {
        auto* data = static_cast<CountdownData*>(user_data);
        if (!data || !data->dialog) {
            return G_SOURCE_REMOVE;
        }

        data->seconds_remaining--;

        if (data->seconds_remaining > 0) {
            // Update button text with countdown
            auto button_text = std::format("Confirm ({})", data->seconds_remaining);
            adw_alert_dialog_set_response_label(data->dialog, "confirm", button_text.c_str());
            return G_SOURCE_CONTINUE;
        }

        // Countdown finished - enable the button
        adw_alert_dialog_set_response_label(data->dialog, "confirm", "Confirm");
        adw_alert_dialog_set_response_enabled(data->dialog, "confirm", TRUE);
        data->timer_id = 0;
        return G_SOURCE_REMOVE;
    }

    void cleanup_countdown_data(gpointer data) {
        auto* countdown = static_cast<CountdownData*>(data);
        if (countdown) {
            if (countdown->timer_id > 0) {
                g_source_remove(countdown->timer_id);
            }
            delete countdown;
        }
    }
} // anonymous namespace

MainWindow::MainWindow(AdwApplicationWindow* window)
    : window_(window) {
}

MainWindow::~MainWindow() {
    // Note: Subscriptions are automatically cleaned up when ViewModel is destroyed
    // But we should be explicit about unbinding if the View is destroyed first
    subscriptions_.clear();

    {
        std::lock_guard lock(idle_mutex_);
        for (guint source_id : idle_sources_) {
            g_source_remove(source_id);
        }
        idle_sources_.clear();
    }

    destroyed_ = true;
}

void MainWindow::bind(std::shared_ptr<MainViewModel> view_model) {
    view_model_ = std::move(view_model);

    // Set up data bindings
    bind_disks();
    bind_algorithms();
    bind_wipe_progress();
    bind_can_wipe();
    bind_messages();
}

void MainWindow::bind_disks() {
    if (!view_model_) return;

    auto id = view_model_->disks.subscribe([this](const std::vector<DiskInfo>& disks) {
        auto disks_copy = disks;
        post_idle([this, disks_copy = std::move(disks_copy)]() {
            if (window_ && GTK_IS_WIDGET(window_)) {
                update_disk_list(disks_copy);
            }
        });
    });
    subscriptions_.push_back(id);
}

void MainWindow::bind_algorithms() {
    if (!view_model_) return;

    auto id = view_model_->algorithms.subscribe([this](const std::vector<AlgorithmInfo>& algorithms) {
        auto algos_copy = algorithms;
        post_idle([this, algos_copy = std::move(algos_copy)]() {
            if (window_ && GTK_IS_WIDGET(window_)) {
                update_algorithm_list(algos_copy);
            }
        });
    });
    subscriptions_.push_back(id);
}

void MainWindow::bind_wipe_progress() {
    if (!view_model_) return;

    auto id = view_model_->wipe_progress.subscribe([this](const WipeProgress& progress) {
        post_idle([this, progress]() {
            if (window_ && GTK_IS_WIDGET(window_)) {
                update_wipe_progress(progress);
            }
        });
    });
    subscriptions_.push_back(id);
}

void MainWindow::bind_can_wipe() {
    if (!view_model_) return;

    auto id = view_model_->can_wipe.subscribe([this](bool can) {
        post_idle([this, can]() {
            auto* button = wipe_button_;
            if (button && GTK_IS_WIDGET(button)) {
                gtk_widget_set_sensitive(button, can);
            }
        });
    });
    subscriptions_.push_back(id);
}

void MainWindow::bind_messages() {
    if (!view_model_) return;

    auto id = view_model_->current_message.subscribe([this](const MessageInfo& message) {
        if (message.title.empty()) return;  // Skip empty messages

        post_idle([this, message]() {
            if (window_ && GTK_IS_WIDGET(window_)) {
                show_message(message);
            }
        });
    });
    subscriptions_.push_back(id);
}

void MainWindow::setup_ui() {
    auto* main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    create_header_bar();
    gtk_box_append(GTK_BOX(main_box), header_bar_);

    auto* content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(content_box, 12);
    gtk_widget_set_margin_end(content_box, 12);
    gtk_widget_set_margin_top(content_box, 12);
    gtk_widget_set_margin_bottom(content_box, 12);

    // Disk selection section
    auto* disk_title = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(disk_title), "<b>Select Storage Device</b>");
    gtk_widget_set_halign(disk_title, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(content_box), disk_title);

    create_disk_list_view();
    auto* scrolled_window = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scrolled_window), 200);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window), disk_list_);
    gtk_widget_add_css_class(scrolled_window, "card");
    gtk_box_append(GTK_BOX(content_box), scrolled_window);

    // Wipe options section
    auto* options_title = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(options_title), "<b>Wiping Options</b>");
    gtk_widget_set_halign(options_title, GTK_ALIGN_START);
    gtk_widget_set_margin_top(options_title, 12);
    gtk_box_append(GTK_BOX(content_box), options_title);

    create_wipe_options();
    gtk_box_append(GTK_BOX(content_box), options_box_);

    gtk_box_append(GTK_BOX(main_box), content_box);

    // Progress section
    create_progress_view();
    gtk_box_append(GTK_BOX(main_box), progress_bar_);
    gtk_box_append(GTK_BOX(main_box), progress_label_);

    // Action bar
    create_action_bar();
    gtk_box_append(GTK_BOX(main_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(main_box), action_bar_);

    adw_application_window_set_content(window_, main_box);

    // Initially hide progress elements
    update_progress_visibility(false);
}

void MainWindow::show() {
    gtk_window_present(GTK_WINDOW(window_));
}

void MainWindow::create_header_bar() {
    header_bar_ = adw_header_bar_new();
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header_bar_),
                                    adw_window_title_new("Storage Wiper", "Secure Disk Wiping Tool"));

    auto* refresh_button = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(refresh_button, "Refresh disk list");
    g_signal_connect(refresh_button, "clicked", G_CALLBACK(on_refresh_clicked), this);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header_bar_), refresh_button);

    auto* about_button = gtk_button_new_from_icon_name("help-about-symbolic");
    gtk_widget_set_tooltip_text(about_button, "About");
    adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar_), about_button);
}

void MainWindow::create_disk_list_view() {
    disk_list_ = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(disk_list_), GTK_SELECTION_SINGLE);
    g_signal_connect(disk_list_, "row-selected", G_CALLBACK(on_disk_row_selected), this);
}

void MainWindow::create_wipe_options() {
    options_box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(options_box_, "card");
    gtk_widget_set_margin_start(options_box_, 12);
    gtk_widget_set_margin_end(options_box_, 12);
    gtk_widget_set_margin_top(options_box_, 12);
    gtk_widget_set_margin_bottom(options_box_, 12);

    // Algorithms will be populated via data binding from ViewModel
}

void MainWindow::create_action_bar() {
    action_bar_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(action_bar_, 12);
    gtk_widget_set_margin_end(action_bar_, 12);
    gtk_widget_set_margin_top(action_bar_, 12);
    gtk_widget_set_margin_bottom(action_bar_, 12);

    auto* warning_label = gtk_label_new("Warning: This will permanently destroy all data!");
    gtk_widget_add_css_class(warning_label, "warning");
    gtk_widget_set_hexpand(warning_label, TRUE);
    gtk_widget_set_halign(warning_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(action_bar_), warning_label);

    // Cancel button (initially hidden)
    cancel_button_ = gtk_button_new_with_label("Cancel");
    gtk_widget_add_css_class(cancel_button_, "destructive-action");
    gtk_widget_set_visible(cancel_button_, FALSE);
    g_signal_connect(cancel_button_, "clicked", G_CALLBACK(on_cancel_wipe_clicked), this);
    gtk_box_append(GTK_BOX(action_bar_), cancel_button_);

    wipe_button_ = gtk_button_new_with_label("Start Wipe");
    gtk_widget_add_css_class(wipe_button_, "destructive-action");
    gtk_widget_set_sensitive(wipe_button_, FALSE);
    g_signal_connect(wipe_button_, "clicked", G_CALLBACK(on_wipe_clicked), this);
    gtk_box_append(GTK_BOX(action_bar_), wipe_button_);
}

void MainWindow::create_progress_view() {
    progress_bar_ = gtk_progress_bar_new();
    gtk_widget_set_margin_start(progress_bar_, 12);
    gtk_widget_set_margin_end(progress_bar_, 12);
    gtk_widget_set_margin_top(progress_bar_, 12);

    progress_label_ = gtk_label_new("");
    gtk_widget_set_margin_start(progress_label_, 12);
    gtk_widget_set_margin_end(progress_label_, 12);
    gtk_widget_set_margin_bottom(progress_label_, 6);
    gtk_widget_add_css_class(progress_label_, "dim-label");
}

void MainWindow::update_disk_list(const std::vector<DiskInfo>& disks) {
    clear_disk_list();

    for (const auto& disk : disks) {
        auto* row = create_disk_row(disk);
        gtk_list_box_append(GTK_LIST_BOX(disk_list_), row);
    }
}

void MainWindow::update_algorithm_list(const std::vector<AlgorithmInfo>& algorithms) {
    // Clear existing algorithm buttons
    algorithm_buttons_.clear();
    while (GtkWidget* child = gtk_widget_get_first_child(options_box_)) {
        gtk_box_remove(GTK_BOX(options_box_), child);
    }

    GtkWidget* first_radio = nullptr;
    for (const auto& algo : algorithms) {
        auto* row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);

        GtkWidget* radio;
        if (!first_radio) {
            radio = gtk_check_button_new();
            first_radio = radio;
            gtk_check_button_set_active(GTK_CHECK_BUTTON(radio), TRUE);
        } else {
            radio = gtk_check_button_new();
            gtk_check_button_set_group(GTK_CHECK_BUTTON(radio),
                                       GTK_CHECK_BUTTON(first_radio));
        }

        g_object_set_data(G_OBJECT(radio), "algorithm",
                          GINT_TO_POINTER(static_cast<int>(algo.algorithm)));
        g_signal_connect(radio, "toggled", G_CALLBACK(on_algorithm_toggled), this);

        algorithm_buttons_.push_back(radio);

        auto* label_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

        // Build name with pass count
        std::string name_text = algo.name;
        if (algo.pass_count > 1) {
            name_text += " (" + std::to_string(algo.pass_count) + " passes)";
        }
        auto* name_label = gtk_label_new(name_text.c_str());
        gtk_widget_set_halign(name_label, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(label_box), name_label);

        auto* desc_label = gtk_label_new(algo.description.c_str());
        gtk_widget_set_halign(desc_label, GTK_ALIGN_START);
        gtk_widget_add_css_class(desc_label, "dim-label");
        gtk_widget_add_css_class(desc_label, "caption");
        gtk_box_append(GTK_BOX(label_box), desc_label);

        gtk_box_append(GTK_BOX(row_box), radio);
        gtk_box_append(GTK_BOX(row_box), label_box);
        gtk_widget_set_margin_bottom(row_box, 4);

        gtk_box_append(GTK_BOX(options_box_), row_box);
    }
}

void MainWindow::update_wipe_progress(const WipeProgress& progress) {
    if (progress_bar_) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar_),
                                      progress.percentage / 100.0);
    }

    if (progress_label_) {
        std::ostringstream status;
        status << progress.status;
        if (progress.current_pass > 0 && progress.total_passes > 1) {
            status << " (Pass " << progress.current_pass << "/" << progress.total_passes << ")";
        }
        status << " - " << static_cast<int>(progress.percentage) << "%";

        gtk_label_set_text(GTK_LABEL(progress_label_), status.str().c_str());
    }

    // Show/hide progress elements
    update_progress_visibility(!progress.is_complete);

    // Show/hide cancel button during operation
    if (cancel_button_) {
        gtk_widget_set_visible(cancel_button_, !progress.is_complete && !progress.has_error);
    }
}

void MainWindow::show_message(const MessageInfo& message) {
    auto* dialog = adw_alert_dialog_new(message.title.c_str(), message.message.c_str());

    if (message.type == MessageInfo::Type::CONFIRMATION) {
        adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel");

        // Add confirm button with initial countdown text
        auto initial_text = std::format("Confirm ({})", CONFIRMATION_COUNTDOWN_SECONDS);
        adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "confirm", initial_text.c_str());
        adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "confirm",
                                                 ADW_RESPONSE_DESTRUCTIVE);

        // Disable confirm button initially - user must wait for countdown
        adw_alert_dialog_set_response_enabled(ADW_ALERT_DIALOG(dialog), "confirm", FALSE);

        // Set up countdown timer
        auto* countdown = new CountdownData{
            .dialog = ADW_ALERT_DIALOG(dialog),
            .seconds_remaining = CONFIRMATION_COUNTDOWN_SECONDS,
            .timer_id = 0
        };
        countdown->timer_id = g_timeout_add_seconds(1, on_countdown_tick, countdown);

        // Store countdown data for cleanup when dialog is destroyed
        g_object_set_data_full(G_OBJECT(dialog), "countdown", countdown, cleanup_countdown_data);

        if (message.confirmation_callback) {
            auto* callback_ptr = new std::function<void(bool)>(message.confirmation_callback);
            g_object_set_data_full(G_OBJECT(dialog), "callback", callback_ptr,
                                   +[](gpointer data) { delete static_cast<std::function<void(bool)>*>(data); });

            g_signal_connect(dialog, "response", G_CALLBACK(+[](AdwAlertDialog* dlg, const char* resp, gpointer) {
                auto* cb = static_cast<std::function<void(bool)>*>(g_object_get_data(G_OBJECT(dlg), "callback"));
                if (cb) {
                    (*cb)(g_strcmp0(resp, "confirm") == 0);
                }
            }), nullptr);
        }
    } else {
        adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "ok", "OK");
    }

    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window_));
}

void MainWindow::clear_disk_list() {
    if (!disk_list_) return;

    while (GtkWidget* child = gtk_widget_get_first_child(disk_list_)) {
        gtk_list_box_remove(GTK_LIST_BOX(disk_list_), child);
    }
}

auto MainWindow::create_disk_row(const DiskInfo& disk) -> GtkWidget* {
    auto* row = gtk_list_box_row_new();
    auto* row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(row_box, 12);
    gtk_widget_set_margin_end(row_box, 12);
    gtk_widget_set_margin_top(row_box, 8);
    gtk_widget_set_margin_bottom(row_box, 8);

    auto* icon = gtk_image_new_from_icon_name("drive-harddisk-symbolic");
    gtk_widget_set_size_request(icon, 32, 32);
    gtk_box_append(GTK_BOX(row_box), icon);

    auto* info_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand(info_box, TRUE);

    auto* name_label = gtk_label_new(disk.path.c_str());
    gtk_widget_set_halign(name_label, GTK_ALIGN_START);
    gtk_label_set_markup(GTK_LABEL(name_label),
                         ("<b>" + disk.path + "</b> - " + disk.model).c_str());
    gtk_box_append(GTK_BOX(info_box), name_label);

    auto size_gb = static_cast<double>(disk.size_bytes) / (1024.0 * 1024.0 * 1024.0);
    auto info_text = std::to_string(static_cast<int>(size_gb)) + " GB";
    if (disk.is_ssd) info_text += " (SSD)";
    if (disk.is_mounted) info_text += " - Mounted at " + disk.mount_point;

    auto* info_label = gtk_label_new(info_text.c_str());
    gtk_widget_set_halign(info_label, GTK_ALIGN_START);
    gtk_widget_add_css_class(info_label, "dim-label");
    gtk_box_append(GTK_BOX(info_box), info_label);

    gtk_box_append(GTK_BOX(row_box), info_box);

    if (disk.is_mounted) {
        auto* mounted_label = gtk_label_new("MOUNTED");
        gtk_widget_add_css_class(mounted_label, "warning");
        gtk_box_append(GTK_BOX(row_box), mounted_label);
    }

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), row_box);
    g_object_set_data_full(G_OBJECT(row), "disk-path",
                           g_strdup(disk.path.c_str()), g_free);

    return row;
}

void MainWindow::update_progress_visibility(bool visible) {
    if (progress_bar_) {
        gtk_widget_set_visible(progress_bar_, visible);
    }
    if (progress_label_) {
        gtk_widget_set_visible(progress_label_, visible);
    }
}

void MainWindow::post_idle(std::function<void()> task) {
    struct IdleTask {
        MainWindow* self;
        std::function<void()> fn;
        guint source_id;
    };

    auto* idle_task = new IdleTask{this, std::move(task), 0};
    auto source_id = g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer data) -> gboolean {
        auto* task = static_cast<IdleTask*>(data);
        if (!task->self->destroyed_) {
            task->fn();
        }
        task->self->unregister_idle_source(task->source_id);
        delete task;
        return G_SOURCE_REMOVE;
    }, idle_task, nullptr);

    idle_task->source_id = source_id;
    register_idle_source(source_id);
}

void MainWindow::register_idle_source(guint source_id) {
    std::lock_guard lock(idle_mutex_);
    idle_sources_.push_back(source_id);
}

void MainWindow::unregister_idle_source(guint source_id) {
    std::lock_guard lock(idle_mutex_);
    idle_sources_.erase(std::remove(idle_sources_.begin(), idle_sources_.end(), source_id),
                        idle_sources_.end());
}

// Event handlers - trigger ViewModel commands/methods

void MainWindow::on_disk_row_selected(GtkListBox*, GtkListBoxRow* row, gpointer user_data) {
    auto* self = static_cast<MainWindow*>(user_data);
    if (!self->view_model_) return;

    if (row) {
        const char* path = static_cast<const char*>(g_object_get_data(G_OBJECT(row), "disk-path"));
        if (path) {
            self->view_model_->select_disk(path);
        }
    } else {
        self->view_model_->select_disk("");
    }
}

void MainWindow::on_refresh_clicked(GtkWidget*, gpointer user_data) {
    auto* self = static_cast<MainWindow*>(user_data);
    if (self->view_model_ && self->view_model_->refresh_command) {
        self->view_model_->refresh_command->execute();
    }
}

void MainWindow::on_wipe_clicked(GtkWidget*, gpointer user_data) {
    auto* self = static_cast<MainWindow*>(user_data);
    if (self->view_model_ && self->view_model_->wipe_command) {
        self->view_model_->wipe_command->execute();
    }
}

void MainWindow::on_algorithm_toggled(GtkWidget* widget, gpointer user_data) {
    auto* self = static_cast<MainWindow*>(user_data);
    if (!self->view_model_) return;

    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(widget))) {
        int algo_int = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "algorithm"));
        auto algorithm = static_cast<WipeAlgorithm>(algo_int);
        self->view_model_->select_algorithm(algorithm);
    }
}

void MainWindow::on_cancel_wipe_clicked(GtkWidget*, gpointer user_data) {
    auto* self = static_cast<MainWindow*>(user_data);
    if (self->view_model_ && self->view_model_->cancel_command) {
        self->view_model_->cancel_command->execute();
    }
}
