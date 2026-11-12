#pragma once

#include "models/WipePlan.hpp"

// A reconnect snapshot belongs to the prepared plan UUID, never just its reusable
// device path. Timestamps are Unix seconds from the helper, including after reattach.
struct WipeJobSnapshot {
    WipePlan plan;
    WipeProgress progress;
    int64_t started_at_unix = 0;
    int64_t completed_at_unix = 0;
    // Maximum progress speed seen over the job, kept so a certificate written
    // when the GUI reconnects carries the same throughput as the live record.
    uint64_t peak_speed_bytes_per_sec = 0;
};
