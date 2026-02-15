#pragma once
// ============================================================================
// RenderEngine -- Per-Device Audio Consumer with Drift Compensation
//
// Each RenderEngine instance owns a thread that:
//   1. Reads PCM from the shared RingBuffer via its own ConsumerCursor
//   2. Applies a virtual delay line for initial phase alignment
//   3. Feeds data to a WASAPI render endpoint (Bluetooth adapter)
//   4. Measures phase drift via IAudioClient::GetPosition vs master QPC
//   5. Reports drift metrics back to the SyncController
// ============================================================================

#include "Common.h"
#include "RingBuffer.h"
#include <thread>
#include <string>

namespace msbt {

// Lightweight IAudioSessionEvents implementation for disconnect detection
class SessionEventListener : public IAudioSessionEvents {
public:
    explicit SessionEventListener(std::atomic<bool>& disconnectFlag)
        : disconnectFlag_(disconnectFlag) {}

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refCount_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = InterlockedDecrement(&refCount_);
        if (count == 0) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioSessionEvents)) {
            *ppv = static_cast<IAudioSessionEvents*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    // IAudioSessionEvents — only OnSessionDisconnected is interesting
    HRESULT STDMETHODCALLTYPE OnDisplayNameChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnIconPathChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSimpleVolumeChanged(float, BOOL, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnChannelVolumeChanged(DWORD, float[], DWORD, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnGroupingParamChanged(LPCGUID, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnStateChanged(AudioSessionState) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE OnSessionDisconnected(AudioSessionDisconnectReason reason) override {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "[RenderEngine] Session disconnected, reason=%d\n",
                 static_cast<int>(reason));
        OutputDebugStringA(msg);
        disconnectFlag_.store(true, std::memory_order_release);
        return S_OK;
    }

private:
    std::atomic<bool>& disconnectFlag_;
    ULONG refCount_ = 1;
};

class RenderEngine {
public:
    // deviceId: WASAPI endpoint ID string
    // deviceIndex: slot in the SyncController's state array [0..kMaxDevices)
    RenderEngine(uint32_t deviceIndex,
                 const std::wstring& deviceId,
                 RingBuffer& ring,
                 DeviceSyncState& syncState);
    ~RenderEngine();

    RenderEngine(const RenderEngine&) = delete;
    RenderEngine& operator=(const RenderEngine&) = delete;

    void Start(int64_t masterEpochQpc);
    void Stop() noexcept;

    bool IsRunning() const noexcept { return running_.load(std::memory_order_acquire); }
    uint32_t DeviceIndex() const noexcept { return deviceIndex_; }
    const std::wstring& DeviceId() const noexcept { return deviceId_; }

    // Whether device was lost and needs reconnection
    bool IsDeviceLost() const noexcept { return deviceLost_.load(std::memory_order_acquire); }

    // Set initial delay (in frames) for phase alignment
    void SetDelayFrames(uint32_t frames) noexcept;

    // Set resampling ratio for drift compensation (1.0 = no change)
    void SetResampleRatio(double ratio) noexcept;

private:
    void RenderThread();
    void InitializeRenderClient();
    void ReleaseResources();
    bool TryReconnect();
    void MeasureDrift();
    void ApplyDelayLine(const int16_t* src, int16_t* dst, uint32_t frames);
    void ApplyResample(const int16_t* src, uint32_t srcFrames,
                       int16_t* dst, uint32_t& dstFrames);
    void RunRenderLoopProtected();

    uint32_t            deviceIndex_;
    std::wstring        deviceId_;
    RingBuffer&         ring_;
    DeviceSyncState&    syncState_;

    // WASAPI render objects
    ComPtr<IMMDevice>          device_;
    ComPtr<IAudioClient>       audioClient_;
    ComPtr<IAudioRenderClient> renderClient_;
    ComPtr<IAudioClock>        audioClock_;
    ComPtr<IAudioClock2>       audioClock2_;         // hardware-level position (may be null)
    ComPtr<IAudioClockAdjustment> clockAdjust_;      // hardware rate adjust (may be null)
    bool                       useHardwareRateAdjust_ = false;
    HANDLE                     renderEvent_ = nullptr;
    UINT32                     bufferFrames_ = 0;

    // Thread control
    std::atomic<bool>    running_{false};
    std::atomic<bool>    stopRequested_{false};
    std::atomic<bool>    deviceLost_{false};
    std::atomic<bool>    sessionDisconnected_{false};
    ComPtr<IAudioSessionControl> sessionControl_;
    SessionEventListener* sessionListener_ = nullptr;
    std::thread          thread_;
    int64_t              masterEpochQpc_ = 0;
    static constexpr uint32_t kMaxReconnectAttempts = 5;

    // Consumer cursor into the ring buffer
    RingBuffer::ConsumerCursor cursor_;

    // Virtual delay line for phase alignment
    static constexpr uint32_t kDelayBufFrames = kDelayLineMaxFrames;
    alignas(64) int16_t delayBuffer_[kDelayBufFrames * kChannels]{};
    uint32_t delayWritePos_ = 0;
    uint32_t delayReadPos_  = 0;
    std::atomic<uint32_t> targetDelayFrames_{0};
    uint32_t currentDelayFrames_ = 0;

    // Adaptive resampling state
    std::atomic<double> targetResampleRatio_{1.0};
    double currentResampleRatio_ = 1.0;
    double resampleAccum_ = 0.0;  // fractional accumulator for resampling

    // History samples for Hermite cubic interpolation (last 2 from previous call)
    int16_t resampleHistory_[2 * kChannels]{};
    bool resampleHistoryValid_ = false;

    // Per-device timing
    uint64_t deviceStartQpc_ = 0;
    uint64_t deviceFrequency_ = 0;
};

} // namespace msbt
