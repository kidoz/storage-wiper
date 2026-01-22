#include "views/DiskRow.hpp"

#include <format>

DiskRow::DiskRow(const DiskInfo& disk) : disk_(disk) {
    setup_from_builder();
    populate_from_disk_info();
}

void DiskRow::setup_from_builder() {
    // Load UI from GResource
    auto builder = Gtk::Builder::create_from_resource("/org/storage/wiper/ui/disk-row.ui");

    // Get the row content box
    auto* row_content = builder->get_widget<Gtk::Box>("row_content");
    if (!row_content) {
        throw std::runtime_error("Failed to load disk-row.ui: row_content not found");
    }

    // Set the content as our child
    set_child(*row_content);

    // Get references to child widgets
    disk_icon_ = builder->get_widget<Gtk::Image>("disk_icon");
    name_label_ = builder->get_widget<Gtk::Label>("name_label");
    info_label_ = builder->get_widget<Gtk::Label>("info_label");
    mounted_label_ = builder->get_widget<Gtk::Label>("mounted_label");
    health_box_ = builder->get_widget<Gtk::Box>("health_box");
    health_icon_ = builder->get_widget<Gtk::Image>("health_icon");
    health_label_ = builder->get_widget<Gtk::Label>("health_label");

    if (!disk_icon_ || !name_label_ || !info_label_ || !mounted_label_) {
        throw std::runtime_error("Failed to load disk-row.ui: required widgets not found");
    }
}

void DiskRow::populate_from_disk_info() {
    // Set disk name with model (using markup for bold path)
    auto name_markup = std::format("<b>{}</b> - {}", disk_.path, disk_.model);
    name_label_->set_markup(name_markup);

    // Build info text: size, type, LVM/mount status
    auto size_gb = static_cast<double>(disk_.size_bytes) / (1024.0 * 1024.0 * 1024.0);
    auto info_text = std::format("{:.1f} GB", size_gb);

    if (disk_.is_ssd) {
        info_text += " (SSD)";
    }

    if (disk_.is_lvm_pv) {
        info_text += " [LVM]";
    }

    if (disk_.is_mounted) {
        info_text += " - Mounted at " + disk_.mount_point;
    }

    info_label_->set_text(info_text);

    // Setup health indicator from SMART data
    setup_health_indicator();

    // Show/hide mounted indicator (also show if LVM volume is mounted)
    mounted_label_->set_visible(disk_.is_mounted);
}

void DiskRow::setup_health_indicator() {
    if (!health_box_ || !health_icon_ || !health_label_) {
        return;  // Widgets not available
    }

    // Only show if SMART data is available
    if (!disk_.smart.available) {
        health_box_->set_visible(false);
        return;
    }

    health_box_->set_visible(true);

    // Set icon and label based on health status
    // Using standard Adwaita symbolic icons
    switch (disk_.smart.status) {
        case SmartData::HealthStatus::GOOD:
            health_icon_->set_from_icon_name("object-select-symbolic");
            health_label_->set_text("Good");
            health_label_->remove_css_class("warning");
            health_label_->remove_css_class("error");
            health_label_->add_css_class("success");
            break;

        case SmartData::HealthStatus::WARNING:
            health_icon_->set_from_icon_name("warning-symbolic");
            health_label_->set_text("Warning");
            health_label_->remove_css_class("success");
            health_label_->remove_css_class("error");
            health_label_->add_css_class("warning");
            break;

        case SmartData::HealthStatus::CRITICAL:
            health_icon_->set_from_icon_name("error-symbolic");
            health_label_->set_text("Critical");
            health_label_->remove_css_class("success");
            health_label_->remove_css_class("warning");
            health_label_->add_css_class("error");
            break;

        case SmartData::HealthStatus::UNKNOWN:
        default:
            health_box_->set_visible(false);
            break;
    }

    // Set tooltip with SMART details
    std::string tooltip;
    if (disk_.smart.power_on_hours >= 0) {
        tooltip += std::format("Power-on: {} hours\n", disk_.smart.power_on_hours);
    }
    if (disk_.smart.temperature_celsius >= 0) {
        tooltip += std::format("Temperature: {}°C\n", disk_.smart.temperature_celsius);
    }
    if (disk_.smart.reallocated_sectors >= 0) {
        tooltip += std::format("Reallocated sectors: {}\n", disk_.smart.reallocated_sectors);
    }
    if (disk_.smart.pending_sectors >= 0) {
        tooltip += std::format("Pending sectors: {}\n", disk_.smart.pending_sectors);
    }

    if (!tooltip.empty()) {
        // Remove trailing newline
        tooltip.pop_back();
        health_box_->set_tooltip_text(tooltip);
    }
}
