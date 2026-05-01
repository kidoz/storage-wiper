#ifndef STORAGE_WIPER_HELPER_SERVICES_DEVICE_PATH_MATCHER_HPP
#define STORAGE_WIPER_HELPER_SERVICES_DEVICE_PATH_MATCHER_HPP

#include <cctype>
#include <string_view>

namespace device_path_matcher {

namespace detail {

// Devices whose partitions use 'p' as a separator: /dev/nvme0n1p1, /dev/mmcblk0p1.
// /dev/sd* and /dev/vd* attach the partition number directly (/dev/sda1).
constexpr auto uses_p_separator(std::string_view parent) noexcept -> bool {
    return parent.starts_with("/dev/nvme") || parent.starts_with("/dev/mmcblk");
}

constexpr auto is_all_digits(std::string_view s) noexcept -> bool {
    if (s.empty()) {
        return false;
    }
    for (const char c : s) {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
            return false;
        }
    }
    return true;
}

}  // namespace detail

// Generic "looks like a partition suffix": pure digits ("1", "10") or 'p' + digits
// ("p1", "p10"). Use is_device_or_partition_of for actual device matching - this
// helper is parent-class-agnostic and cannot tell a partition from a sibling
// namespace (e.g. nvme0n1 vs nvme0n10).
constexpr auto is_partition_suffix(std::string_view suffix) noexcept -> bool {
    if (suffix.empty()) {
        return false;
    }
    if (suffix.front() == 'p') {
        suffix.remove_prefix(1);
    }
    return detail::is_all_digits(suffix);
}

// True if `candidate` is `parent` itself or one of its partition nodes.
// Parent-aware: enforces the correct separator rule for the device class so
// that /dev/sda does not match /dev/sdaa1, and /dev/nvme0n1 does not match
// /dev/nvme0n10. Both inputs are expected to be canonical /dev/* paths.
constexpr auto is_device_or_partition_of(std::string_view parent,
                                         std::string_view candidate) noexcept -> bool {
    if (parent.empty() || candidate.empty()) {
        return false;
    }
    if (candidate == parent) {
        return true;
    }
    if (!candidate.starts_with(parent)) {
        return false;
    }
    auto suffix = candidate.substr(parent.size());
    if (detail::uses_p_separator(parent)) {
        if (suffix.empty() || suffix.front() != 'p') {
            return false;
        }
        suffix.remove_prefix(1);
    }
    return detail::is_all_digits(suffix);
}

}  // namespace device_path_matcher

#endif  // STORAGE_WIPER_HELPER_SERVICES_DEVICE_PATH_MATCHER_HPP
