/**
 * @file DiskRow.hpp
 * @brief Disk list row composite widget using gtkmm4
 *
 * Represents a single disk entry in the disk selection list.
 */

#pragma once

#include "models/DiskInfo.hpp"

#include <gtkmm.h>

/**
 * @class DiskRow
 * @brief Composite widget for displaying disk information in a list row
 *
 * Displays:
 * - Disk icon
 * - Disk path and model name
 * - Size, type (SSD/HDD), and mount status
 * - Mounted indicator badge
 */
class DiskRow : public Gtk::ListBoxRow {
public:
    /**
     * @brief Construct a new DiskRow from disk info
     * @param disk The disk information to display
     */
    explicit DiskRow(const DiskInfo& disk);
    ~DiskRow() override = default;

    // Prevent copying
    DiskRow(const DiskRow&) = delete;
    DiskRow& operator=(const DiskRow&) = delete;
    DiskRow(DiskRow&&) = delete;
    DiskRow& operator=(DiskRow&&) = delete;

    /**
     * @brief Get the disk path this row represents
     * @return The device path (e.g., "/dev/sda")
     */
    [[nodiscard]] auto get_disk_path() const -> std::string { return disk_.path; }

    /**
     * @brief Get the full disk info
     * @return Reference to the disk info struct
     */
    [[nodiscard]] auto get_disk_info() const -> const DiskInfo& { return disk_; }

private:
    DiskInfo disk_;

    // Child widgets from UI template
    Gtk::Image* disk_icon_ = nullptr;
    Gtk::Label* name_label_ = nullptr;
    Gtk::Label* info_label_ = nullptr;
    Gtk::Label* mounted_label_ = nullptr;

    void setup_from_builder();
    void populate_from_disk_info();
};
