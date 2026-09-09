#include "services/DBusClient.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

namespace {

constexpr auto NAME = "su.kidoz.storage_wiper.Helper";
constexpr auto PATH = "/su/kidoz/storage_wiper/Helper";
constexpr auto XML = R"(<node><interface name="su.kidoz.storage_wiper.Helper">
  <method name="StartWipe">
    <arg type="s" direction="in"/><arg type="u" direction="in"/>
    <arg type="b" direction="in"/><arg type="b" direction="out"/>
    <arg type="s" direction="out"/>
  </method>
</interface></node>)";

class DBusClientTest : public testing::Test {
protected:
    static GTestDBus* bus;
    GDBusConnection* helper = nullptr;
    GMainLoop* loop = nullptr;
    std::thread helper_thread;
    std::unique_ptr<DBusClient> client;
    std::atomic<bool> reject{false};

    static void SetUpTestSuite() {
        bus = g_test_dbus_new(G_TEST_DBUS_NONE);
        g_test_dbus_up(bus);
    }
    static void TearDownTestSuite() {
        g_test_dbus_down(bus);
        g_object_unref(bus);
    }

    static void method_call(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                            const gchar*, GVariant*, GDBusMethodInvocation* invocation,
                            gpointer data) {
        const auto* self = static_cast<DBusClientTest*>(data);
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
            auto* node = g_dbus_node_info_new_for_xml(XML, nullptr);
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

    void emit(const char* path, bool complete, bool failed = false) {
        ASSERT_TRUE(g_dbus_connection_emit_signal(
            helper, nullptr, PATH, NAME, "WipeProgress",
            g_variant_new("(sdiisbbstttxbbbdt)", path, 50.0, 1, 3, "test", complete, failed,
                          failed ? "cancelled" : "", guint64{1'024}, guint64{2'048}, guint64{1},
                          gint64{0}, false, false, false, 0.0, guint64{4}),
            nullptr));
        g_dbus_connection_flush_sync(helper, nullptr, nullptr);
    }
};

GTestDBus* DBusClientTest::bus = nullptr;

TEST_F(DBusClientTest, DisconnectTerminatesAllCallbacksAndAllowsRestart) {
    int failures = 0;
    auto callback = [&](const WipeProgress& progress) {
        EXPECT_TRUE(progress.is_complete);
        EXPECT_TRUE(progress.has_error);
        EXPECT_FALSE(progress.error_message.empty());
        ++failures;
        // A callback may call the client again; no client mutex may be held.
        EXPECT_FALSE(client->wipe_disk("/dev/sdc", WipeAlgorithm::ZERO_FILL, nullptr));
    };
    ASSERT_TRUE(client->wipe_disk("/dev/sda", WipeAlgorithm::ZERO_FILL, callback));
    ASSERT_TRUE(client->wipe_disk("/dev/sdb", WipeAlgorithm::ZERO_FILL, callback));
    change_name("ReleaseName");
    ASSERT_TRUE(pump_until([&] { return failures == 2; }));
    change_name("RequestName");
    ASSERT_TRUE(pump_until([&] { return client->is_connected(); }));
    int completed = 0;
    ASSERT_TRUE(
        client->wipe_disk("/dev/sda", WipeAlgorithm::ZERO_FILL, [&](const auto&) { ++completed; }));
    emit("/dev/sda", true);
    ASSERT_TRUE(pump_until([&] { return completed == 1; }));
    EXPECT_EQ(failures, 2);
}

TEST_F(DBusClientTest, RoutesParallelProgressAndRemovesOnlyCompletedDevice) {
    int first = 0;
    int second = 0;
    ASSERT_TRUE(client->wipe_disk("/dev/sda", WipeAlgorithm::ZERO_FILL, [&](const auto& p) {
        ++first;
        EXPECT_EQ(p.total_passes, 3);
        EXPECT_EQ(p.bad_block_count, 4u);
    }));
    ASSERT_TRUE(
        client->wipe_disk("/dev/sdb", WipeAlgorithm::ZERO_FILL, [&](const auto&) { ++second; }));
    EXPECT_FALSE(client->wipe_disk("/dev/sda", WipeAlgorithm::ZERO_FILL, nullptr));
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
    EXPECT_FALSE(client->wipe_disk("/dev/sda", WipeAlgorithm::ZERO_FILL, nullptr));
    reject.store(false);
    int completed = 0;
    ASSERT_TRUE(
        client->wipe_disk("/dev/sda", WipeAlgorithm::ZERO_FILL, [&](const auto&) { ++completed; }));
    emit("/dev/sda", true);
    EXPECT_TRUE(pump_until([&] { return completed == 1; }));
}

}  // namespace
