#pragma once
// ============================================================================
// Watchdog.h -- Thread-health monitor for MultiConnect
//
// Periodically checks heartbeat counters for registered slots.
// If a slot has not advanced its counter for two consecutive check intervals
// (i.e., 4+ seconds with the default 2-second period), a stall warning is
// logged via OutputDebugStringA.
//
// Usage:
//   Watchdog wd;
//   uint32_t slot = wd.RegisterSlot("Capture");
//   wd.Start();
//   // In the monitored thread's hot loop:
//   wd.Beat(slot);
//   // On shutdown:
//   wd.Stop();  // also called by destructor
// ============================================================================

#include "Common.h"

#include <thread>
#include <cstdio>

namespace msbt {

class Watchdog {
public:
    static constexpr uint32_t kMaxSlots     = kMaxDevices + 1;  // capture + renders
    static constexpr uint32_t kCheckPeriodMs = 2000;            // 2 seconds
    static constexpr uint32_t kStaleThreshold = 2;              // consecutive misses

    Watchdog() {
        for (uint32_t i = 0; i < kMaxSlots; ++i) {
            active_[i].store(false, std::memory_order_relaxed);
            heartbeat_[i].store(0, std::memory_order_relaxed);
            prevHeartbeat_[i] = 0;
            staleTicks_[i]    = 0;
            names_[i]         = nullptr;
        }
    }

    ~Watchdog() {
        Stop();
    }

    // Non-copyable, non-movable
    Watchdog(const Watchdog&)            = delete;
    Watchdog& operator=(const Watchdog&) = delete;

    // -----------------------------------------------------------------------
    // RegisterSlot -- claim a slot for a named thread.
    // Returns the slot index.  `name` must point to storage that outlives the
    // Watchdog (string literals are fine).
    // -----------------------------------------------------------------------
    uint32_t RegisterSlot(const char* name) {
        for (uint32_t i = 0; i < kMaxSlots; ++i) {
            bool expected = false;
            if (active_[i].compare_exchange_strong(expected, true,
                    std::memory_order_acq_rel)) {
                names_[i] = name;
                heartbeat_[i].store(0, std::memory_order_relaxed);
                prevHeartbeat_[i] = 0;
                staleTicks_[i]    = 0;
                return i;
            }
        }
        // All slots occupied -- should never happen if kMaxSlots is sized correctly
        OutputDebugStringA("[Watchdog] ERROR: no free slots\n");
        return UINT32_MAX;
    }

    // -----------------------------------------------------------------------
    // UnregisterSlot -- release a previously registered slot.
    // -----------------------------------------------------------------------
    void UnregisterSlot(uint32_t slot) {
        if (slot < kMaxSlots) {
            active_[slot].store(false, std::memory_order_release);
            names_[slot] = nullptr;
        }
    }

    // -----------------------------------------------------------------------
    // Beat -- called from the monitored thread to signal liveness.
    // This is intentionally cheap: a single relaxed atomic increment.
    // -----------------------------------------------------------------------
    void Beat(uint32_t slot) noexcept {
        if (slot < kMaxSlots) {
            heartbeat_[slot].fetch_add(1, std::memory_order_relaxed);
        }
    }

    // -----------------------------------------------------------------------
    // Start / Stop -- manage the background monitor thread.
    // -----------------------------------------------------------------------
    void Start() {
        if (running_.load(std::memory_order_acquire)) return;
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this]() { MonitorLoop(); });

        char msg[128];
        snprintf(msg, sizeof(msg),
                 "[Watchdog] Started (period=%ums, threshold=%u checks)\n",
                 kCheckPeriodMs, kStaleThreshold);
        OutputDebugStringA(msg);
    }

    void Stop() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;
        if (thread_.joinable()) {
            thread_.join();
        }
        OutputDebugStringA("[Watchdog] Stopped\n");
    }

private:
    void MonitorLoop() {
        while (running_.load(std::memory_order_acquire)) {
            Sleep(kCheckPeriodMs);
            if (!running_.load(std::memory_order_acquire)) break;

            for (uint32_t i = 0; i < kMaxSlots; ++i) {
                if (!active_[i].load(std::memory_order_acquire)) continue;

                uint64_t current = heartbeat_[i].load(std::memory_order_relaxed);
                if (current == prevHeartbeat_[i]) {
                    ++staleTicks_[i];
                    if (staleTicks_[i] >= kStaleThreshold) {
                        char warn[256];
                        snprintf(warn, sizeof(warn),
                                 "[Watchdog] STALL detected: slot %u (\"%s\") "
                                 "-- no heartbeat for %u+ seconds\n",
                                 i,
                                 names_[i] ? names_[i] : "?",
                                 staleTicks_[i] * (kCheckPeriodMs / 1000));
                        OutputDebugStringA(warn);
                    }
                } else {
                    staleTicks_[i] = 0;
                }
                prevHeartbeat_[i] = current;
            }
        }
    }

    std::atomic<bool>     running_{false};
    std::thread           thread_;

    // Per-slot data (only the monitor thread reads prev/stale; only the
    // registered thread writes heartbeat; both are fine without extra sync).
    std::atomic<bool>     active_    [kMaxSlots];
    std::atomic<uint64_t> heartbeat_ [kMaxSlots];
    uint64_t              prevHeartbeat_[kMaxSlots];
    uint32_t              staleTicks_   [kMaxSlots];
    const char*           names_        [kMaxSlots];
};

} // namespace msbt
