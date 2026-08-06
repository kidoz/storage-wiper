#include "views/MainWindow.hpp"

#include "util/Logger.hpp"

#include <format>

#include "config.h"

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
}  // anonymous namespace

MainWindow::MainWindow(AdwApplicationWindow* window)
    : window_(window), content_(std::make_unique<MainWindowContent>()) {}

MainWindow::~MainWindow() {
    // Unsubscribe from message observable
    if (view_model_ && message_subscription_id_ != 0) {
        view_model_->current_message.unsubscribe(message_subscription_id_);
    }
    if (view_model_ && refresh_connected_subscription_id_ != 0) {
        view_model_->is_connected.unsubscribe(refresh_connected_subscription_id_);
    }
    if (view_model_ && refresh_wipe_subscription_id_ != 0) {
        view_model_->is_wipe_in_progress.unsubscribe(refresh_wipe_subscription_id_);
    }
    if (view_model_ && refresh_pending_subscription_id_ != 0) {
        view_model_->is_operation_pending.unsubscribe(refresh_pending_subscription_id_);
    }
}

void MainWindow::bind(std::shared_ptr<MainViewModel> view_model) {
    view_model_ = std::move(view_model);

    // Bind content widget to ViewModel
    content_->bind(view_model_);

    // Bind messages (handled at window level for Adwaita dialogs)
    bind_messages();
    bind_refresh_state();
}

void MainWindow::setup_ui() {
    // Create main vertical box (C API for Adwaita window)
    auto* main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    // Create Adwaita header bar (C API)
    create_header_bar();
    gtk_box_append(GTK_BOX(main_box), header_bar_);

    // Add gtkmm4 content widget using gobj() interop
    gtk_box_append(GTK_BOX(main_box), GTK_WIDGET(content_->gobj()));

    // Set window content
    adw_application_window_set_content(window_, main_box);
}

void MainWindow::show() {
    gtk_window_present(GTK_WINDOW(window_));
}

void MainWindow::create_header_bar() {
    header_bar_ = adw_header_bar_new();
    adw_header_bar_set_title_widget(
        ADW_HEADER_BAR(header_bar_),
        adw_window_title_new("Storage Wiper", "Securely Erase Storage Devices"));

    // Refresh button
    refresh_button_ = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(refresh_button_, "Refresh disk list");
    g_signal_connect(refresh_button_, "clicked", G_CALLBACK(on_refresh_clicked), this);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header_bar_), refresh_button_);

    // Primary menu
    auto* action_group = g_simple_action_group_new();
    auto* about_action = g_simple_action_new("about", nullptr);
    g_signal_connect(about_action, "activate", G_CALLBACK(on_about_action), this);
    g_action_map_add_action(G_ACTION_MAP(action_group), G_ACTION(about_action));
    gtk_widget_insert_action_group(GTK_WIDGET(window_), "win", G_ACTION_GROUP(action_group));
    g_object_unref(about_action);
    g_object_unref(action_group);

    auto* menu_model = g_menu_new();
    g_menu_append(menu_model, "About Storage Wiper", "win.about");

    auto* menu_button = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button), "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button), G_MENU_MODEL(menu_model));
    gtk_widget_set_tooltip_text(menu_button, "Main Menu");
    adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar_), menu_button);
    g_object_unref(menu_model);
}

void MainWindow::bind_messages() {
    if (!view_model_)
        return;

    message_subscription_id_ =
        view_model_->current_message.subscribe([this](const MessageInfo& message) {
            if (message.title.empty())
                return;  // Skip empty messages

            // Use g_idle_add_full with destroy notify to prevent memory leaks
            // if the idle source is removed before execution
            auto* data = new std::pair<MainWindow*, MessageInfo*>(this, new MessageInfo(message));

            g_idle_add_full(
                G_PRIORITY_DEFAULT_IDLE,
                [](gpointer user_data) -> gboolean {
                    auto* args = static_cast<std::pair<MainWindow*, MessageInfo*>*>(user_data);
                    args->first->show_message(*args->second);
                    return G_SOURCE_REMOVE;
                },
                data,
                [](gpointer user_data) {
                    // Destroy notify - called when source is removed
                    auto* args = static_cast<std::pair<MainWindow*, MessageInfo*>*>(user_data);
                    delete args->second;
                    delete args;
                });
        });
}

void MainWindow::bind_refresh_state() {
    if (!view_model_ || !refresh_button_)
        return;

    refresh_connected_subscription_id_ =
        view_model_->is_connected.subscribe([this](bool) { update_refresh_button_state(); });
    refresh_wipe_subscription_id_ =
        view_model_->is_wipe_in_progress.subscribe([this](bool) { update_refresh_button_state(); });
    refresh_pending_subscription_id_ = view_model_->is_operation_pending.subscribe(
        [this](bool) { update_refresh_button_state(); });

    update_refresh_button_state();
}

void MainWindow::update_refresh_button_state() {
    if (!view_model_ || !refresh_button_)
        return;

    gtk_widget_set_sensitive(refresh_button_, view_model_->refresh_command &&
                                                  view_model_->refresh_command->can_execute());
}

void MainWindow::show_message(const MessageInfo& message) {
    if (!window_ || !GTK_IS_WIDGET(window_))
        return;

    if (message.type == MessageInfo::Type::CONFIRMATION) {
        show_confirmation_dialog(message);
    } else {
        show_info_dialog(message);
    }
}

void MainWindow::show_confirmation_dialog(const MessageInfo& message) {
    auto* dialog = adw_alert_dialog_new(message.title.c_str(), message.message.c_str());
    if (!dialog) {
        LOG_ERROR("MainWindow", "Failed to create confirmation dialog");
        return;
    }

    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel");

    // Add confirm button with initial countdown text
    auto initial_text = std::format("Confirm ({})", CONFIRMATION_COUNTDOWN_SECONDS);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "confirm", initial_text.c_str());
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "confirm",
                                             ADW_RESPONSE_DESTRUCTIVE);

    // Disable confirm button initially - user must wait for countdown
    adw_alert_dialog_set_response_enabled(ADW_ALERT_DIALOG(dialog), "confirm", FALSE);

    // Set up countdown timer
    auto* countdown = new CountdownData{.dialog = ADW_ALERT_DIALOG(dialog),
                                        .seconds_remaining = CONFIRMATION_COUNTDOWN_SECONDS,
                                        .timer_id = 0};
    countdown->timer_id = g_timeout_add_seconds(1, on_countdown_tick, countdown);

    // Store countdown data for cleanup when dialog is destroyed
    g_object_set_data_full(G_OBJECT(dialog), "countdown", countdown, cleanup_countdown_data);

    if (message.confirmation_callback) {
        auto* callback_ptr = new std::function<void(bool)>(message.confirmation_callback);
        g_object_set_data_full(
            G_OBJECT(dialog), "callback", callback_ptr,
            +[](gpointer data) { delete static_cast<std::function<void(bool)>*>(data); });

        g_signal_connect(dialog, "response",
                         G_CALLBACK(+[](AdwAlertDialog* dlg, const char* resp, gpointer) {
                             auto* cb = static_cast<std::function<void(bool)>*>(
                                 g_object_get_data(G_OBJECT(dlg), "callback"));
                             if (cb) {
                                 (*cb)(g_strcmp0(resp, "confirm") == 0);
                             }
                         }),
                         nullptr);
    }

    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window_));
}

void MainWindow::show_info_dialog(const MessageInfo& message) {
    auto* dialog = adw_alert_dialog_new(message.title.c_str(), message.message.c_str());
    if (!dialog) {
        LOG_ERROR("MainWindow", "Failed to create info dialog");
        return;
    }
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "ok", "OK");
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window_));
}

// Event handlers for header bar buttons

void MainWindow::on_refresh_clicked(GtkWidget*, gpointer user_data) {
    auto* self = static_cast<MainWindow*>(user_data);
    if (self->view_model_ && self->view_model_->refresh_command) {
        self->view_model_->refresh_command->execute();
    }
}

void MainWindow::on_about_action(GSimpleAction*, GVariant*, gpointer user_data) {
    auto* self = static_cast<MainWindow*>(user_data);
    self->show_about_dialog();
}

void MainWindow::show_about_dialog() {
    auto* about = ADW_ABOUT_DIALOG(adw_about_dialog_new());

    adw_about_dialog_set_application_name(about, "Storage Wiper");
    adw_about_dialog_set_application_icon(about, "storage-wiper");
    adw_about_dialog_set_version(about, PROJECT_VERSION);
    adw_about_dialog_set_developer_name(about, "Aleksandr Pavlov");
    adw_about_dialog_set_comments(
        about, "Secure disk wiping tool with multiple DoD-compliant algorithms.\n\n"
               "Supports Zero Fill, Random Fill, DoD 5220.22-M, Schneier, "
               "VSITR, Gutmann, GOST R 50739-95, and ATA Secure Erase methods.");
    adw_about_dialog_set_license_type(about, GTK_LICENSE_MIT_X11);
    adw_about_dialog_set_copyright(about, "Copyright \xC2\xA9 2025 Aleksandr Pavlov");

    adw_about_dialog_set_issue_url(about, "https://github.com/kidoz/storage-wiper/issues");
    adw_about_dialog_set_website(about, "https://github.com/kidoz/storage-wiper");

    const char* developers[] = {"Aleksandr Pavlov <ckidoz@gmail.com>", nullptr};
    adw_about_dialog_set_developers(about, developers);

    // Credits section
    const char* built_with[] = {"GTK4 - GNOME toolkit", "gtkmm4 - C++ bindings for GTK4",
                                "libadwaita - Adaptive GNOME applications",
                                "C++23 - Modern C++ standard", nullptr};
    adw_about_dialog_add_credit_section(about, "Built With", built_with);

    adw_about_dialog_add_legal_section(
        about, "Disclaimer", nullptr, GTK_LICENSE_CUSTOM,
        "This software is provided for legitimate data sanitization purposes. "
        "Use responsibly and ensure you have proper authorization before wiping any storage "
        "device. "
        "The authors are not responsible for any data loss.");

    adw_dialog_present(ADW_DIALOG(about), GTK_WIDGET(window_));
}
