/**
 * @file DBusSignatures.hpp
 * @brief Single source of truth for the GVariant signatures shared by the
 *        privileged helper and the D-Bus client
 *
 * The helper builds records with these strings and the client parses them. They
 * are NUL-terminated character arrays so they can be handed straight to the
 * GVariant varargs functions.
 * GVariant format strings are varargs, so a stray type code compiles fine and
 * fails only at runtime with a GLib-CRITICAL and an empty result. Keeping the
 * build and parse forms side by side, with a compile-time count of their type
 * codes, turns that class of bug into a build error.
 */

#pragma once

#include <cstddef>
#include <string_view>

namespace dbus_signatures {

/**
 * @brief Number of basic type codes in a GVariant format string
 *
 * Container brackets, the array marker at the start, and the `&` borrow
 * prefix used on the parse side carry no value of their own and are skipped.
 */
[[nodiscard]] consteval auto type_code_count(std::string_view format) -> std::size_t {
    std::size_t count = 0;
    for (std::size_t i = 0; i < format.size(); ++i) {
        const char code = format[i];
        if (code == '(' || code == ')' || code == '&') {
            continue;
        }
        if (code == 'a' && i + 1 < format.size() && format[i + 1] == '(') {
            continue;  // array-of-tuple marker, not a value
        }
        ++count;
    }
    return count;
}

// --- GetDisks -----------------------------------------------------------------
//
// One record per block device, in this order:
//   s path            s model              s serial            x size_bytes
//   b is_removable    b is_ssd             s filesystem        b is_mounted
//   s mount_point     u smart_status       b smart_available   b smart_healthy
//   x power_on_hours  i reallocated_sectors i pending_sectors  i temperature_celsius
//   i uncorrectable_errors  i percentage_used  i available_spare_percent
//   i available_spare_threshold_percent  b is_partition  s parent_disk

inline constexpr std::size_t DISK_RECORD_FIELDS = 22;

/// Build form used by the helper with g_variant_builder_add
inline constexpr char DISK_RECORD[] = "(sssxbbsbsubbxiiiiiiibs)";

/// Parse form used by the client with g_variant_iter_next (strings borrowed)
inline constexpr char DISK_RECORD_PARSE[] = "(&s&s&sxbb&sb&subbxiiiiiiib&s)";

/// Array type advertised in the introspection XML and used for the builder
inline constexpr char DISK_ARRAY[] = "a(sssxbbsbsubbxiiiiiiibs)";

/// Complete method reply: a single-element tuple holding the array
inline constexpr char DISK_LIST_REPLY[] = "(a(sssxbbsbsubbxiiiiiiibs))";

static_assert(type_code_count(DISK_RECORD) == DISK_RECORD_FIELDS,
              "GetDisks build format must name exactly one type code per field");
static_assert(type_code_count(DISK_RECORD_PARSE) == DISK_RECORD_FIELDS,
              "GetDisks parse format must match the build format field for field");
static_assert(std::string_view{DISK_ARRAY}.substr(1) == DISK_RECORD,
              "GetDisks array type must wrap the record");
static_assert(std::string_view{DISK_LIST_REPLY}.substr(1, sizeof(DISK_LIST_REPLY) - 3) ==
                  DISK_ARRAY,
              "GetDisks reply must wrap the array");

// --- GetDiskSMART -------------------------------------------------------------
//
//   b available  b healthy  x power_on_hours  i reallocated_sectors  i pending_sectors
//   i temperature_celsius  i uncorrectable_errors  i percentage_used
//   i available_spare_percent  i available_spare_threshold_percent  u status

inline constexpr char SMART_RECORD[] = "(bbxiiiiiiiu)";
static_assert(type_code_count(SMART_RECORD) == 11);

// --- WipeProgress signal ------------------------------------------------------
//
//   s device_path  d percentage  i current_pass  i total_passes  s status
//   b is_complete  b has_error  s error_message  t bytes_written  t total_bytes
//   t speed_bytes_per_sec  x estimated_seconds_remaining  b verification_enabled
//   b verification_in_progress  b verification_passed  d verification_percentage
//   t bad_block_count

inline constexpr std::size_t WIPE_PROGRESS_FIELDS = 17;
inline constexpr char WIPE_PROGRESS[] = "(sdiisbbstttxbbbdt)";
inline constexpr char WIPE_PROGRESS_PARSE[] = "(&sdii&sbb&stttxbbbdt)";

static_assert(type_code_count(WIPE_PROGRESS) == WIPE_PROGRESS_FIELDS);
static_assert(type_code_count(WIPE_PROGRESS_PARSE) == WIPE_PROGRESS_FIELDS,
              "WipeProgress parse format must match the emit format field for field");

}  // namespace dbus_signatures
