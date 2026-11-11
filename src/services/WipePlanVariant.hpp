#pragma once

#include "models/WipePlan.hpp"
#include "util/Result.hpp"

#include <gio/gio.h>

namespace wipe_plan_variant {

inline constexpr char IDENTITY_TYPE[] = "(sssttttbsbbbb)";
inline constexpr char TARGETS_TYPE[] = "a(sssttttbsbbbb)";

inline auto identity(const DeviceIdentity& value) -> GVariant* {
    const auto& disk = value.disk;
    return g_variant_new(
        IDENTITY_TYPE, disk.path.c_str(), disk.model.c_str(), disk.serial.c_str(),
        static_cast<guint64>(disk.size_bytes), static_cast<guint64>(value.device_number),
        static_cast<guint64>(value.disk_sequence), static_cast<guint64>(value.partition_start),
        disk.is_partition, disk.parent_disk.c_str(), disk.is_ssd, disk.is_removable, disk.is_lvm_pv,
        disk.has_dm_holders);
}

inline auto read_identity(GVariant* value) -> DeviceIdentity {
    const char *path = nullptr, *model = nullptr, *serial = nullptr, *parent = nullptr;
    guint64 size = 0, number = 0, sequence = 0, start = 0;
    gboolean partition = FALSE, ssd = FALSE, removable = FALSE, lvm = FALSE, dm = FALSE;
    g_variant_get(value, "(&s&s&sttttb&sbbbb)", &path, &model, &serial, &size, &number, &sequence,
                  &start, &partition, &parent, &ssd, &removable, &lvm, &dm);
    DeviceIdentity result;
    result.disk.path = path;
    result.disk.model = model;
    result.disk.serial = serial;
    result.disk.size_bytes = size;
    result.disk.is_partition = partition != FALSE;
    result.disk.parent_disk = parent;
    result.disk.is_ssd = ssd != FALSE;
    result.disk.is_removable = removable != FALSE;
    result.disk.is_lvm_pv = lvm != FALSE;
    result.disk.has_dm_holders = dm != FALSE;
    result.device_number = number;
    result.disk_sequence = sequence;
    result.partition_start = start;
    return result;
}

// Versioned dictionary: fields may be added without changing GetDisks or the
// legacy interface. Missing required fields and unknown versions fail closed.
inline auto encode(const WipePlan& plan) -> GVariant* {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&builder, "{sv}", "version", g_variant_new_uint32(plan.version));
    g_variant_builder_add(&builder, "{sv}", "id", g_variant_new_string(plan.id.c_str()));
    g_variant_builder_add(&builder, "{sv}", "device", identity(plan.device));
    g_variant_builder_add(&builder, "{sv}", "algorithm",
                          g_variant_new_uint32(static_cast<guint32>(plan.algorithm)));
    g_variant_builder_add(&builder, "{sv}", "verify", g_variant_new_boolean(plan.verify));
    g_variant_builder_add(&builder, "{sv}", "method",
                          g_variant_new_string(plan.firmware.method.c_str()));
    g_variant_builder_add(&builder, "{sv}", "scope",
                          g_variant_new_string(plan.firmware.scope.c_str()));
    g_variant_builder_add(&builder, "{sv}", "cancellable",
                          g_variant_new_boolean(plan.firmware.cancellable));
    GVariantBuilder targets;
    g_variant_builder_init(&targets, G_VARIANT_TYPE(TARGETS_TYPE));
    for (const auto& target : plan.targets) {
        g_variant_builder_add_value(&targets, identity(target));
    }
    g_variant_builder_add(&builder, "{sv}", "targets", g_variant_builder_end(&targets));
    return g_variant_builder_end(&builder);
}

inline auto decode(GVariant* value) -> std::expected<WipePlan, util::Error> {
    const auto invalid = [] {
        return std::unexpected(
            util::Error{"Invalid or unsupported prepared-wipe reply; update the helper"});
    };
    if (!g_variant_is_of_type(value, G_VARIANT_TYPE_VARDICT)) {
        return invalid();
    }
    WipePlan plan;
    guint32 algorithm = 0;
    gboolean verify = FALSE, cancellable = FALSE;
    const char *id = nullptr, *method = nullptr, *scope = nullptr;
    if (!g_variant_lookup(value, "version", "u", &plan.version) ||
        plan.version != WipePlan::VERSION || !g_variant_lookup(value, "id", "&s", &id) ||
        !g_uuid_string_is_valid(id) || !g_variant_lookup(value, "algorithm", "u", &algorithm) ||
        algorithm > static_cast<guint32>(WipeAlgorithm::ATA_SECURE_ERASE) ||
        !g_variant_lookup(value, "verify", "b", &verify) ||
        !g_variant_lookup(value, "method", "&s", &method) ||
        !g_variant_lookup(value, "scope", "&s", &scope) ||
        !g_variant_lookup(value, "cancellable", "b", &cancellable)) {
        return invalid();
    }
    plan.id = id;
    plan.algorithm = static_cast<WipeAlgorithm>(algorithm);
    plan.verify = verify != FALSE;
    plan.firmware = {method, scope, cancellable != FALSE};
    const bool firmware_method = plan.firmware.method == "ata-security-erase" ||
                                 plan.firmware.method == "ata-enhanced-security-erase" ||
                                 plan.firmware.method == "nvme-sanitize-crypto" ||
                                 plan.firmware.method == "nvme-sanitize-block" ||
                                 plan.firmware.method == "nvme-sanitize-overwrite" ||
                                 plan.firmware.method == "nvme-format-crypto";
    if ((plan.firmware.scope != "device" && plan.firmware.scope != "nvme-subsystem") ||
        (plan.algorithm == WipeAlgorithm::ATA_SECURE_ERASE ? !firmware_method
                                                           : plan.firmware.method != "overwrite")) {
        return invalid();
    }
    auto* device = g_variant_lookup_value(value, "device", G_VARIANT_TYPE(IDENTITY_TYPE));
    if (!device) {
        return invalid();
    }
    plan.device = read_identity(device);
    g_variant_unref(device);
    auto* targets = g_variant_lookup_value(value, "targets", G_VARIANT_TYPE(TARGETS_TYPE));
    if (!targets) {
        return invalid();
    }
    const auto count = g_variant_n_children(targets);
    if (count == 0 || count > 64) {
        g_variant_unref(targets);
        return invalid();
    }
    for (gsize i = 0; i < count; ++i) {
        auto* target = g_variant_get_child_value(targets, i);
        plan.targets.push_back(read_identity(target));
        g_variant_unref(target);
    }
    g_variant_unref(targets);
    for (const auto& target : plan.targets) {
        if (!target.disk.path.starts_with("/dev/") || target.disk.size_bytes == 0 ||
            target.disk_sequence == 0) {
            return invalid();
        }
    }
    if (plan.targets.front() != plan.device) {
        return invalid();
    }
    return plan;
}

}  // namespace wipe_plan_variant
