#include "helper/services/WipePlanRegistry.hpp"
#include "services/WipePlanVariant.hpp"

#include <gtest/gtest.h>

namespace {
auto plan(const std::string& id = "afbd16ae-5d6d-4ae0-a01f-d443c2d62442") -> WipePlan {
    WipePlan value;
    value.id = id;
    value.device.disk.path = "/dev/sda";
    value.device.disk.model = "Confirmed model";
    value.device.disk.serial = "Confirmed serial";
    value.device.disk.size_bytes = 4'096;
    value.device.disk.is_lvm_pv = true;
    value.device.disk.has_dm_holders = true;
    value.device.disk_sequence = 17;
    value.device.device_number = 2'048;
    value.firmware = {"overwrite", "device", true};
    value.targets = {value.device};
    return value;
}
}  // namespace

TEST(WipePlanRegistryTest, PlanIsBoundToSenderAndConsumedOnce) {
    WipePlanRegistry registry;
    const auto prepared = plan();
    ASSERT_TRUE(registry.insert(":1.1", prepared));
    EXPECT_FALSE(registry.take({.sender = ":1.2", .id = prepared.id}));
    EXPECT_FALSE(registry.discard({.sender = ":1.2", .id = prepared.id}));
    auto accepted = registry.take({.sender = ":1.1", .id = prepared.id});
    ASSERT_TRUE(accepted);
    EXPECT_EQ(accepted->device, prepared.device);
    EXPECT_FALSE(registry.take({.sender = ":1.1", .id = prepared.id}));
}

TEST(WipePlanRegistryTest, ExpiredAndDisconnectedConfirmationsCannotStart) {
    WipePlanRegistry registry;
    const auto now = WipePlanRegistry::Clock::now();
    const auto prepared = plan();
    ASSERT_TRUE(registry.insert(":1.1", prepared, now));
    EXPECT_FALSE(
        registry.take({.sender = ":1.1", .id = prepared.id}, now + WipePlanRegistry::LIFETIME));
    ASSERT_TRUE(registry.insert(":1.1", prepared));
    registry.remove_sender(":1.1");
    EXPECT_FALSE(registry.take({.sender = ":1.1", .id = prepared.id}));
}

TEST(WipePlanRegistryTest, PendingStateIsBoundedPerSenderAndGlobally) {
    WipePlanRegistry registry;
    for (std::size_t i = 0; i < WipePlanRegistry::MAX_PER_SENDER; ++i) {
        ASSERT_TRUE(registry.insert(":1.1", plan(std::to_string(i))));
    }
    EXPECT_FALSE(registry.insert(":1.1", plan("excess")));
    for (std::size_t i = WipePlanRegistry::MAX_PER_SENDER; i < WipePlanRegistry::MAX_PLANS; ++i) {
        ASSERT_TRUE(registry.insert(":1." + std::to_string(i), plan(std::to_string(i))));
    }
    EXPECT_FALSE(registry.insert(":1.99", plan("excess")));
    registry.remove_sender(":1.1");
    EXPECT_TRUE(registry.insert(":1.99", plan("available")));
}

TEST(WipePlanVariantTest, IdentityAndScopeRoundTripIncludingLvmWarning) {
    const auto prepared = plan();
    auto* encoded = g_variant_ref_sink(wipe_plan_variant::encode(prepared));
    const auto decoded = wipe_plan_variant::decode(encoded);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->device, prepared.device);
    EXPECT_EQ(decoded->firmware, prepared.firmware);
    EXPECT_EQ(decoded->targets, prepared.targets);
    EXPECT_EQ(decoded->id, prepared.id);
    g_variant_unref(encoded);
}

TEST(WipePlanVariantTest, UnknownVersionAndMissingIdentityFailClosed) {
    auto prepared = plan();
    prepared.version = 99;
    auto* encoded = g_variant_ref_sink(wipe_plan_variant::encode(prepared));
    EXPECT_FALSE(wipe_plan_variant::decode(encoded));
    g_variant_unref(encoded);
    prepared.version = WipePlan::VERSION;
    prepared.device.disk_sequence = 0;
    prepared.targets = {prepared.device};
    encoded = g_variant_ref_sink(wipe_plan_variant::encode(prepared));
    EXPECT_FALSE(wipe_plan_variant::decode(encoded));
    g_variant_unref(encoded);
}
