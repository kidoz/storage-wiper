#pragma once

#include "models/WipeJobSnapshot.hpp"
#include "services/DBusSignatures.hpp"
#include "services/WipePlanVariant.hpp"

#include <cmath>

namespace wipe_job_variant {

inline constexpr guint32 VERSION = 1;

inline auto encode(const WipeJobSnapshot& job) -> GVariant* {
    const auto& p = job.progress;
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&builder, "{sv}", "version", g_variant_new_uint32(VERSION));
    g_variant_builder_add(&builder, "{sv}", "plan", wipe_plan_variant::encode(job.plan));
    g_variant_builder_add(&builder, "{sv}", "started-at", g_variant_new_int64(job.started_at_unix));
    g_variant_builder_add(&builder, "{sv}", "completed-at",
                          g_variant_new_int64(job.completed_at_unix));
    g_variant_builder_add(&builder, "{sv}", "verification-mismatches",
                          g_variant_new_uint64(p.verification_mismatches));
    g_variant_builder_add(&builder, "{sv}", "peak-speed",
                          g_variant_new_uint64(job.peak_speed_bytes_per_sec));
    g_variant_builder_add(
        &builder, "{sv}", "progress",
        g_variant_new(dbus_signatures::WIPE_PROGRESS, job.plan.device.disk.path.c_str(),
                      p.percentage, p.current_pass, p.total_passes, p.status.c_str(), p.is_complete,
                      p.has_error, p.error_message.c_str(), static_cast<guint64>(p.bytes_written),
                      static_cast<guint64>(p.total_bytes),
                      static_cast<guint64>(p.speed_bytes_per_sec),
                      static_cast<gint64>(p.estimated_seconds_remaining), p.verification_enabled,
                      p.verification_in_progress, p.verification_passed, p.verification_percentage,
                      static_cast<guint64>(p.bad_block_count)));
    return g_variant_builder_end(&builder);
}

inline auto decode(GVariant* value) -> std::expected<WipeJobSnapshot, util::Error> {
    const auto invalid = [] {
        return std::unexpected(util::Error{"Invalid or unsupported wipe job snapshot"});
    };
    guint32 version = 0;
    WipeJobSnapshot job;
    if (!value || !g_variant_is_of_type(value, G_VARIANT_TYPE_VARDICT) ||
        !g_variant_lookup(value, "version", "u", &version) || version != VERSION ||
        !g_variant_lookup(value, "started-at", "x", &job.started_at_unix) ||
        !g_variant_lookup(value, "completed-at", "x", &job.completed_at_unix)) {
        return invalid();
    }
    auto* plan_value = g_variant_lookup_value(value, "plan", G_VARIANT_TYPE_VARDICT);
    if (!plan_value) {
        return invalid();
    }
    auto plan = wipe_plan_variant::decode(plan_value);
    g_variant_unref(plan_value);
    if (!plan) {
        return std::unexpected(plan.error());
    }
    job.plan = std::move(*plan);
    auto* progress =
        g_variant_lookup_value(value, "progress", G_VARIANT_TYPE(dbus_signatures::WIPE_PROGRESS));
    if (!progress) {
        return invalid();
    }
    const char *path = nullptr, *status = nullptr, *error = nullptr;
    gboolean complete = FALSE, failed = FALSE, verify = FALSE, verifying = FALSE, verified = FALSE;
    guint64 written = 0, total = 0, speed = 0, bad_blocks = 0, mismatches = 0;
    gint64 remaining = -1;
    auto& p = job.progress;
    g_variant_get(progress, dbus_signatures::WIPE_PROGRESS_PARSE, &path, &p.percentage,
                  &p.current_pass, &p.total_passes, &status, &complete, &failed, &error, &written,
                  &total, &speed, &remaining, &verify, &verifying, &verified,
                  &p.verification_percentage, &bad_blocks);
    const bool valid_path = path == job.plan.device.disk.path;
    p.status = status;
    p.error_message = error;
    p.is_complete = complete != FALSE;
    p.has_error = failed != FALSE;
    p.bytes_written = written;
    p.total_bytes = total;
    p.speed_bytes_per_sec = speed;
    p.estimated_seconds_remaining = remaining;
    p.verification_enabled = verify != FALSE;
    p.verification_in_progress = verifying != FALSE;
    p.verification_passed = verified != FALSE;
    p.bad_block_count = bad_blocks;
    g_variant_unref(progress);
    // Optional for compatibility with a helper that predates the field; a
    // missing value stays zero rather than rejecting the whole snapshot.
    guint64 peak_speed = 0;
    g_variant_lookup(value, "peak-speed", "t", &peak_speed);
    job.peak_speed_bytes_per_sec = peak_speed;
    if (!g_variant_lookup(value, "verification-mismatches", "t", &mismatches) || !valid_path ||
        job.started_at_unix <= 0 ||
        (p.is_complete ? job.completed_at_unix < job.started_at_unix
                       : job.completed_at_unix != 0) ||
        !std::isfinite(p.percentage) || p.percentage < 0 || p.percentage > 100 ||
        !std::isfinite(p.verification_percentage) || p.verification_percentage < 0 ||
        p.verification_percentage > 100) {
        return invalid();
    }
    p.verification_mismatches = mismatches;
    return job;
}

}  // namespace wipe_job_variant
