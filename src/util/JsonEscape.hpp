#ifndef STORAGE_WIPER_UTIL_JSON_ESCAPE_HPP
#define STORAGE_WIPER_UTIL_JSON_ESCAPE_HPP

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace util {

// Escape `input` for safe inclusion between the quote characters of a JSON
// string literal per RFC 8259 sec. 7. Quote, backslash, and ASCII control
// characters (< 0x20) are escaped; bytes >= 0x20 (including UTF-8 sequences)
// pass through verbatim. Callers still have to write the surrounding quotes.
inline auto json_escape(std::string_view input) -> std::string {
    std::string out;
    out.reserve(input.size());

    for (const char raw : input) {
        const auto byte = static_cast<std::uint8_t>(raw);
        switch (byte) {
            case '"':
                out.append("\\\"");
                break;
            case '\\':
                out.append("\\\\");
                break;
            case '\b':
                out.append("\\b");
                break;
            case '\f':
                out.append("\\f");
                break;
            case '\n':
                out.append("\\n");
                break;
            case '\r':
                out.append("\\r");
                break;
            case '\t':
                out.append("\\t");
                break;
            default:
                if (byte < 0x20) {
                    constexpr std::array<char, 16> hex{'0', '1', '2', '3', '4', '5', '6', '7',
                                                       '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
                    out.append("\\u00");
                    out.push_back(hex[(byte >> 4) & 0x0F]);
                    out.push_back(hex[byte & 0x0F]);
                } else {
                    out.push_back(raw);
                }
                break;
        }
    }
    return out;
}

}  // namespace util

#endif  // STORAGE_WIPER_UTIL_JSON_ESCAPE_HPP
