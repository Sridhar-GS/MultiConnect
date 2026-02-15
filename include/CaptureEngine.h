#pragma once
// ============================================================================
// CaptureEngine -- WASAPI Loopback Capture (Producer)
//
// Captures system audio via AUDCLNT_STREAMFLAGS_LOOPBACK on the default
// render endpoint.  Runs an event-driven capture loop on a dedicated thread
// with MMCSS "Pro Audio" priority.  Each captured packet is QPC-timestamped
// and written into the shared RingBuffer.
// ============================================================================

#include "Common.h"
#include "RingBuffer.h"
#include <thread>
#include <functional>

namespace msbt {

// Callback signature for packet notifications.
// (qpcTimestamp, frameCount, ringByteOffset)
using PacketCallback = std::function<void(uint64_t, uint32_t, uint32_t)>;

class CaptureEngine {
public:
    CaptureEngine(RingBuffer& ring, PacketCallback onPacket);
    ~CaptureEngine();

    CaptureEngine(const CaptureEngine&) = delete;
    CaptureEngine& operator=(const CaptureEngine&) = delete;

    void Start();
    void Stop() noexcept;

    bool IsRunning() const noexcept { return running_.load(std::memory_order_acquire); }

    // Master clock reference: QPC tick when the very first packet was captured
    int64_t MasterEpochQpc() const noexcept { return masterEpoch_.load(std::memory_order_acquire); }

    // Total frames captured since start
    uint64_t TotalFrames() const noexcept { return totalFrames_.load(std::memory_order_acquire); }

    // Whether the audio device was lost (invalidated)
    bool IsDeviceLost() const noexcept { return deviceLost_.load(std::memory_order_acquire); }

private:
    void CaptureThread();
    void InitializeLoopback();
    void RunCaptureLoopProtected();

    RingBuffer&          ring_;
    PacketCallback       onPacket_;

    // COM / WASAPI objects (initialized on capture thread)
    ComPtr<IMMDeviceEnumerator> enumerator_;
    ComPtr<IMMDevice>           device_;
    ComPtr<IAudioClient>        audioClient_;
    ComPtr<IAudioCaptureClient> captureClient_;
    HANDLE                      captureEvent_ = nullptr;

    // Thread control
    std::atomic<bool>    running_{false};
    std::atomic<bool>    stopRequested_{false};
    std::thread          thread_;

    // Timing
    std::atomic<int64_t> masterEpoch_{0};
    std::atomic<uint64_t> totalFrames_{0};

    // Device state
    std::atomic<bool>    deviceLost_{false};
};

} // namespace msbt
