// ============================================================================
// SyncController.cpp -- Master Clock Orchestration & Drift Compensation
// ============================================================================

#include "SyncController.h"
#include <cmath>
#include <algorithm>

namespace msbt {

// PI controller tuning rationale:
//   kPropGain: At max drift (5000us), proportional term = 0.000002 * 5000 = 0.01
//     which matches the output clamp, providing smooth linear scaling.
//   kIntegralGain: Very slow integral to eliminate steady-state error
//     without causing oscillation. With 100ms interval, integral settles in ~10s.
static constexpr double kPropGain     = 0.000002;
static constexpr double kIntegralGain = 0.0000001;
static constexpr double kControlIntervalMs = 100.0; // ~10 Hz

SyncController::SyncController() {
    engines_.fill(nullptr);
}

SyncController::~SyncController() {
    Stop();
}

DeviceSyncState& SyncController::RegisterDevice(uint32_t index) {
    if (index >= kMaxDevices) {
        throw std::runtime_error("Device index out of range");
    }
    return deviceStates_[index];
}

void SyncController::BindRenderEngine(uint32_t index, RenderEngine* engine) {
    if (index >= kMaxDevices) return;
    std::lock_guard<std::mutex> lock(controlMutex_);
    engines_[index] = engine;
}

void SyncController::SetMasterEpoch(int64_t qpc) noexcept {
    masterEpochQpc_.store(qpc, std::memory_order_release);
}

void SyncController::SetReportCallback(DriftReportCallback cb) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    reportCallback_ = std::move(cb);
}

void SyncController::Start() {
    if (running_.load(std::memory_order_acquire)) return;
    stopRequested_.store(false, std::memory_order_release);
    thread_ = std::thread(&SyncController::ControlThread, this);
}

void SyncController::Stop() noexcept {
    stopRequested_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
}

void SyncController::ComputeInitialAlignment() {
    std::lock_guard<std::mutex> lock(controlMutex_);

    // Find max stream latency across active devices
    uint32_t maxLatencyFrames = 0;

    for (uint32_t i = 0; i < kMaxDevices; ++i) {
        if (!deviceStates_[i].active.load(std::memory_order_acquire)) continue;
        if (!engines_[i]) continue;

        uint32_t lat = deviceStates_[i].streamLatencyFrames.load(std::memory_order_acquire);
        if (lat > maxLatencyFrames) {
            maxLatencyFrames = lat;
        }
    }

    // Fallback if no latency data
    if (maxLatencyFrames == 0) {
        maxLatencyFrames = 480;  // 10ms default
    }

    // Set delay for each device: slowest device gets 0 delay, faster devices get more
    for (uint32_t i = 0; i < kMaxDevices; ++i) {
        if (!engines_[i]) continue;
        if (!deviceStates_[i].active.load(std::memory_order_acquire)) continue;

        uint32_t lat = deviceStates_[i].streamLatencyFrames.load(std::memory_order_acquire);
        uint32_t delay = (maxLatencyFrames > lat) ? (maxLatencyFrames - lat) : 0;
        delay = std::min(delay, kDelayLineMaxFrames - 1);
        engines_[i]->SetDelayFrames(delay);
    }
}

void SyncController::EvaluateAndCorrect() {
    DeviceDriftReport reports[kMaxDevices]{};
    uint32_t reportCount = 0;
    DriftReportCallback callbackCopy;

    {
        std::lock_guard<std::mutex> lock(controlMutex_);

        for (uint32_t i = 0; i < kMaxDevices; ++i) {
            auto& state = deviceStates_[i];
            if (!state.active.load(std::memory_order_acquire)) continue;
            if (!engines_[i]) continue;

            double drift = state.filteredDriftUs.load(std::memory_order_acquire);
            auto& cs = correctionStates_[i];

            // Build telemetry report
            auto& report = reports[reportCount];
            report.deviceIndex    = i;
            report.active         = true;
            report.rawDriftUs     = state.phaseDriftUs.load(std::memory_order_acquire);
            report.filteredDriftUs = drift;
            report.delayFrames    = state.delayFrames.load(std::memory_order_acquire);
            report.resampleRatio  = state.resampleRatio.load(std::memory_order_acquire);
            report.framesRendered = state.framesRendered.load(std::memory_order_acquire);
            report.underrunCount  = state.underrunCount.load(std::memory_order_acquire);
            report.overrunCount   = state.overrunCount.load(std::memory_order_acquire);
            reportCount++;

            // Skip correction if drift is below threshold
            if (std::abs(drift) < kDriftThresholdUs) {
                cs.integralDrift = 0.0;
                continue;
            }

            // If drift exceeds max correctable range, log and skip
            if (std::abs(drift) > kMaxCompensationUs) {
                char msg[128];
                snprintf(msg, sizeof(msg),
                         "[SyncCtrl] Device %u: drift %.1f us exceeds max, skipping\n",
                         i, drift);
                OutputDebugStringA(msg);
                continue;
            }

            // PI controller for resampling ratio
            cs.integralDrift += drift * (kControlIntervalMs / 1000.0);
            cs.integralDrift = std::clamp(cs.integralDrift, -50000.0, 50000.0);

            double correction = kPropGain * drift + kIntegralGain * cs.integralDrift;

            double newRatio = 1.0 + correction;
            newRatio = std::clamp(newRatio, 0.99, 1.01);

            engines_[i]->SetResampleRatio(newRatio);

            // Anti-windup: if output is saturated, stop integral accumulation
            if (std::abs(newRatio - 1.0) >= 0.0099) {
                cs.integralDrift *= 0.5;  // decay integral toward zero
            }

            // For large drift, also adjust the delay line
            if (std::abs(drift) > 2000.0) {
                int32_t driftFrames = static_cast<int32_t>(
                    (drift / 1'000'000.0) * kSampleRate);

                uint32_t currentDelay = state.delayFrames.load(std::memory_order_acquire);
                int32_t newDelay = static_cast<int32_t>(currentDelay) - driftFrames;
                newDelay = std::clamp(newDelay, 0,
                                      static_cast<int32_t>(kDelayLineMaxFrames - 1));
                engines_[i]->SetDelayFrames(static_cast<uint32_t>(newDelay));
            }

            cs.prevDrift = drift;
        }

        callbackCopy = reportCallback_;
    }

    // Fire telemetry callback outside the lock to avoid deadlock
    if (callbackCopy && reportCount > 0) {
        callbackCopy(reports, reportCount);
    }
}

void SyncController::ControlThread() {
    // Control thread runs at lower priority -- no MMCSS needed
    running_.store(true, std::memory_order_release);

    while (!stopRequested_.load(std::memory_order_acquire)) {
        Sleep(static_cast<DWORD>(kControlIntervalMs));

        if (stopRequested_.load(std::memory_order_acquire)) break;

        EvaluateAndCorrect();
    }

    running_.store(false, std::memory_order_release);
}

} // namespace msbt
