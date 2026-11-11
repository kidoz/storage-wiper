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
    : window_(window), callback_state_(std::make_shared<CallbackState>(this)),
      content_(std::make_unique<MainWindowContent>()) {
    connect_signal(G_OBJECT(window_), "destroy", G_CALLBACK(on_window_destroyed));
    connect_signal(G_OBJECT(window_), "notify::application",
                   G_CALLBACK(on_window_application_changed));
    connect_signal(G_OBJECT(window_), "close-request", G_CALLBACK(on_close_requested));
}

MainWindow::~MainWindow() {
    unbind();
    callback_state_->owner = nullptr;
}

void MainWindow::connect_signal(GObject* object, const char* signal, GCallback callback) {
    g_signal_connect_data(
        object, signal, callback, new WeakState(callback_state_),
        +[](gpointer data, GClosure*) { delete static_cast<WeakState*>(data); }, G_CONNECT_DEFAULT);
}

void MainWindow::invalidate_window() {
    unbind();
    window_ = nullptr;
    header_bar_ = nullptr;
    refresh_button_ = nullptr;
    callback_state_->owner = nullptr;
}

void MainWindow::on_window_destroyed(GtkWidget*, gpointer user_data) {
    if (auto state = static_cast<WeakState*>(user_data)->lock(); state && state->owner) {
        state->owner->invalidate_window();
    }
}

void MainWindow::on_window_application_changed(GObject* object, GParamSpec*, gpointer user_data) {
    if (!gtk_window_get_application(GTK_WINDOW(object))) {
        on_window_destroyed(GTK_WIDGET(object), user_data);
    }
}

auto MainWindow::on_close_requested(GtkWindow*, gpointer user_data) -> gboolean {
    if (auto state = static_cast<WeakState*>(user_data)->lock(); state && state->owner) {
        return state->owner->request_close();
    }
    return FALSE;
}

void MainWindow::unbind() {
    ++callback_state_->generation;
    content_->detach();
    // Subscription ID zero is valid.
    if (view_model_ && message_subscription_id_.has_value()) {
        view_model_->current_message.unsubscribe(*message_subscription_id_);
        message_subscription_id_.reset();
    }
    if (view_model_ && refresh_connected_subscription_id_.has_value()) {
        view_model_->is_connected.unsubscribe(*refresh_connected_subscription_id_);
        refresh_connected_subscription_id_.reset();
    }
    if (view_model_ && refresh_wipe_subscription_id_.has_value()) {
        view_model_->is_wipe_in_progress.unsubscribe(*refresh_wipe_subscription_id_);
        refresh_wipe_subscription_id_.reset();
    }
    if (view_model_ && refresh_pending_subscription_id_.has_value()) {
        view_model_->is_operation_pending.unsubscribe(*refresh_pending_subscription_id_);
        refresh_pending_subscription_id_.reset();
    }
    view_model_.reset();
}

void MainWindow::bind(std::shared_ptr<MainViewModel> view_model) {
    unbind();
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
    if (window_) {
        gtk_window_present(GTK_WINDOW(window_));
    }
}

void MainWindow::create_header_bar() {
    header_bar_ = adw_header_bar_new();
    adw_header_bar_set_title_widget(
        ADW_HEADER_BAR(header_bar_),
        adw_window_title_new("Storage Wiper", "Securely Erase Storage Devices"));

    // Refresh button
    refresh_button_ = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(refresh_button_, "Refresh disk list");
    connect_signal(G_OBJECT(refresh_button_), "clicked", G_CALLBACK(on_refresh_clicked));
    adw_header_bar_pack_start(ADW_HEADER_BAR(header_bar_), refresh_button_);

    // Primary menu
    auto* action_group = g_simple_action_group_new();
    auto* about_action = g_simple_action_new("about", nullptr);
    connect_signal(G_OBJECT(about_action), "activate", G_CALLBACK(on_about_action));
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

    const WeakState weak = callback_state_;
    const auto generation = callback_state_->generation;
    message_subscription_id_ =
        view_model_->current_message.subscribe([weak, generation](const MessageInfo& message) {
            if (message.title.empty())
                return;
            struct QueuedMessage {
                WeakState state;
                size_t generation;
                MessageInfo message;
            };
            auto* data = new QueuedMessage{weak, generation, message};
            g_idle_add_full(
                G_PRIORITY_DEFAULT_IDLE,
                +[](gpointer user_data) -> gboolean {
                    const auto& args = *static_cast<QueuedMessage*>(user_data);
                    if (auto state = args.state.lock();
                        state && state->owner && state->generation == args.generation) {
                        state->owner->show_message(args.message);
                    }
                    return G_SOURCE_REMOVE;
                },
                data, +[](gpointer user_data) { delete static_cast<QueuedMessage*>(user_data); });
        });
}

void MainWindow::bind_refresh_state() {
    if (!view_model_ || !refresh_button_)
        return;

    const WeakState weak = callback_state_;
    auto update = [weak](bool) {
        if (auto state = weak.lock(); state && state->owner) {
            state->owner->update_refresh_button_state();
            state->owner->update_close_state();
        }
    };
    refresh_connected_subscription_id_ = view_model_->is_connected.subscribe(update);
    refresh_wipe_subscription_id_ = view_model_->is_wipe_in_progress.subscribe(update);
    refresh_pending_subscription_id_ = view_model_->is_operation_pending.subscribe(update);

    update_refresh_button_state();
}

void MainWindow::update_refresh_button_state() {
    if (!view_model_ || !refresh_button_)
        return;

    gtk_widget_set_sensitive(refresh_button_, view_model_->refresh_command &&
                                                  view_model_->refresh_command->can_execute());
}

auto MainWindow::request_close() -> bool {
    if (allow_close_ || !view_model_ || !view_model_->has_pending_or_active_operations()) {
        return false;
    }
    if (close_dialog_open_)
        return true;
    close_dialog_open_ = true;
    auto body = std::string{
        "Closing now would leave work unfinished. Keep this window open to monitor its outcome.\n\n"
        "Stopping cancels pending preparation and authorization, and requests cancellation of "
        "software wipes. "
        "A cancelled software wipe can leave data partially erased."};
    if (view_model_->has_uncancellable_operations()) {
        body += "\n\nFirmware erases cannot be stopped once accepted by the device. Stopping keeps "
                "this window "
                "open until they finish.";
    }
    body += "\n\nYou can also leave started jobs running and close. Pending wipe requests will be "
            "abandoned. Reopen Storage Wiper to monitor jobs and recent results while the helper "
            "remains running.";
    auto* dialog =
        ADW_ALERT_DIALOG(adw_alert_dialog_new("Wipe work is still in progress", body.c_str()));
    adw_alert_dialog_add_responses(dialog, "keep", "Keep Open", "stop",
                                   "Stop and Close When Finished", "leave",
                                   "Leave Started Jobs Running and Close", nullptr);
    adw_alert_dialog_set_default_response(dialog, "keep");
    adw_alert_dialog_set_close_response(dialog, "keep");
    adw_alert_dialog_set_response_appearance(dialog, "stop", ADW_RESPONSE_DESTRUCTIVE);
    connect_signal(G_OBJECT(dialog), "response",
                   G_CALLBACK(+[](AdwAlertDialog*, const char* response, gpointer data) {
                       if (auto state = static_cast<WeakState*>(data)->lock();
                           state && state->owner) {
                           auto* self = state->owner;
                           self->close_dialog_open_ = false;
                           if (g_strcmp0(response, "leave") == 0) {
                               self->allow_close_ = true;
                               if (self->view_model_)
                                   self->view_model_->cleanup();
                               gtk_window_close(GTK_WINDOW(self->window_));
                               return;
                           }
                           self->close_when_idle_ = g_strcmp0(response, "stop") == 0;
                           if (self->close_when_idle_ && self->view_model_) {
                               self->view_model_->stop_for_close();
                               self->update_close_state();
                           }
                       }
                   }));
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window_));
    return true;
}

void MainWindow::update_close_state() {
    if (!close_when_idle_ || close_idle_scheduled_ || !view_model_ ||
        view_model_->has_pending_or_active_operations())
        return;
    close_idle_scheduled_ = true;
    g_idle_add_full(
        G_PRIORITY_DEFAULT_IDLE,
        +[](gpointer data) -> gboolean {
            if (auto state = static_cast<WeakState*>(data)->lock(); state && state->owner) {
                auto* self = state->owner;
                self->close_idle_scheduled_ = false;
                if (self->close_when_idle_ && self->window_ && self->view_model_ &&
                    !self->view_model_->has_pending_or_active_operations()) {
                    gtk_window_close(GTK_WINDOW(self->window_));
                }
            }
            return G_SOURCE_REMOVE;
        },
        new WeakState(callback_state_),
        +[](gpointer data) { delete static_cast<WeakState*>(data); });
}

void MainWindow::show_message(const MessageInfo& message) {
    if (!window_)
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
        const WeakState weak = callback_state_;
        const auto generation = callback_state_->generation;
        auto* callback_ptr = new std::function<void(bool)>(
            [weak, generation, callback = message.confirmation_callback](bool confirmed) {
                const auto state = weak.lock();
                callback(confirmed && state && state->owner && state->generation == generation);
            });
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
    if (auto state = static_cast<WeakState*>(user_data)->lock(); state && state->owner) {
        auto* self = state->owner;
        if (self->view_model_ && self->view_model_->refresh_command) {
            self->view_model_->refresh_command->execute();
        }
    }
}

void MainWindow::on_about_action(GSimpleAction*, GVariant*, gpointer user_data) {
    if (auto state = static_cast<WeakState*>(user_data)->lock(); state && state->owner) {
        state->owner->show_about_dialog();
    }
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
