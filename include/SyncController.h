#pragma once
// ============================================================================
// SyncController -- Master Clock, Drift Metrics, Delay Alignment
//
// The SyncController is the brain of the synchronization system.  It:
//   1. Owns the master QPC clock epoch
//   2. Periodically reads drift metrics from each DeviceSyncState
//   3. Computes corrective actions (delay adjustments, resample ratios)
//   4. Pushes those corrections to each RenderEngine
//
// It runs on its own low-priority control thread (~10 Hz update rate).
// ============================================================================

#include "Common.h"
#include "RenderEngine.h"
#include <array>
#include <memory>
#include <mutex>
#include <thread>
#include <functional>

namespace msbt {

// Diagnostic snapshot for one device
struct DeviceDriftReport {
    uint32_t deviceIndex;
    bool     active;
    double   rawDriftUs;
    double   filteredDriftUs;
    uint32_t delayFrames;
    double   resampleRatio;
    uint64_t framesRendered;
    uint64_t underrunCount;
    uint64_t overrunCount;
};

// Callback for telemetry / logging
using DriftReportCallback = std::function<void(const DeviceDriftReport*, uint32_t count)>;

class SyncController {
public:
    SyncController();
    ~SyncController();

    SyncController(const SyncController&) = delete;
    SyncController& operator=(const SyncController&) = delete;

    // Register a render engine; returns the DeviceSyncState it should use
    DeviceSyncState& RegisterDevice(uint32_t index);

    // Bind a render engine pointer for correction pushes
    void BindRenderEngine(uint32_t index, RenderEngine* engine);

    // Set the master epoch (from CaptureEngine)
    void SetMasterEpoch(int64_t qpc) noexcept;
    int64_t MasterEpoch() const noexcept { return masterEpochQpc_.load(std::memory_order_acquire); }

    // Start / stop the control loop
    void Start();
    void Stop() noexcept;

    // Set telemetry callback
    void SetReportCallback(DriftReportCallback cb);

    // One-shot: compute initial delay offsets for all active devices
    void ComputeInitialAlignment();

    bool IsRunning() const noexcept { return running_.load(std::memory_order_acquire); }

private:
    void ControlThread();
    void EvaluateAndCorrect();

    // Per-device state (cache-line aligned, no false sharing)
    std::array<DeviceSyncState, kMaxDevices> deviceStates_{};
    std::array<RenderEngine*, kMaxDevices>   engines_{};

    std::atomic<int64_t> masterEpochQpc_{0};

    // Control thread
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::thread       thread_;

    DriftReportCallback reportCallback_;

    // Mutex protecting engines_[], reportCallback_, correctionStates_[]
    // (accessed from main thread and control thread)
    mutable std::mutex controlMutex_;

    // PID-like correction state per device
    struct CorrectionState {
        double integralDrift = 0.0;
        double prevDrift     = 0.0;
    };
    std::array<CorrectionState, kMaxDevices> correctionStates_{};
};

} // namespace msbt
