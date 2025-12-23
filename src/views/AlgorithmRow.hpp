/**
 * @file AlgorithmRow.hpp
 * @brief Algorithm selection row composite widget using gtkmm4
 *
 * Represents a single algorithm option in the algorithm selection list.
 */

#pragma once

#include "view_models/MainViewModel.hpp"  // For AlgorithmInfo
#include <gtkmm.h>

/**
 * @class AlgorithmRow
 * @brief Composite widget for displaying an algorithm option with radio button
 *
 * Displays:
 * - Radio button for selection
 * - Algorithm name with pass count
 * - Algorithm description
 */
class AlgorithmRow : public Gtk::Box {
public:
    /**
     * @brief Construct a new AlgorithmRow
     * @param algo The algorithm information to display
     * @param group_leader Optional group leader for radio button grouping (null for first)
     */
    explicit AlgorithmRow(const AlgorithmInfo& algo, Gtk::CheckButton* group_leader = nullptr);
    ~AlgorithmRow() override = default;

    // Prevent copying
    AlgorithmRow(const AlgorithmRow&) = delete;
    AlgorithmRow& operator=(const AlgorithmRow&) = delete;
    AlgorithmRow(AlgorithmRow&&) = delete;
    AlgorithmRow& operator=(AlgorithmRow&&) = delete;

    /**
     * @brief Get the algorithm this row represents
     * @return The wipe algorithm enum value
     */
    [[nodiscard]] auto get_algorithm() const -> WipeAlgorithm { return algo_.algorithm; }

    /**
     * @brief Get the radio button for grouping
     * @return Pointer to the radio button widget
     */
    [[nodiscard]] auto get_radio_button() -> Gtk::CheckButton* { return radio_button_; }

    /**
     * @brief Set the radio button active state
     * @param active Whether the radio button should be selected
     */
    void set_active(bool active);

    /**
     * @brief Check if this row is selected
     * @return True if the radio button is active
     */
    [[nodiscard]] auto is_active() const -> bool;

    /**
     * @brief Signal emitted when this algorithm is selected
     * @return Reference to the toggled signal
     */
    auto signal_toggled() -> sigc::signal<void()>& { return signal_toggled_; }

private:
    AlgorithmInfo algo_;

    // Child widgets from UI template
    Gtk::CheckButton* radio_button_ = nullptr;
    Gtk::Label* name_label_ = nullptr;
    Gtk::Label* description_label_ = nullptr;

    // Custom signal for selection notification
    sigc::signal<void()> signal_toggled_;

    void setup_from_builder(Gtk::CheckButton* group_leader);
    void populate_from_algorithm_info();
    void on_radio_toggled();
};
