#include "views/DiskRow.hpp"

#include <format>

DiskRow::DiskRow(const DiskInfo& disk)
    : disk_(disk)
{
    setup_from_builder();
    populate_from_disk_info();
}

void DiskRow::setup_from_builder() {
    // Load UI from GResource
    auto builder = Gtk::Builder::create_from_resource(
        "/org/storage/wiper/ui/disk-row.ui");

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

    if (!disk_icon_ || !name_label_ || !info_label_ || !mounted_label_) {
        throw std::runtime_error("Failed to load disk-row.ui: required widgets not found");
    }
}

void DiskRow::populate_from_disk_info() {
    // Set disk name with model (using markup for bold path)
    auto name_markup = std::format("<b>{}</b> - {}", disk_.path, disk_.model);
    name_label_->set_markup(name_markup);

    // Build info text: size, type, mount status
    auto size_gb = static_cast<double>(disk_.size_bytes) / (1024.0 * 1024.0 * 1024.0);
    auto info_text = std::format("{} GB", static_cast<int>(size_gb));

    if (disk_.is_ssd) {
        info_text += " (SSD)";
    }

    if (disk_.is_mounted) {
        info_text += " - Mounted at " + disk_.mount_point;
    }

    info_label_->set_text(info_text);

    // Show/hide mounted indicator
    mounted_label_->set_visible(disk_.is_mounted);
}
