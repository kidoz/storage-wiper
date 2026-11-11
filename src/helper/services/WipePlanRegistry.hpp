#pragma once

#include "models/WipePlan.hpp"
#include "util/Result.hpp"

#include <algorithm>
#include <chrono>
#include <map>

// Accessed only on the helper's main context. A plan is a one-shot capability
// bound to a D-Bus unique sender, not an authorization transferable by its ID.
class WipePlanRegistry {
public:
    using Clock = std::chrono::steady_clock;
    struct Key {
        std::string sender;
        std::string id;
    };
    static constexpr auto LIFETIME = std::chrono::minutes{2};
    static constexpr std::size_t MAX_PLANS = 32;
    static constexpr std::size_t MAX_PER_SENDER = 2;

    [[nodiscard]] auto insert(const std::string& sender, WipePlan plan,
                              Clock::time_point now = Clock::now())
        -> std::expected<void, util::Error> {
        expire(now);
        const auto count = std::count_if(plans_.begin(), plans_.end(), [&](const auto& item) {
            return item.second.sender == sender;
        });
        if (sender.empty() || plan.id.empty() || plans_.contains(plan.id)) {
            return std::unexpected(util::Error{"Invalid prepared-wipe owner or ID"});
        }
        if (plans_.size() >= MAX_PLANS || count >= static_cast<std::ptrdiff_t>(MAX_PER_SENDER)) {
            return std::unexpected(util::Error{"Too many pending wipe confirmations"});
        }
        const auto id = plan.id;
        plans_.emplace(id, Entry{sender, std::move(plan), now + LIFETIME});
        return {};
    }

    [[nodiscard]] auto take(const Key& key, Clock::time_point now = Clock::now())
        -> std::expected<WipePlan, util::Error> {
        expire(now);
        const auto it = plans_.find(key.id);
        if (it == plans_.end() || it->second.sender != key.sender) {
            return std::unexpected(util::Error{"Wipe plan expired, was already used, or belongs to "
                                               "another caller. Prepare and confirm again."});
        }
        auto plan = std::move(it->second.plan);
        plans_.erase(it);
        return plan;
    }

    auto discard(const Key& key) -> bool {
        const auto it = plans_.find(key.id);
        if (it == plans_.end() || it->second.sender != key.sender) {
            return false;
        }
        plans_.erase(it);
        return true;
    }

    void remove_sender(const std::string& sender) {
        std::erase_if(plans_, [&](const auto& item) { return item.second.sender == sender; });
    }

    void expire(Clock::time_point now = Clock::now()) {
        std::erase_if(plans_, [&](const auto& item) { return item.second.expires <= now; });
    }

private:
    struct Entry {
        std::string sender;
        WipePlan plan;
        Clock::time_point expires;
    };
    std::map<std::string, Entry> plans_;
};
