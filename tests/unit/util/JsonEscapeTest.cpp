/**
 * @file JsonEscapeTest.cpp
 * @brief Unit tests for util::json_escape.
 *
 * Regression target: CLI --json output emitted disk model, mount point,
 * filesystem, and path values without escaping. Quotes, backslashes, and
 * control bytes from sysfs/mount data could produce invalid JSON.
 */

#include "util/JsonEscape.hpp"

#include <gtest/gtest.h>

#include <string>

using util::json_escape;

TEST(JsonEscape, EmptyInput_ReturnsEmpty) {
    EXPECT_EQ(json_escape(""), "");
}

TEST(JsonEscape, PlainAscii_PassesThrough) {
    EXPECT_EQ(json_escape("/dev/sda"), "/dev/sda");
    EXPECT_EQ(json_escape("Samsung SSD 970"), "Samsung SSD 970");
    EXPECT_EQ(json_escape("ext4"), "ext4");
}

TEST(JsonEscape, DoubleQuote_Escaped) {
    EXPECT_EQ(json_escape("a\"b"), "a\\\"b");
    EXPECT_EQ(json_escape("\""), "\\\"");
}

TEST(JsonEscape, Backslash_Escaped) {
    EXPECT_EQ(json_escape("a\\b"), "a\\\\b");
    EXPECT_EQ(json_escape("\\"), "\\\\");
    // A literal backslash followed by 'n' must NOT collapse to a newline escape.
    EXPECT_EQ(json_escape("\\n"), "\\\\n");
}

TEST(JsonEscape, NamedControlChars_Escaped) {
    EXPECT_EQ(json_escape("\b"), "\\b");
    EXPECT_EQ(json_escape("\f"), "\\f");
    EXPECT_EQ(json_escape("\n"), "\\n");
    EXPECT_EQ(json_escape("\r"), "\\r");
    EXPECT_EQ(json_escape("\t"), "\\t");
}

TEST(JsonEscape, OtherControlChars_HexEscaped) {
    EXPECT_EQ(json_escape(std::string{'\x00'}), "\\u0000");
    EXPECT_EQ(json_escape(std::string{'\x01'}), "\\u0001");
    EXPECT_EQ(json_escape(std::string{'\x07'}), "\\u0007");  // BEL
    EXPECT_EQ(json_escape(std::string{'\x0b'}), "\\u000b");  // VT
    EXPECT_EQ(json_escape(std::string{'\x1f'}), "\\u001f");
}

TEST(JsonEscape, Del0x7f_PassesThrough) {
    // RFC 8259 only requires escaping U+0000..U+001F. 0x7F (DEL) is allowed
    // raw in a JSON string. Confirm we don't over-escape.
    EXPECT_EQ(json_escape(std::string{'\x7f'}), std::string{'\x7f'});
}

TEST(JsonEscape, HighBitBytes_PassThrough) {
    // UTF-8 multi-byte sequences must round-trip unchanged.
    const std::string utf8 = "café \xe2\x9c\x93";  // "café ✓"
    EXPECT_EQ(json_escape(utf8), utf8);
}

TEST(JsonEscape, EmbeddedNul_DoesNotTruncate) {
    const std::string with_nul{"a\0b", 3};
    EXPECT_EQ(json_escape(with_nul), "a\\u0000b");
}

TEST(JsonEscape, MixedRealisticInput) {
    // Disk model strings from sysfs sometimes contain quotes and tabs.
    const std::string model = "Vendor \"X\"\tModel\nv1";
    EXPECT_EQ(json_escape(model), "Vendor \\\"X\\\"\\tModel\\nv1");
}

TEST(JsonEscape, AllAsciiPrintable_PassesThrough) {
    std::string s;
    for (char c = 0x20; c < 0x7f; ++c) {
        if (c == '"' || c == '\\') {
            continue;
        }
        s.push_back(c);
    }
    EXPECT_EQ(json_escape(s), s);
}
