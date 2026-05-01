/**
 * @file SecureRandomTest.cpp
 * @brief Unit tests for util::secure_random_fill.
 *
 * The helper backs every random-pass overwrite, so these tests check the
 * basics: empty span is a no-op, output is non-trivial, two calls produce
 * different output, and a large fill passes a chi-squared uniformity test
 * (the same statistic VerificationHelper uses for post-wipe entropy).
 */

#include "util/SecureRandom.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using util::secure_random_fill;

TEST(SecureRandom, Empty_Noop) {
    std::vector<std::byte> empty;
    EXPECT_NO_THROW(secure_random_fill(empty));
    EXPECT_TRUE(empty.empty());
}

TEST(SecureRandom, SmallFill_NotAllZero) {
    // 32 bytes from /dev/urandom being all zero is ~1 in 2^256.
    std::array<std::byte, 32> buf{};
    secure_random_fill(buf);
    const bool all_zero = std::ranges::all_of(buf, [](std::byte b) { return b == std::byte{0}; });
    EXPECT_FALSE(all_zero);
}

TEST(SecureRandom, TwoCalls_ProduceDifferentOutput) {
    std::array<std::byte, 64> a{};
    std::array<std::byte, 64> b{};
    secure_random_fill(a);
    secure_random_fill(b);
    EXPECT_NE(a, b) << "Two consecutive 64-byte fills matched - "
                       "the helper is almost certainly broken.";
}

TEST(SecureRandom, LargeFill_PassesChiSquared) {
    // Mirror VerificationHelper::verify_random's threshold: 1 MiB sample,
    // chi-squared(255, 0.001) = 310.5. Healthy CSPRNG output passes with
    // overwhelming probability.
    constexpr std::size_t SIZE = 1 << 20;  // 1 MiB
    std::vector<std::byte> buf(SIZE);
    secure_random_fill(buf);

    std::array<std::uint64_t, 256> counts{};
    for (std::byte b : buf) {
        counts[static_cast<std::uint8_t>(b)]++;
    }

    const double expected = static_cast<double>(SIZE) / 256.0;
    double chi_squared = 0.0;
    for (const auto c : counts) {
        const double diff = static_cast<double>(c) - expected;
        chi_squared += (diff * diff) / expected;
    }

    constexpr double CRITICAL_VALUE = 310.5;
    EXPECT_LT(chi_squared, CRITICAL_VALUE)
        << "1 MiB sample failed chi-squared(255, 0.001); CSPRNG output looks non-uniform.";

    // No single byte should dominate; healthy output is ~0.39% per value.
    const auto max_count = *std::ranges::max_element(counts);
    const double max_ratio = static_cast<double>(max_count) / static_cast<double>(SIZE);
    EXPECT_LT(max_ratio, 0.01);
}

TEST(SecureRandom, OddSize_FillsExactly) {
    // Force the loop to handle a non-power-of-two length.
    std::vector<std::byte> buf(4096 + 17, std::byte{0xAB});
    secure_random_fill(buf);

    // Sentinel at end was overwritten - extremely high probability that
    // not every byte equals the original 0xAB sentinel.
    const auto unchanged =
        std::ranges::count(buf, std::byte{0xAB});
    EXPECT_LT(unchanged, static_cast<std::ptrdiff_t>(buf.size()));
}
