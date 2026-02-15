#pragma once
// ============================================================================
// MultiConnect Bluetooth Audio Engine
// Common types and constants
// ============================================================================

#include <Windows.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <Audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <wrl/client.h>

#include <cstdint>
#include <atomic>
#include <array>
#include <string>
#include <stdexcept>
#include <cassert>

namespace msbt {

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Audio format constants
// ---------------------------------------------------------------------------
inline constexpr uint32_t kSampleRate      = 48000;
inline constexpr uint16_t kBitsPerSample   = 16;
inline constexpr uint16_t kChannels        = 2;
inline constexpr uint16_t kBlockAlign      = kChannels * (kBitsPerSample / 8);
inline constexpr uint32_t kBytesPerSec     = kSampleRate * kBlockAlign;
inline constexpr uint32_t kFrameSize       = kBlockAlign;  // bytes per frame

// Engine limits
inline constexpr size_t   kMaxDevices      = 10;

// Buffer sizing (power of two for masking)
// ~170ms at 48kHz stereo 16-bit = 32768 frames
inline constexpr uint32_t kRingFrames      = 1u << 15;  // 32768 frames
inline constexpr uint32_t kRingBytes       = kRingFrames * kFrameSize;

// Capture buffer: 10ms in WASAPI reference-time units (100ns ticks)
inline constexpr REFERENCE_TIME kCapturePeriod = 100000;  // 10ms

// Drift compensation
inline constexpr double   kDriftThresholdUs      = 500.0;    // action threshold
inline constexpr double   kMaxCompensationUs     = 5000.0;   // max correctable drift
inline constexpr uint32_t kDelayLineMaxFrames    = 2400;     // 50ms at 48kHz
inline constexpr double   kDriftFilterAlpha      = 0.02;     // EMA smoothing

// ---------------------------------------------------------------------------
// QPC-stamped audio packet descriptor
// ---------------------------------------------------------------------------
struct AudioPacket {
    uint64_t qpcTimestamp;     // QPC tick at capture
    uint32_t frameCount;       // frames in this packet
    uint32_t ringOffset;       // byte offset into ring buffer where data starts
};

// ---------------------------------------------------------------------------
// Per-device drift / sync metrics (cache-line sized)
// ---------------------------------------------------------------------------
struct alignas(64) DeviceSyncState {
    std::atomic<int64_t>  phaseDriftQpc{0};       // measured phase drift in QPC ticks
    std::atomic<double>   phaseDriftUs{0.0};       // drift in microseconds
    std::atomic<double>   filteredDriftUs{0.0};    // EMA-filtered drift
    std::atomic<uint32_t> delayFrames{0};          // applied delay-line depth
    std::atomic<double>   resampleRatio{1.0};      // adaptive resampling ratio
    std::atomic<uint64_t> framesRendered{0};       // total frames pushed to device
    std::atomic<uint64_t> lastPositionQpc{0};      // last GetPosition QPC stamp
    std::atomic<bool>     active{false};
    std::atomic<uint32_t> streamLatencyFrames{0};  // from IAudioClient::GetStreamLatency
    std::atomic<uint64_t> underrunCount{0};         // render buffer underruns
    std::atomic<uint64_t> overrunCount{0};           // ring buffer overruns detected
};

// ---------------------------------------------------------------------------
// HRESULT helper -- throws on failure in debug, returns code in release
// ---------------------------------------------------------------------------
inline void HrCheck(HRESULT hr, const char* context) {
    if (FAILED(hr)) {
        // In production, log + propagate; here we use structured error path
        char msg[256];
        snprintf(msg, sizeof(msg), "[FATAL] %s failed: 0x%08lX", context, hr);
        OutputDebugStringA(msg);
        OutputDebugStringA("\n");
        // Throw to unwind RAII stack
        throw std::runtime_error(msg);
    }
}

// ---------------------------------------------------------------------------
// QPC utilities
// ---------------------------------------------------------------------------
inline int64_t QpcNow() noexcept {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

inline int64_t QpcFrequency() noexcept {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return freq;
}

inline double QpcToMicroseconds(int64_t ticks) noexcept {
    return static_cast<double>(ticks) * 1'000'000.0 / static_cast<double>(QpcFrequency());
}

inline double QpcToSeconds(int64_t ticks) noexcept {
    return static_cast<double>(ticks) / static_cast<double>(QpcFrequency());
}

// ---------------------------------------------------------------------------
// WASAPI format helper
// ---------------------------------------------------------------------------
inline WAVEFORMATEX MakeCaptureFormat() noexcept {
    WAVEFORMATEX wfx{};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels        = kChannels;
    wfx.nSamplesPerSec   = kSampleRate;
    wfx.wBitsPerSample   = kBitsPerSample;
    wfx.nBlockAlign      = kBlockAlign;
    wfx.nAvgBytesPerSec  = kBytesPerSec;
    wfx.cbSize           = 0;
    return wfx;
}

// ---------------------------------------------------------------------------
// MMCSS RAII wrapper
// ---------------------------------------------------------------------------
class MmcssGuard {
public:
    explicit MmcssGuard(const wchar_t* taskName) {
        handle_ = AvSetMmThreadCharacteristicsW(taskName, &taskIndex_);
        if (!handle_) {
            OutputDebugStringA("[WARN] AvSetMmThreadCharacteristics failed\n");
        }
    }
    ~MmcssGuard() {
        if (handle_) {
            AvRevertMmThreadCharacteristics(handle_);
        }
    }
    MmcssGuard(const MmcssGuard&) = delete;
    MmcssGuard& operator=(const MmcssGuard&) = delete;

private:
    HANDLE   handle_    = nullptr;
    DWORD    taskIndex_ = 0;
};

} // namespace msbt
