#pragma once

#include "models/WipeJobSnapshot.hpp"
#include "util/Result.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Main-context-owned reconnect history. Completed jobs are bounded and expire;
// an active job is never evicted merely because its GUI disconnected.
struct WipeJobTime {
    std::chrono::steady_clock::time_point steady = std::chrono::steady_clock::now();
    int64_t unix_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
};

class WipeJobRegistry {
public:
    using Clock = std::chrono::steady_clock;
    using Time = WipeJobTime;
    static constexpr std::size_t MAX_JOBS = 64;
    static constexpr auto COMPLETED_LIFETIME = std::chrono::hours{1};

    [[nodiscard]] auto begin(const WipePlan& plan, Time now = {})
        -> std::expected<void, util::Error> {
        return begin(plan, std::string{}, now);
    }

    [[nodiscard]] auto begin(const WipePlan& plan, std::string sender, Time now = {})
        -> std::expected<void, util::Error> {
        expire(now.steady);
        if (plan.id.empty() || jobs_.contains(plan.id)) {
            return std::unexpected(util::Error{"Invalid or duplicate wipe job ID"});
        }
        if (active_by_path_.contains(plan.device.disk.path)) {
            return std::unexpected(util::Error{"A wipe job is still active for this device"});
        }
        if (jobs_.size() >= MAX_JOBS) {
            auto oldest = jobs_.end();
            auto oldest_completion = Clock::time_point::max();
            for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
                const auto completion = it->second.completed.value_or(Clock::time_point::max());
                if (it->second.completed &&
                    (oldest == jobs_.end() || completion < oldest_completion)) {
                    oldest = it;
                    oldest_completion = completion;
                }
            }
            if (oldest == jobs_.end()) {
                return std::unexpected(util::Error{"Too many active wipe jobs"});
            }
            jobs_.erase(oldest);
        }
        WipeJobSnapshot snapshot;
        snapshot.plan = plan;
        snapshot.started_at_unix = now.unix_seconds;
        snapshot.progress.total_bytes = plan.device.disk.size_bytes;
        snapshot.progress.verification_enabled = plan.verify;
        snapshot.progress.status = "Starting wipe...";
        jobs_.emplace(plan.id, Entry{std::move(snapshot), std::nullopt, std::move(sender)});
        active_by_path_[plan.device.disk.path] = plan.id;
        return {};
    }

    // A start rejected by the service was never an accepted job.
    void abandon(const std::string& id) {
        const auto it = jobs_.find(id);
        if (it == jobs_.end()) {
            return;
        }
        remove_active_path(it->second.snapshot);
        jobs_.erase(it);
    }

    [[nodiscard]] auto update(const std::string& id, const WipeProgress& progress, Time now = {})
        -> bool {
        const auto it = jobs_.find(id);
        if (it == jobs_.end() || it->second.snapshot.progress.is_complete) {
            return false;
        }
        auto& entry = it->second;
        entry.snapshot.progress = progress;
        entry.snapshot.peak_speed_bytes_per_sec =
            std::max(entry.snapshot.peak_speed_bytes_per_sec, progress.speed_bytes_per_sec);
        if (progress.is_complete) {
            entry.completed = now.steady;
            entry.snapshot.completed_at_unix =
                std::max(now.unix_seconds, entry.snapshot.started_at_unix);
            remove_active_path(entry.snapshot);
        }
        return true;
    }

    [[nodiscard]] auto cancellation_target(const std::string& id,
                                           const std::string& sender = {}) const
        -> std::optional<std::string> {
        const auto it = jobs_.find(id);
        if (it == jobs_.end() || it->second.snapshot.progress.is_complete ||
            !it->second.snapshot.plan.firmware.cancellable ||
            (!sender.empty() && it->second.sender != sender)) {
            return std::nullopt;
        }
        const auto& path = it->second.snapshot.plan.device.disk.path;
        const auto active = active_by_path_.find(path);
        if (active == active_by_path_.end() || active->second != id) {
            return std::nullopt;
        }
        return path;
    }

    [[nodiscard]] auto cancellable_jobs() const -> std::vector<std::string> {
        std::vector<std::string> ids;
        for (const auto& [path, id] : active_by_path_) {
            (void)path;
            if (cancellation_target(id)) {
                ids.push_back(id);
            }
        }
        return ids;
    }

    [[nodiscard]] auto active_count() const -> std::size_t { return active_by_path_.size(); }

    [[nodiscard]] auto snapshots(Clock::time_point now = Clock::now())
        -> std::vector<WipeJobSnapshot> {
        expire(now);
        std::vector<WipeJobSnapshot> result;
        for (const auto& [id, entry] : jobs_) {
            (void)id;
            result.push_back(entry.snapshot);
        }
        std::ranges::sort(result, [](const auto& left, const auto& right) {
            if (left.started_at_unix != right.started_at_unix) {
                return left.started_at_unix < right.started_at_unix;
            }
            return left.plan.id < right.plan.id;
        });
        return result;
    }

private:
    struct Entry {
        WipeJobSnapshot snapshot;
        std::optional<Clock::time_point> completed;
        std::string sender;
    };
    std::map<std::string, Entry> jobs_;
    std::map<std::string, std::string> active_by_path_;

    void remove_active_path(const WipeJobSnapshot& job) {
        const auto active = active_by_path_.find(job.plan.device.disk.path);
        if (active != active_by_path_.end() && active->second == job.plan.id) {
            active_by_path_.erase(active);
        }
    }

    void expire(Clock::time_point now) {
        std::erase_if(jobs_, [&](const auto& item) {
            return item.second.completed && now - *item.second.completed >= COMPLETED_LIFETIME;
        });
    }
};
