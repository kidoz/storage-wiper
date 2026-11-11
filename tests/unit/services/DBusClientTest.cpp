#include "services/DBusClient.hpp"

#include "services/DBusSignatures.hpp"
#include "services/WipeJobVariant.hpp"
#include "services/WipePlanVariant.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <optional>
#include <regex>
#include <sstream>
#include <thread>

namespace {

constexpr auto NAME = "su.kidoz.storage_wiper.Helper";
constexpr auto PATH = "/su/kidoz/storage_wiper/Helper";
// A fake helper advertises GetDisks with the array type it was built against and
// answers it with a record in that same layout, exactly as a real helper does.
auto helper_xml(const char* disk_array_type) -> std::string {
    return std::string{R"(<node><interface name="su.kidoz.storage_wiper.Helper">
  <method name="PrepareWipe">
    <arg type="s" direction="in"/><arg type="u" direction="in"/>
    <arg type="b" direction="in"/><arg type="a{sv}" direction="out"/>
  </method>
  <method name="StartPreparedWipe">
    <arg type="s" direction="in"/><arg type="b" direction="out"/><arg type="s" direction="out"/>
  </method>
  <method name="DiscardWipePlan">
    <arg type="s" direction="in"/><arg type="b" direction="out"/>
  </method>
  <method name="GetWipeJobs"><arg type="aa{sv}" direction="out"/></method>
  <method name="CancelPreparedWipe">
    <arg type="s" direction="in"/><arg type="b" direction="out"/>
  </method>
  <method name="StartWipe">
    <arg type="s" direction="in"/><arg type="u" direction="in"/>
    <arg type="b" direction="in"/><arg type="b" direction="out"/>
    <arg type="s" direction="out"/>
  </method>
  <method name="GetDisks2"><arg type="u" direction="out"/><arg type=")"} +
           disk_array_type + R"(" direction="out"/></method>
</interface></node>)";
}

/// One fully populated disk record, built the way the privileged helper builds it
auto make_disk_reply(bool lvm = true, bool dm = false,
                     guint32 version = dbus_signatures::DISK_CONTRACT_VERSION) -> GVariant* {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE(dbus_signatures::DISK_ARRAY_V2));
    g_variant_builder_add(&builder, dbus_signatures::DISK_RECORD_V2, "/dev/sda1", "Model X",
                          "SERIAL42", gint64{4'096}, TRUE, FALSE, "ext4", TRUE, "/mnt/data",
                          guint32{2}, TRUE, FALSE, gint64{12'345}, 5, 1, 41, 0, 12, 100, 10, TRUE,
                          "/dev/sda", lvm ? TRUE : FALSE, dm ? TRUE : FALSE);
    return g_variant_new(dbus_signatures::DISK_LIST_REPLY_V2, version, &builder);
}

// The record layout a helper binary from an earlier package version serves: 20
// fields, before is_partition and parent_disk were appended.
constexpr auto LEGACY_DISK_ARRAY = "a(sssxbbsbsubbxiiiiiii)";
constexpr auto LEGACY_DISK_RECORD = "(sssxbbsbsubbxiiiiiii)";

/// One record in the older layout, as a helper an upgrade left running replies
auto make_legacy_disk_reply() -> GVariant* {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE(LEGACY_DISK_ARRAY));
    g_variant_builder_add(&builder, LEGACY_DISK_RECORD, "/dev/sdb", "Legacy Model", "LEGACY1",
                          gint64{2'048}, TRUE, TRUE, "ext4", FALSE, "", guint32{1}, TRUE, TRUE,
                          gint64{100}, 0, 0, 30, 0, 0, 0, 0);
    return g_variant_new("(ua(sssxbbsbsubbxiiiiiii))", guint32{1}, &builder);
}

class DBusClientTest : public testing::Test {
protected:
    std::atomic<int> prepared_starts{0};
    std::atomic<bool> cancelled_expected_plan{false};
    static GTestDBus* bus;
    GDBusConnection* helper = nullptr;
    GMainLoop* loop = nullptr;
    std::thread helper_thread;
    std::unique_ptr<DBusClient> client;
    std::atomic<bool> reject{false};
    std::atomic<int> path_only_starts{0};
    std::atomic<bool> fail_start_reply{false};
    std::atomic<bool> fail_job_query{false};
    std::atomic<bool> include_job{true};
    std::string xml{helper_xml(dbus_signatures::DISK_ARRAY_V2)};

    /// The record the fake helper serves for GetDisks
    virtual auto disk_reply() const -> GVariant* { return make_disk_reply(); }

    static void SetUpTestSuite() {
        bus = g_test_dbus_new(G_TEST_DBUS_NONE);
        g_test_dbus_up(bus);
    }
    static void TearDownTestSuite() {
        g_test_dbus_down(bus);
        g_object_unref(bus);
    }

    static auto make_plan(const std::string& path) -> WipePlan {
        WipePlan plan;
        plan.id = "72e6f913-1d4b-472f-b9e7-3423c4e841fd";
        plan.device.disk.path = path;
        plan.device.disk.size_bytes = 4'096;
        plan.device.disk_sequence = 12;
        plan.firmware = {"overwrite", "device", true};
        plan.targets = {plan.device};
        return plan;
    }

    static void method_call(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                            const gchar* method_name, GVariant* parameters,
                            GDBusMethodInvocation* invocation, gpointer data) {
        auto* self = static_cast<DBusClientTest*>(data);
        if (std::string_view{method_name} == "StartWipe") {
            ++self->path_only_starts;
        }
        if (std::string_view{method_name} == "StartPreparedWipe") {
            ++self->prepared_starts;
            if (self->fail_start_reply) {
                g_dbus_method_invocation_return_error(
                    invocation, G_DBUS_ERROR, G_DBUS_ERROR_IO_ERROR, "simulated lost start reply");
                return;
            }
        }
        if (std::string_view{method_name} == "GetWipeJobs") {
            if (self->fail_job_query) {
                g_dbus_method_invocation_return_error(
                    invocation, G_DBUS_ERROR, G_DBUS_ERROR_IO_ERROR, "simulated job-query failure");
                return;
            }
            WipeJobSnapshot job;
            job.plan = make_plan("/dev/sda");
            job.started_at_unix = 100;
            job.progress.total_bytes = 4'096;
            GVariantBuilder jobs;
            g_variant_builder_init(&jobs, G_VARIANT_TYPE("aa{sv}"));
            if (self->include_job) {
                g_variant_builder_add_value(&jobs, wipe_job_variant::encode(job));
            }
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(aa{sv})", &jobs));
            return;
        }
        if (std::string_view{method_name} == "CancelPreparedWipe") {
            const char* id = nullptr;
            g_variant_get(parameters, "(&s)", &id);
            self->cancelled_expected_plan = id == make_plan("/dev/sda").id;
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", TRUE));
            return;
        }
        if (std::string_view{method_name} == "GetDisks2") {
            g_dbus_method_invocation_return_value(invocation, self->disk_reply());
            return;
        }
        if (std::string_view{method_name} == "PrepareWipe") {
            const char* path = nullptr;
            guint32 algorithm = 0;
            gboolean verify = FALSE;
            g_variant_get(parameters, "(&sub)", &path, &algorithm, &verify);
            auto prepared = make_plan(path);
            prepared.algorithm = static_cast<WipeAlgorithm>(algorithm);
            prepared.verify = verify != FALSE;
            g_dbus_method_invocation_return_value(
                invocation, g_variant_new("(@a{sv})", wipe_plan_variant::encode(prepared)));
            return;
        }
        if (std::string_view{method_name} == "DiscardWipePlan") {
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", TRUE));
            return;
        }
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(bs)", !self->reject.load(), "test rejection"));
    }

    void change_name(const char* method) {
        auto* reply = g_dbus_connection_call_sync(
            helper, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", method,
            std::string_view{method} == "RequestName" ? g_variant_new("(su)", NAME, 0u)
                                                      : g_variant_new("(s)", NAME),
            nullptr, G_DBUS_CALL_FLAGS_NONE, 5'000, nullptr, nullptr);
        ASSERT_NE(reply, nullptr);
        g_variant_unref(reply);
    }

    auto pump_until(const std::function<bool()>& predicate) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!predicate() && std::chrono::steady_clock::now() < deadline) {
            while (g_main_context_iteration(nullptr, false)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return predicate();
    }

    void SetUp() override {
        std::promise<void> ready;
        helper_thread = std::thread([this, &ready] {
            auto* context = g_main_context_new();
            g_main_context_push_thread_default(context);
            loop = g_main_loop_new(context, false);
            helper = g_dbus_connection_new_for_address_sync(
                g_test_dbus_get_bus_address(bus),
                static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                  G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
                nullptr, nullptr, nullptr);
            auto* node = g_dbus_node_info_new_for_xml(xml.c_str(), nullptr);
            static const GDBusInterfaceVTable TABLE{method_call, nullptr, nullptr, {nullptr}};
            const auto registration = g_dbus_connection_register_object(
                helper, PATH, node->interfaces[0], &TABLE, this, nullptr, nullptr);
            change_name("RequestName");
            ready.set_value();
            g_main_loop_run(loop);
            g_dbus_connection_unregister_object(helper, registration);
            g_dbus_node_info_unref(node);
            g_main_context_pop_thread_default(context);
            g_main_context_unref(context);
        });
        ready.get_future().wait();
        client = std::make_unique<DBusClient>(G_BUS_TYPE_SESSION);
        ASSERT_TRUE(client->connect());
        while (g_main_context_iteration(nullptr, false)) {}
    }

    void TearDown() override {
        client.reset();
        g_main_loop_quit(loop);
        helper_thread.join();
        g_dbus_connection_close_sync(helper, nullptr, nullptr);
        g_object_unref(helper);
        g_main_loop_unref(loop);
        while (g_main_context_iteration(nullptr, false)) {}
    }

    void emit(const char* path, bool complete, bool failed = false,
              const char* plan_id = "72e6f913-1d4b-472f-b9e7-3423c4e841fd") {
        ASSERT_TRUE(g_dbus_connection_emit_signal(
            helper, nullptr, PATH, NAME, "PreparedWipeProgress",
            g_variant_new("(s@(sdiisbbstttxbbbdt))", plan_id,
                          g_variant_new(dbus_signatures::WIPE_PROGRESS, path, 50.0, 1, 3, "test",
                                        complete, failed, failed ? "cancelled" : "", guint64{1'024},
                                        guint64{2'048}, guint64{1}, gint64{0}, false, false, false,
                                        0.0, guint64{4})),
            nullptr));
        g_dbus_connection_flush_sync(helper, nullptr, nullptr);
    }
};

GTestDBus* DBusClientTest::bus = nullptr;

TEST_F(DBusClientTest, ReconnectObservesExistingJobWithoutStartingAndCancelsById) {
    auto jobs = client->get_wipe_jobs();
    ASSERT_TRUE(jobs) << jobs.error().message;
    ASSERT_EQ(jobs->size(), 1u);
    EXPECT_EQ(jobs->front().started_at_unix, 100);
    int completed = 0;
    ASSERT_TRUE(client->observe_wipe_job(jobs->front().plan, [&](const auto& progress) {
        completed += progress.is_complete ? 1 : 0;
    }));
    EXPECT_TRUE(client->cancel_operation("/dev/sda"));
    EXPECT_TRUE(cancelled_expected_plan.load());
    emit("/dev/sda", true);
    ASSERT_TRUE(pump_until([&] { return completed == 1; }));
    EXPECT_EQ(prepared_starts.load(), 0);
    EXPECT_FALSE(client->cancel_operation("/dev/sda"));
}

TEST_F(DBusClientTest, TerminalBetweenSnapshotAndObserveIsDeliveredOnce) {
    auto jobs = client->get_wipe_jobs();
    ASSERT_TRUE(jobs);
    ASSERT_EQ(jobs->size(), 1u);
    emit("/dev/sda", true);
    emit("/dev/sda", false);  // An older queued update must not replace the terminal cache.
    bool round_trip = false;
    client->get_available_disks([&](auto) { round_trip = true; });
    ASSERT_TRUE(pump_until([&] { return round_trip; }));
    int completed = 0;
    ASSERT_TRUE(client->observe_wipe_job(jobs->front().plan, [&](const auto& progress) {
        EXPECT_TRUE(progress.is_complete);
        ++completed;
    }));
    EXPECT_EQ(completed, 1);
    EXPECT_EQ(prepared_starts.load(), 0);
    EXPECT_FALSE(client->cancel_operation("/dev/sda"));
}

TEST_F(DBusClientTest, DisconnectTerminatesAllCallbacksAndAllowsRestart) {
    int failures = 0;
    auto callback = [&](const WipeProgress& progress) {
        EXPECT_TRUE(progress.is_complete);
        EXPECT_TRUE(progress.has_error);
        EXPECT_FALSE(progress.outcome_known);
        EXPECT_FALSE(progress.error_message.empty());
        ++failures;
        // A callback may call the client again; no client mutex may be held.
        EXPECT_FALSE(client->start_prepared_wipe(make_plan("/dev/sdc"), nullptr));
    };
    ASSERT_TRUE(client->start_prepared_wipe(make_plan("/dev/sda"), callback));
    ASSERT_TRUE(client->start_prepared_wipe(make_plan("/dev/sdb"), callback));
    change_name("ReleaseName");
    ASSERT_TRUE(pump_until([&] { return failures == 2; }));
    change_name("RequestName");
    ASSERT_TRUE(pump_until([&] { return client->is_connected(); }));
    int completed = 0;
    ASSERT_TRUE(
        client->start_prepared_wipe(make_plan("/dev/sda"), [&](const auto&) { ++completed; }));
    emit("/dev/sda", true);
    ASSERT_TRUE(pump_until([&] { return completed == 1; }));
    EXPECT_EQ(failures, 2);
}

TEST_F(DBusClientTest, RoutesParallelProgressAndRemovesOnlyCompletedDevice) {
    int first = 0;
    int second = 0;
    ASSERT_TRUE(client->start_prepared_wipe(make_plan("/dev/sda"), [&](const auto& p) {
        ++first;
        EXPECT_EQ(p.total_passes, 3);
        EXPECT_EQ(p.bad_block_count, 4u);
    }));
    ASSERT_TRUE(client->start_prepared_wipe(make_plan("/dev/sdb"), [&](const auto&) { ++second; }));
    EXPECT_FALSE(client->start_prepared_wipe(make_plan("/dev/sda"), nullptr));
    emit("/dev/sda", true, true);
    emit("/dev/sdb", false);
    ASSERT_TRUE(pump_until([&] { return first == 1 && second == 1; }));
    emit("/dev/sda", true);
    emit("/dev/sdb", true);
    ASSERT_TRUE(pump_until([&] { return second == 2; }));
    EXPECT_EQ(first, 1);
}

TEST_F(DBusClientTest, RejectedStartDoesNotLeaveCallbackRegistered) {
    reject.store(true);
    EXPECT_FALSE(client->start_prepared_wipe(make_plan("/dev/sda"), nullptr));
    reject.store(false);
    int completed = 0;
    ASSERT_TRUE(
        client->start_prepared_wipe(make_plan("/dev/sda"), [&](const auto&) { ++completed; }));
    emit("/dev/sda", true);
    EXPECT_TRUE(pump_until([&] { return completed == 1; }));
}

/// A helper from an earlier package version: it advertises and serves the legacy
/// 20-field record, as the helper binary an upgrade left running does.
class LegacyHelperDBusClientTest : public DBusClientTest {
protected:
    LegacyHelperDBusClientTest() { xml = helper_xml(LEGACY_DISK_ARRAY); }
    auto disk_reply() const -> GVariant* override { return make_legacy_disk_reply(); }
};

class DmBackingDBusClientTest : public DBusClientTest {
protected:
    auto disk_reply() const -> GVariant* override { return make_disk_reply(false, true); }
};

class FutureDisksDBusClientTest : public DBusClientTest {
protected:
    auto disk_reply() const -> GVariant* override { return make_disk_reply(false, false, 999); }
};

TEST_F(DmBackingDBusClientTest, DeviceMapperBackingDoesNotBecomeLvm) {
    std::optional<std::expected<std::vector<DiskInfo>, util::Error>> result;
    client->get_available_disks([&](auto value) { result = std::move(value); });
    ASSERT_TRUE(pump_until([&] { return result.has_value(); }));
    ASSERT_TRUE(result->has_value());
    ASSERT_EQ(result->value().size(), 1u);
    EXPECT_FALSE(result->value().front().is_lvm_pv);
    EXPECT_TRUE(result->value().front().has_dm_holders);
}

TEST_F(FutureDisksDBusClientTest, UnsupportedVersionFailsClosed) {
    std::optional<std::expected<std::vector<DiskInfo>, util::Error>> result;
    client->get_available_disks([&](auto value) { result = std::move(value); });
    ASSERT_TRUE(pump_until([&] { return result.has_value(); }));
    EXPECT_FALSE(result->has_value());
}

}  // namespace

// Regression: 1.5.0 shipped a helper format string with one type code more than
// the values it passed, so every GetDisks record failed to parse and both
// clients reported "No disks found". The record here is built with the exact
// string the helper uses and must come back field for field.
TEST_F(DBusClientTest, GetDisksRecordRoundTripsEveryField) {
    std::vector<DiskInfo> disks;
    std::atomic<bool> done{false};
    client->get_available_disks([&](auto result) {
        if (result) {
            disks = std::move(*result);
        }
        done.store(true);
    });
    ASSERT_TRUE(pump_until([&] { return done.load(); }));
    ASSERT_EQ(disks.size(), 1u) << "a signature mismatch yields an empty list, not an error";

    const auto& disk = disks.front();
    EXPECT_EQ(disk.path, "/dev/sda1");
    EXPECT_EQ(disk.model, "Model X");
    EXPECT_EQ(disk.serial, "SERIAL42");
    EXPECT_EQ(disk.size_bytes, 4'096u);
    EXPECT_TRUE(disk.is_removable);
    EXPECT_FALSE(disk.is_ssd);
    EXPECT_EQ(disk.filesystem, "ext4");
    EXPECT_TRUE(disk.is_mounted);
    EXPECT_EQ(disk.mount_point, "/mnt/data");
    EXPECT_EQ(disk.smart.status, SmartData::HealthStatus::WARNING);
    EXPECT_TRUE(disk.smart.available);
    EXPECT_FALSE(disk.smart.healthy);
    EXPECT_EQ(disk.smart.power_on_hours, 12'345);
    EXPECT_EQ(disk.smart.reallocated_sectors, 5);
    EXPECT_EQ(disk.smart.pending_sectors, 1);
    EXPECT_EQ(disk.smart.temperature_celsius, 41);
    EXPECT_EQ(disk.smart.uncorrectable_errors, 0);
    EXPECT_EQ(disk.smart.percentage_used, 12);
    EXPECT_EQ(disk.smart.available_spare_percent, 100);
    EXPECT_EQ(disk.smart.available_spare_threshold_percent, 10);
    EXPECT_TRUE(disk.is_partition);
    EXPECT_EQ(disk.parent_disk, "/dev/sda");
    EXPECT_TRUE(disk.is_lvm_pv);
    EXPECT_FALSE(disk.has_dm_holders);
}

// Regression: installing a new package does not replace a helper that is already
// running, so a client can be answered by a helper serving the previous record
// layout. That reply must surface as a named version skew, with both layouts in
// the message, rather than a GLib-CRITICAL and an empty list.
TEST_F(LegacyHelperDBusClientTest, RunningOlderHelperIsReportedAsVersionSkew) {
    std::optional<std::expected<std::vector<DiskInfo>, util::Error>> result;
    client->get_available_disks([&](auto value) { result = std::move(value); });
    ASSERT_TRUE(pump_until([&] { return result.has_value(); }));
    ASSERT_FALSE(result->has_value());
    EXPECT_NE(result->error().message.find("older version"), std::string::npos);
    EXPECT_NE(result->error().message.find(LEGACY_DISK_ARRAY), std::string::npos);
    EXPECT_NE(result->error().message.find(dbus_signatures::DISK_ARRAY_V2), std::string::npos);
}

// The shipped interface description must advertise the same GetDisks type the
// helper and client compile against.
TEST(DBusSignaturesTest, ShippedInterfaceDescriptionMatchesCode) {
    std::ifstream xml{std::string{SOURCE_ROOT} + "/data/dbus/su.kidoz.storage_wiper.Helper.xml"};
    ASSERT_TRUE(xml) << "interface description not found under SOURCE_ROOT";
    std::stringstream buffer;
    buffer << xml.rdbuf();

    std::smatch match;
    const std::regex pattern{R"re(<method name="GetDisks">\s*<arg name="disks" type="([^"]+)")re"};
    const std::string text = buffer.str();
    ASSERT_TRUE(std::regex_search(text, match, pattern)) << "GetDisks arg not found";
    EXPECT_EQ(match[1].str(), std::string{dbus_signatures::DISK_ARRAY});
}

TEST_F(DBusClientTest, MalformedProgressDoesNotConsumeActiveCallback) {
    int callbacks = 0;
    ASSERT_TRUE(
        client->start_prepared_wipe(make_plan("/dev/sda"), [&](const auto&) { ++callbacks; }));
    ASSERT_TRUE(g_dbus_connection_emit_signal(helper, nullptr, PATH, NAME, "PreparedWipeProgress",
                                              g_variant_new("(s)", "/dev/sda"), nullptr));
    g_dbus_connection_flush_sync(helper, nullptr, nullptr);
    // A valid event sent after the malformed one must still reach the callback.
    emit("/dev/sda", true);
    ASSERT_TRUE(pump_until([&] { return callbacks == 1; }));
}

TEST(DBusSignaturesTest, ShippedWipeMethodsAndProgressMatchCode) {
    std::ifstream xml{std::string{SOURCE_ROOT} + "/data/dbus/su.kidoz.storage_wiper.Helper.xml"};
    ASSERT_TRUE(xml);
    std::stringstream buffer;
    buffer << xml.rdbuf();
    GError* error = nullptr;
    auto* node = g_dbus_node_info_new_for_xml(buffer.str().c_str(), &error);
    ASSERT_NE(node, nullptr) << (error ? error->message : "invalid XML");
    auto* interface = g_dbus_node_info_lookup_interface(node, "su.kidoz.storage_wiper.Helper");
    ASSERT_NE(interface, nullptr);
    auto signature = [](GDBusArgInfo** arguments) {
        std::string value = "(";
        for (size_t i = 0; arguments && arguments[i]; ++i) {
            value += arguments[i]->signature;
        }
        return value + ")";
    };
    auto* start = g_dbus_interface_info_lookup_method(interface, "StartWipe");
    ASSERT_NE(start, nullptr);
    EXPECT_EQ(signature(start->in_args), dbus_signatures::START_WIPE_REQUEST);
    EXPECT_EQ(signature(start->out_args), dbus_signatures::START_WIPE_REPLY);
    auto* prepare = g_dbus_interface_info_lookup_method(interface, "PrepareWipe");
    ASSERT_NE(prepare, nullptr);
    EXPECT_EQ(signature(prepare->in_args), "(sub)");
    EXPECT_EQ(signature(prepare->out_args), "(a{sv})");
    auto* prepared_start = g_dbus_interface_info_lookup_method(interface, "StartPreparedWipe");
    ASSERT_NE(prepared_start, nullptr);
    EXPECT_EQ(signature(prepared_start->in_args), "(s)");
    EXPECT_EQ(signature(prepared_start->out_args), "(bs)");
    auto* disks_v2 = g_dbus_interface_info_lookup_method(interface, "GetDisks2");
    ASSERT_NE(disks_v2, nullptr);
    EXPECT_EQ(signature(disks_v2->out_args), dbus_signatures::DISK_LIST_REPLY_V2);
    auto* jobs = g_dbus_interface_info_lookup_method(interface, "GetWipeJobs");
    ASSERT_NE(jobs, nullptr);
    EXPECT_EQ(signature(jobs->out_args), "(aa{sv})");
    auto* cancel_prepared = g_dbus_interface_info_lookup_method(interface, "CancelPreparedWipe");
    ASSERT_NE(cancel_prepared, nullptr);
    EXPECT_EQ(signature(cancel_prepared->in_args), "(s)");
    EXPECT_EQ(signature(cancel_prepared->out_args), "(b)");
    auto* cancel = g_dbus_interface_info_lookup_method(interface, "CancelWipe");
    ASSERT_NE(cancel, nullptr);
    EXPECT_EQ(signature(cancel->in_args), dbus_signatures::CANCEL_WIPE_REQUEST);
    EXPECT_EQ(signature(cancel->out_args), dbus_signatures::CANCEL_WIPE_REPLY);
    auto* prepared_progress =
        g_dbus_interface_info_lookup_signal(interface, "PreparedWipeProgress");
    ASSERT_NE(prepared_progress, nullptr);
    EXPECT_EQ(signature(prepared_progress->args), dbus_signatures::PREPARED_WIPE_PROGRESS);
    auto* progress = g_dbus_interface_info_lookup_signal(interface, "WipeProgress");
    ASSERT_NE(progress, nullptr);
    EXPECT_EQ(signature(progress->args), dbus_signatures::WIPE_PROGRESS);
    g_dbus_node_info_unref(node);
    g_clear_error(&error);
}

TEST_F(DBusClientTest, PrepareReturnsHelperSnapshotAndLegacyStartFailsClosed) {
    auto prepared = client->prepare_wipe("/dev/sda", WipeAlgorithm::ZERO_FILL, true);
    ASSERT_TRUE(prepared) << prepared.error().message;
    EXPECT_EQ(prepared->device.disk.path, "/dev/sda");
    EXPECT_EQ(prepared->device.disk_sequence, 12u);
    EXPECT_TRUE(prepared->verify);
    EXPECT_EQ(prepared->firmware.scope, "device");
    EXPECT_FALSE(client->wipe_disk("/dev/sda", WipeAlgorithm::ZERO_FILL, nullptr));
}

TEST_F(DBusClientTest, PreviousPlansTerminalSignalCannotCompleteNewWipeOnSamePath) {
    int callbacks = 0;
    ASSERT_TRUE(
        client->start_prepared_wipe(make_plan("/dev/sda"), [&](const auto&) { ++callbacks; }));
    emit("/dev/sda", true, false, "b0f249d1-e2e7-407c-948a-934e21c2aa14");
    emit("/dev/sda", false);
    ASSERT_TRUE(pump_until([&] { return callbacks != 0; }));
    EXPECT_EQ(callbacks, 1);
    emit("/dev/sda", true);
    ASSERT_TRUE(pump_until([&] { return callbacks == 2; }));
}

class PathOnlyHelperDBusClientTest : public DBusClientTest {
protected:
    PathOnlyHelperDBusClientTest() {
        xml = R"(<node><interface name="su.kidoz.storage_wiper.Helper">
          <method name="StartWipe">
            <arg type="s" direction="in"/><arg type="u" direction="in"/>
            <arg type="b" direction="in"/><arg type="b" direction="out"/>
            <arg type="s" direction="out"/>
          </method>
        </interface></node>)";
    }
};

TEST_F(PathOnlyHelperDBusClientTest, OldHelperCannotTriggerPathOnlyFallback) {
    EXPECT_FALSE(client->prepare_wipe("/dev/sda", WipeAlgorithm::ZERO_FILL, false));
    EXPECT_EQ(path_only_starts.load(), 0);
}
