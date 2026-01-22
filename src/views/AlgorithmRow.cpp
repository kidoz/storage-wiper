#include "views/AlgorithmRow.hpp"

#include <format>

AlgorithmRow::AlgorithmRow(const AlgorithmInfo& algo, Gtk::CheckButton* group_leader)
    : Gtk::Box(Gtk::Orientation::HORIZONTAL, 12), algo_(algo) {
    setup_from_builder(group_leader);
    populate_from_algorithm_info();
}

void AlgorithmRow::setup_from_builder(Gtk::CheckButton* group_leader) {
    // Load UI from GResource
    auto builder = Gtk::Builder::create_from_resource("/org/storage/wiper/ui/algorithm-row.ui");

    // Get the row content box
    auto* row_content = builder->get_widget<Gtk::Box>("row_content");
    if (!row_content) {
        throw std::runtime_error("Failed to load algorithm-row.ui: row_content not found");
    }

    // Move children from the loaded widget to this widget
    while (auto* child = row_content->get_first_child()) {
        row_content->remove(*child);
        append(*child);
    }

    // Get references to child widgets
    radio_button_ = builder->get_widget<Gtk::CheckButton>("radio_button");
    name_label_ = builder->get_widget<Gtk::Label>("name_label");
    description_label_ = builder->get_widget<Gtk::Label>("description_label");

    if (!radio_button_ || !name_label_ || !description_label_) {
        throw std::runtime_error("Failed to load algorithm-row.ui: required widgets not found");
    }

    // Set up radio button group
    if (group_leader) {
        radio_button_->set_group(*group_leader);
    }

    // Connect toggled signal
    radio_button_->signal_toggled().connect(sigc::mem_fun(*this, &AlgorithmRow::on_radio_toggled));

    // Add bottom margin for spacing
    set_margin_bottom(4);
}

void AlgorithmRow::populate_from_algorithm_info() {
    // Build name text with pass count
    std::string name_text = algo_.name;
    if (algo_.pass_count > 1) {
        name_text += std::format(" ({} passes)", algo_.pass_count);
    }
    name_label_->set_text(name_text);

    // Set description
    description_label_->set_text(algo_.description);
}

void AlgorithmRow::set_active(bool active) {
    if (radio_button_) {
        radio_button_->set_active(active);
    }
}

auto AlgorithmRow::is_active() const -> bool {
    return radio_button_ ? radio_button_->get_active() : false;
}

void AlgorithmRow::on_radio_toggled() {
    // Only emit signal when this radio button becomes active
    if (radio_button_ && radio_button_->get_active()) {
        signal_toggled_.emit();
    }
}
