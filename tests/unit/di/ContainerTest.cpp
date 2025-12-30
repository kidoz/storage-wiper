/**
 * @file ContainerTest.cpp
 * @brief Unit tests for DI Container
 */

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <thread>
#include <vector>

#include "di/Container.hpp"
#include "services/IDiskService.hpp"
#include "mocks/MockDiskService.hpp"

class ContainerTest : public ::testing::Test {
protected:
    di::Container container;

    void TearDown() override {
        container.clear();
    }
};

// Test: register_type creates correct interface mapping
TEST_F(ContainerTest, RegisterType_ResolvesCorrectImplementation) {
    container.register_type<IDiskService, MockDiskService>();

    auto resolved = container.resolve<IDiskService>();

    ASSERT_NE(resolved, nullptr);
    EXPECT_NE(dynamic_cast<MockDiskService*>(resolved.get()), nullptr);
}

// Test: singleton lifetime returns same instance
TEST_F(ContainerTest, SingletonLifetime_ReturnsSameInstance) {
    container.register_type<IDiskService, MockDiskService>(di::Lifetime::SINGLETON);

    auto first = container.resolve<IDiskService>();
    auto second = container.resolve<IDiskService>();

    EXPECT_EQ(first.get(), second.get());
}

// Test: transient lifetime creates new instances
TEST_F(ContainerTest, TransientLifetime_CreatesNewInstances) {
    container.register_type<IDiskService, MockDiskService>(di::Lifetime::TRANSIENT);

    auto first = container.resolve<IDiskService>();
    auto second = container.resolve<IDiskService>();

    EXPECT_NE(first.get(), second.get());
}

// Test: register_factory with lambda
TEST_F(ContainerTest, RegisterFactory_UsesProvidedFactory) {
    bool factory_called = false;
    container.register_factory<IDiskService>([&factory_called]() {
        factory_called = true;
        return std::make_shared<MockDiskService>();
    });

    auto resolved = container.resolve<IDiskService>();

    EXPECT_TRUE(factory_called);
    EXPECT_NE(resolved, nullptr);
}

// Test: register_instance stores pre-created instance
TEST_F(ContainerTest, RegisterInstance_ReturnsExactInstance) {
    auto instance = std::make_shared<MockDiskService>();
    container.register_instance<IDiskService>(instance);

    auto resolved = container.resolve<IDiskService>();

    EXPECT_EQ(resolved.get(), instance.get());
}

// Test: resolving unregistered type throws
TEST_F(ContainerTest, ResolveUnregistered_ThrowsException) {
    EXPECT_THROW((void)container.resolve<IDiskService>(), std::runtime_error);
}

// Test: is_registered returns correct status
TEST_F(ContainerTest, IsRegistered_ReturnsCorrectStatus) {
    EXPECT_FALSE(container.is_registered<IDiskService>());

    container.register_type<IDiskService, MockDiskService>();

    EXPECT_TRUE(container.is_registered<IDiskService>());
}

// Test: clear removes all registrations
TEST_F(ContainerTest, Clear_RemovesAllRegistrations) {
    container.register_type<IDiskService, MockDiskService>();
    EXPECT_EQ(container.size(), 1u);

    container.clear();

    EXPECT_EQ(container.size(), 0u);
    EXPECT_FALSE(container.is_registered<IDiskService>());
}

// Test: size returns correct count
TEST_F(ContainerTest, Size_ReturnsCorrectCount) {
    EXPECT_EQ(container.size(), 0u);

    container.register_type<IDiskService, MockDiskService>();
    EXPECT_EQ(container.size(), 1u);
}

// Test: re-registering replaces previous registration
TEST_F(ContainerTest, ReRegister_ReplacesPrevious) {
    auto first_instance = std::make_shared<MockDiskService>();
    auto second_instance = std::make_shared<MockDiskService>();

    container.register_instance<IDiskService>(first_instance);
    container.register_instance<IDiskService>(second_instance);

    auto resolved = container.resolve<IDiskService>();
    EXPECT_EQ(resolved.get(), second_instance.get());
}

// Test: thread-safety of registration and resolution
TEST_F(ContainerTest, ConcurrentAccess_IsThreadSafe) {
    container.register_type<IDiskService, MockDiskService>(di::Lifetime::SINGLETON);

    std::vector<std::thread> threads;
    std::vector<std::shared_ptr<IDiskService>> results(10);

    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([this, &results, i]() {
            results[i] = container.resolve<IDiskService>();
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // All should be same singleton instance
    for (const auto& result : results) {
        ASSERT_NE(result, nullptr);
        EXPECT_EQ(result.get(), results[0].get());
    }
}

// Test: factory singleton caches after first call
TEST_F(ContainerTest, FactorySingleton_CachesInstance) {
    int call_count = 0;
    container.register_factory<IDiskService>([&call_count]() {
        call_count++;
        return std::make_shared<MockDiskService>();
    }, di::Lifetime::SINGLETON);

    (void)container.resolve<IDiskService>();
    (void)container.resolve<IDiskService>();
    (void)container.resolve<IDiskService>();

    EXPECT_EQ(call_count, 1);
}

// Test: factory transient creates new each time
TEST_F(ContainerTest, FactoryTransient_CreatesEachTime) {
    int call_count = 0;
    container.register_factory<IDiskService>([&call_count]() {
        call_count++;
        return std::make_shared<MockDiskService>();
    }, di::Lifetime::TRANSIENT);

    (void)container.resolve<IDiskService>();
    (void)container.resolve<IDiskService>();
    (void)container.resolve<IDiskService>();

    EXPECT_EQ(call_count, 3);
}

// Test: ServiceLocator singleton behavior
TEST(ServiceLocatorTest, Configure_SetsUpGlobalContainer) {
    di::ServiceLocator::reset();

    di::ServiceLocator::configure([](di::Container& c) {
        c.register_type<IDiskService, MockDiskService>();
    });

    auto resolved = di::ServiceLocator::resolve<IDiskService>();
    EXPECT_NE(resolved, nullptr);

    di::ServiceLocator::reset();
}

// Test: ServiceLocator reset clears all
TEST(ServiceLocatorTest, Reset_ClearsGlobalContainer) {
    di::ServiceLocator::configure([](di::Container& c) {
        c.register_type<IDiskService, MockDiskService>();
    });

    di::ServiceLocator::reset();

    EXPECT_THROW((void)di::ServiceLocator::resolve<IDiskService>(), std::runtime_error);
}

// Test: ServiceLocator returns same instance as singleton
TEST(ServiceLocatorTest, Singleton_ReturnsSameInstance) {
    di::ServiceLocator::reset();

    di::ServiceLocator::configure([](di::Container& c) {
        c.register_type<IDiskService, MockDiskService>(di::Lifetime::SINGLETON);
    });

    auto first = di::ServiceLocator::resolve<IDiskService>();
    auto second = di::ServiceLocator::resolve<IDiskService>();

    EXPECT_EQ(first.get(), second.get());

    di::ServiceLocator::reset();
}
