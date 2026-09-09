/**
 * @file WriteHelpersTest.cpp
 * @brief Unit tests for bad-sector tolerant writes
 */

#include "util/WriteHelpers.hpp"

#include "fixtures/TestFixtures.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using util::detail::sector_skipping_write;

// A pwrite_fn that fails on specific 512-byte sector indices, simulated at
// absolute offsets so the position arithmetic is exercised too.
struct FailingPWrite {
    std::vector<uint8_t>* backing;  ///< receives written bytes at their offsets
    std::vector<off_t> failing;     ///< absolute offsets of "bad sectors"

    auto operator()(off_t offset, std::span<const uint8_t> chunk) -> ssize_t {
        const bool bad = std::any_of(failing.begin(), failing.end(),
                                     [offset](off_t bad_offset) { return bad_offset == offset; });
        if (bad) {
            return -1;
        }
        if (backing->size() < static_cast<size_t>(offset) + chunk.size()) {
            backing->resize(static_cast<size_t>(offset) + chunk.size(), 0);
        }
        std::copy(chunk.begin(), chunk.end(), backing->begin() + offset);
        return static_cast<ssize_t>(chunk.size());
    }
};

constexpr size_t FOUR_SECTORS = 4 * 512;

TEST(SectorSkippingWriteTest, WritesAllSectorsWhenNoneFail) {
    std::vector<uint8_t> backing;
    FailingPWrite write{&backing, {}};

    std::vector<uint8_t> data(FOUR_SECTORS, 0xAB);
    uint64_t skipped = 0;
    const auto consumed = sector_skipping_write(write, 1'024, 0, data, skipped);

    EXPECT_EQ(consumed, FOUR_SECTORS);
    EXPECT_EQ(skipped, 0u);
    ASSERT_EQ(backing.size(), 1'024 + FOUR_SECTORS);
    EXPECT_EQ(backing[1'024], 0xAB);
    EXPECT_EQ(backing.back(), 0xAB);
}

TEST(SectorSkippingWriteTest, SkipsFailingSectorAndContinues) {
    std::vector<uint8_t> backing;
    const off_t base = 4'096;
    // Second sector of the region is "bad"
    FailingPWrite write{&backing, {base + 512}};

    std::vector<uint8_t> data(FOUR_SECTORS);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>(i);
    }
    uint64_t skipped = 0;
    const auto consumed = sector_skipping_write(write, base, 0, data, skipped);

    // The whole region is consumed even though one sector could not be written
    EXPECT_EQ(consumed, FOUR_SECTORS);
    EXPECT_EQ(skipped, 1u);

    // Sector 1 landed; sector 2 is absent; sector 3 still written
    EXPECT_EQ(backing[base], data[0]);
    EXPECT_EQ(backing[base + 512], 0u);
    EXPECT_EQ(backing[base + 3 * 512], data[3 * 512]);
}

TEST(SectorSkippingWriteTest, ContinuesAfterBytesAlreadyConsumed) {
    std::vector<uint8_t> backing;
    const off_t base = 0;
    FailingPWrite write{&backing, {2 * 512}};

    std::vector<uint8_t> data(FOUR_SECTORS, 0x5A);
    uint64_t skipped = 0;
    // Simulate a prior partial write of one sector: base + done = 512
    const auto consumed = sector_skipping_write(write, base, 512, data, skipped);

    EXPECT_EQ(consumed, FOUR_SECTORS);
    EXPECT_EQ(skipped, 1u);
    // The already-consumed first sector is not touched by this call; the
    // fallback writes the remainder starting at absolute offset 512.
    EXPECT_EQ(backing[0], 0u);
    EXPECT_EQ(backing[512], 0x5A);
    EXPECT_EQ(backing[2 * 512], 0u);  // the bad sector
    EXPECT_EQ(backing[3 * 512], 0x5A);
}

TEST(SectorSkippingWriteTest, HandlesTrailingPartialSector) {
    std::vector<uint8_t> backing;
    FailingPWrite write{&backing, {}};

    // 2.5 sectors of data: the tail chunk is smaller than 512 bytes
    std::vector<uint8_t> data(2 * 512 + 256, 0x01);
    uint64_t skipped = 0;
    const auto consumed = sector_skipping_write(write, 0, 0, data, skipped);

    EXPECT_EQ(consumed, data.size());
    EXPECT_EQ(skipped, 0u);
    ASSERT_EQ(backing.size(), data.size());
    EXPECT_EQ(backing.back(), 0x01);
}

TEST(SectorSkippingWriteTest, CountsEveryFailingSector) {
    std::vector<uint8_t> backing;
    FailingPWrite write{
        &backing, {512, 3 * 512}
    };

    std::vector<uint8_t> data(FOUR_SECTORS, 0xFF);
    uint64_t skipped = 0;
    sector_skipping_write(write, 0, 0, data, skipped);

    EXPECT_EQ(skipped, 2u);
}

TEST(WriteToleranceTest, FastPathWritesEntireTempFile) {
    TempTestFile file;
    ASSERT_TRUE(file.valid());

    const std::vector<uint8_t> data(1'024, 0x42);
    uint64_t bad_blocks = 0;
    const auto consumed = util::write_with_bad_sector_tolerance(file.fd(), data, bad_blocks);

    EXPECT_EQ(consumed, data.size());
    EXPECT_EQ(bad_blocks, 0u);
}

}  // namespace
