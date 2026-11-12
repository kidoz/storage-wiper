#include "helper/services/WipeJobRegistry.hpp"
#include "services/WipeJobVariant.hpp"

#include <gtest/gtest.h>

namespace {

auto job_plan(std::size_t index = 0) -> WipePlan {
    WipePlan plan;
    plan.id = "21faedc4-a8c6-4be9-a203-ecbd5bddf20" + std::to_string(index);
    plan.device.disk.path = "/dev/test" + std::to_string(index);
    plan.device.disk.model = "Original device";
    plan.device.disk.serial = "Original serial";
    plan.device.disk.size_bytes = 4'096;
    plan.device.disk_sequence = 12 + index;
    plan.targets = {plan.device};
    plan.firmware = {"overwrite", "device", true};
    return plan;
}

auto finished_progress() -> WipeProgress {
    WipeProgress p;
    p.is_complete = true;
    p.percentage = 100;
    p.bytes_written = 4'096;
    p.total_bytes = 4'096;
    p.status = "Wipe completed";
    return p;
}

TEST(WipeJobRegistryTest, ReconnectSnapshotPreservesIdentityProgressAndOriginalTimes) {
    WipeJobRegistry jobs;
    const WipeJobRegistry::Time start{.steady = WipeJobRegistry::Clock::now(),
                                      .unix_seconds = 1'000};
    auto plan = job_plan();
    ASSERT_TRUE(jobs.begin(plan, start));
    plan.device.disk.model = "Path now names another device";
    auto p = finished_progress();
    const WipeJobRegistry::Time finish{.steady = start.steady + std::chrono::seconds{5},
                                       .unix_seconds = 1'005};
    ASSERT_TRUE(jobs.update(plan.id, p, finish));
    const auto snapshots = jobs.snapshots(finish.steady);
    ASSERT_EQ(snapshots.size(), 1u);
    EXPECT_EQ(snapshots.front().plan.device.disk.model, "Original device");
    EXPECT_EQ(snapshots.front().plan.device.disk.serial, "Original serial");
    EXPECT_EQ(snapshots.front().started_at_unix, 1'000);
    EXPECT_EQ(snapshots.front().completed_at_unix, 1'005);
    EXPECT_EQ(snapshots.front().progress, p);
    EXPECT_EQ(jobs.active_count(), 0u);
}

TEST(WipeJobRegistryTest, SnapshotTracksPeakProgressSpeedForReconnectCertificates) {
    WipeJobRegistry jobs;
    const auto plan = job_plan();
    ASSERT_TRUE(jobs.begin(plan));
    auto progress = finished_progress();
    progress.is_complete = false;
    progress.speed_bytes_per_sec = 100;
    ASSERT_TRUE(jobs.update(plan.id, progress));
    progress.speed_bytes_per_sec = 900;
    ASSERT_TRUE(jobs.update(plan.id, progress));
    progress.speed_bytes_per_sec = 200;
    progress.is_complete = true;
    ASSERT_TRUE(jobs.update(plan.id, progress));
    ASSERT_EQ(jobs.snapshots().size(), 1u);
    EXPECT_EQ(jobs.snapshots().front().peak_speed_bytes_per_sec, 900u);
}

TEST(WipeJobRegistryTest, TerminalProgressCannotRegressOrBeReplacedByLateCompletion) {
    WipeJobRegistry jobs;
    const auto plan = job_plan();
    ASSERT_TRUE(jobs.begin(plan));
    const auto terminal = finished_progress();
    ASSERT_TRUE(jobs.update(plan.id, terminal));
    WipeProgress stale;
    stale.percentage = 5;
    EXPECT_FALSE(jobs.update(plan.id, stale));
    auto late = terminal;
    late.has_error = true;
    late.error_message = "Unrelated late event";
    EXPECT_FALSE(jobs.update(plan.id, late));
    ASSERT_EQ(jobs.snapshots().size(), 1u);
    EXPECT_EQ(jobs.snapshots().front().progress, terminal);
}

TEST(WipeJobRegistryTest, CancellationIsBoundToTheJobOwner) {
    WipeJobRegistry jobs;
    const auto plan = job_plan();
    ASSERT_TRUE(jobs.begin(plan, std::string{":1.42"}));
    EXPECT_FALSE(jobs.cancellation_target(plan.id, ":1.43"));
    EXPECT_EQ(jobs.cancellation_target(plan.id, ":1.42"), plan.device.disk.path);
    // Internal shutdown checks are not tied to a caller.
    EXPECT_EQ(jobs.cancellation_target(plan.id), plan.device.disk.path);
}

TEST(WipeJobRegistryTest, OldJobCannotCancelNewJobAtReusedDevicePath) {
    WipeJobRegistry jobs;
    const auto old_plan = job_plan();
    auto replacement = job_plan(1);
    replacement.device.disk.path = old_plan.device.disk.path;
    ASSERT_TRUE(jobs.begin(old_plan));
    EXPECT_FALSE(jobs.begin(replacement));
    EXPECT_EQ(jobs.cancellation_target(old_plan.id), old_plan.device.disk.path);
    ASSERT_TRUE(jobs.update(old_plan.id, finished_progress()));
    ASSERT_TRUE(jobs.begin(replacement));
    EXPECT_FALSE(jobs.cancellation_target(old_plan.id));
    EXPECT_EQ(jobs.cancellation_target(replacement.id), replacement.device.disk.path);
}

TEST(WipeJobRegistryTest, ShutdownCancelsSoftwareButRetainsFirmwareForMonitoring) {
    WipeJobRegistry jobs;
    auto software = job_plan();
    auto firmware = job_plan(1);
    firmware.firmware = {"nvme-sanitize-crypto", "nvme-subsystem", false};
    ASSERT_TRUE(jobs.begin(software));
    ASSERT_TRUE(jobs.begin(firmware));
    EXPECT_EQ(jobs.cancellable_jobs(), std::vector<std::string>{software.id});
    EXPECT_FALSE(jobs.cancellation_target(firmware.id));
    ASSERT_TRUE(jobs.update(software.id, finished_progress()));
    EXPECT_EQ(jobs.active_count(), 1u);
    ASSERT_TRUE(jobs.update(firmware.id, finished_progress()));
    EXPECT_EQ(jobs.active_count(), 0u);
}

TEST(WipeJobRegistryTest, HistoryIsBoundedWithoutEvictingActiveJobs) {
    WipeJobRegistry jobs;
    for (std::size_t index = 0; index < WipeJobRegistry::MAX_JOBS; ++index) {
        ASSERT_TRUE(jobs.begin(job_plan(index)));
    }
    EXPECT_FALSE(jobs.begin(job_plan(WipeJobRegistry::MAX_JOBS)));
    ASSERT_TRUE(jobs.update(job_plan().id, finished_progress()));
    ASSERT_TRUE(jobs.begin(job_plan(WipeJobRegistry::MAX_JOBS)));
    EXPECT_EQ(jobs.snapshots().size(), WipeJobRegistry::MAX_JOBS);
    EXPECT_EQ(jobs.active_count(), WipeJobRegistry::MAX_JOBS);
    EXPECT_EQ(jobs.cancellation_target(job_plan(1).id), job_plan(1).device.disk.path);
}

TEST(WipeJobRegistryTest, CompletedHistoryExpiresButActiveJobsDoNot) {
    WipeJobRegistry jobs;
    const WipeJobRegistry::Time now{.steady = WipeJobRegistry::Clock::now(), .unix_seconds = 1'000};
    ASSERT_TRUE(jobs.begin(job_plan(), now));
    ASSERT_TRUE(jobs.begin(job_plan(1), now));
    ASSERT_TRUE(jobs.update(job_plan().id, finished_progress(), now));
    const auto snapshots = jobs.snapshots(now.steady + WipeJobRegistry::COMPLETED_LIFETIME);
    ASSERT_EQ(snapshots.size(), 1u);
    EXPECT_EQ(snapshots.front().plan.id, job_plan(1).id);
    EXPECT_EQ(jobs.active_count(), 1u);
}

TEST(WipeJobRegistryTest, RejectedStartReleasesRegistryCapacityAndIgnoresQueuedFailure) {
    WipeJobRegistry jobs;
    const auto plan = job_plan();
    ASSERT_TRUE(jobs.begin(plan));
    jobs.abandon(plan.id);
    EXPECT_EQ(jobs.active_count(), 0u);
    EXPECT_TRUE(jobs.snapshots().empty());
    EXPECT_FALSE(jobs.update(plan.id, finished_progress()));
    EXPECT_TRUE(jobs.begin(plan));
}

TEST(WipeJobVariantTest, RoundTripPreservesAuditSnapshotAndVerificationMismatches) {
    WipeJobSnapshot job;
    job.plan = job_plan();
    job.progress = finished_progress();
    job.progress.verification_mismatches = 7;
    job.started_at_unix = 1'000;
    job.completed_at_unix = 1'005;
    job.peak_speed_bytes_per_sec = 123'456;
    auto* encoded = g_variant_ref_sink(wipe_job_variant::encode(job));
    const auto decoded = wipe_job_variant::decode(encoded);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded->plan.device, job.plan.device);
    EXPECT_EQ(decoded->plan.firmware, job.plan.firmware);
    EXPECT_EQ(decoded->progress, job.progress);
    EXPECT_EQ(decoded->started_at_unix, job.started_at_unix);
    EXPECT_EQ(decoded->completed_at_unix, job.completed_at_unix);
    EXPECT_EQ(decoded->peak_speed_bytes_per_sec, job.peak_speed_bytes_per_sec);
    g_variant_unref(encoded);
}

TEST(WipeJobVariantTest, RejectsWrongTypesAndInconsistentCompletionTimestamp) {
    auto* invalid = g_variant_ref_sink(g_variant_new_string("wrong reply"));
    EXPECT_FALSE(wipe_job_variant::decode(invalid));
    g_variant_unref(invalid);
    WipeJobSnapshot job;
    job.plan = job_plan();
    job.started_at_unix = 1'000;
    job.progress = finished_progress();
    auto* encoded = g_variant_ref_sink(wipe_job_variant::encode(job));
    EXPECT_FALSE(wipe_job_variant::decode(encoded));
    g_variant_unref(encoded);
}

}  // namespace
