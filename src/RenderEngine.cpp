// ============================================================================
// RenderEngine.cpp -- Per-Device Audio Render with Drift Compensation
// ============================================================================

#include "RenderEngine.h"
#include <cmath>

namespace msbt {

RenderEngine::RenderEngine(uint32_t deviceIndex,
                           const std::wstring& deviceId,
                           RingBuffer& ring,
                           DeviceSyncState& syncState)
    : deviceIndex_(deviceIndex)
    , deviceId_(deviceId)
    , ring_(ring)
    , syncState_(syncState)
{
    renderEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!renderEvent_) {
        throw std::runtime_error("CreateEvent failed for render");
    }
}

RenderEngine::~RenderEngine() {
    Stop();
    if (renderEvent_) {
        CloseHandle(renderEvent_);
        renderEvent_ = nullptr;
    }
}

void RenderEngine::Start(int64_t masterEpochQpc) {
    if (running_.load(std::memory_order_acquire)) return;
    masterEpochQpc_ = masterEpochQpc;
    stopRequested_.store(false, std::memory_order_release);
    thread_ = std::thread(&RenderEngine::RenderThread, this);
}

void RenderEngine::Stop() noexcept {
    stopRequested_.store(true, std::memory_order_release);
    if (renderEvent_) {
        SetEvent(renderEvent_);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    syncState_.active.store(false, std::memory_order_release);
}

void RenderEngine::SetDelayFrames(uint32_t frames) noexcept {
    targetDelayFrames_.store(std::min(frames, kDelayBufFrames - 1),
                            std::memory_order_release);
}

void RenderEngine::SetResampleRatio(double ratio) noexcept {
    // Clamp to sane range: +/- 1%
    ratio = std::clamp(ratio, 0.99, 1.01);

    if (useHardwareRateAdjust_ && clockAdjust_) {
        // Use hardware rate adjustment - convert ratio to target sample rate
        float targetRate = static_cast<float>(kSampleRate * ratio);
        clockAdjust_->SetSampleRate(targetRate);
    }

    targetResampleRatio_.store(ratio, std::memory_order_release);
}

void RenderEngine::InitializeRenderClient() {
    HrCheck(CoInitializeEx(nullptr, COINIT_MULTITHREADED),
            "CoInitializeEx (render)");

    ComPtr<IMMDeviceEnumerator> enumerator;
    HrCheck(CoCreateInstance(
                __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                IID_PPV_ARGS(&enumerator)),
            "CoCreateInstance MMDeviceEnumerator (render)");

    // Open the specific device by ID
    HrCheck(enumerator->GetDevice(deviceId_.c_str(), &device_),
            "GetDevice by ID");

    HrCheck(device_->Activate(
                __uuidof(IAudioClient), CLSCTX_ALL,
                nullptr, reinterpret_cast<void**>(audioClient_.GetAddressOf())),
            "Activate IAudioClient (render)");

    WAVEFORMATEX wfx = MakeCaptureFormat();

    DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

    WAVEFORMATEX* closestMatch = nullptr;
    HRESULT fmtHr = audioClient_->IsFormatSupported(
        AUDCLNT_SHAREMODE_SHARED, &wfx, &closestMatch);
    if (closestMatch) {
        CoTaskMemFree(closestMatch);
        closestMatch = nullptr;
    }
    if (fmtHr != S_OK) {
        streamFlags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                     | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    }

    // Try with RATEADJUST first for hardware clock correction
    DWORD rateFlags = streamFlags | AUDCLNT_STREAMFLAGS_RATEADJUST;
    HRESULT initHr = audioClient_->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                rateFlags,
                kCapturePeriod * 2,  // 20ms buffer for BT headroom
                0,
                &wfx,
                nullptr);

    if (SUCCEEDED(initHr)) {
        // Try to get the clock adjustment interface
        HRESULT adjHr = audioClient_->GetService(IID_PPV_ARGS(&clockAdjust_));
        if (SUCCEEDED(adjHr)) {
            useHardwareRateAdjust_ = true;
        }
    } else {
        // RATEADJUST not supported; fall back to standard init
        // Need a fresh IAudioClient since Initialize can only be called once
        audioClient_.Reset();
        HrCheck(device_->Activate(
                    __uuidof(IAudioClient), CLSCTX_ALL,
                    nullptr, reinterpret_cast<void**>(audioClient_.GetAddressOf())),
                "Re-activate IAudioClient (render, fallback)");

        HrCheck(audioClient_->Initialize(
                    AUDCLNT_SHAREMODE_SHARED,
                    streamFlags,
                    kCapturePeriod * 2,  // 20ms buffer for BT headroom
                    0,
                    &wfx,
                    nullptr),
                "IAudioClient::Initialize (render)");
    }

    HrCheck(audioClient_->SetEventHandle(renderEvent_),
            "SetEventHandle (render)");

    HrCheck(audioClient_->GetBufferSize(&bufferFrames_),
            "GetBufferSize (render)");

    // Query stream latency for initial alignment
    REFERENCE_TIME latency = 0;
    if (SUCCEEDED(audioClient_->GetStreamLatency(&latency))) {
        uint32_t latFrames = static_cast<uint32_t>(
            (latency * kSampleRate) / 10'000'000);
        syncState_.streamLatencyFrames.store(latFrames, std::memory_order_release);
    }

    HrCheck(audioClient_->GetService(IID_PPV_ARGS(&renderClient_)),
            "GetService IAudioRenderClient");

    HrCheck(audioClient_->GetService(IID_PPV_ARGS(&audioClock_)),
            "GetService IAudioClock");

    // Try IAudioClock2 for more accurate hardware position
    audioClock_->QueryInterface(IID_PPV_ARGS(&audioClock2_));
    // audioClock2_ may be null -- MeasureDrift handles fallback

    // Get device clock frequency for drift measurement
    UINT64 freq = 0;
    HrCheck(audioClock_->GetFrequency(&freq), "IAudioClock::GetFrequency");
    deviceFrequency_ = freq;

    // Register for session disconnect events
    ComPtr<IAudioSessionControl> sessionCtrl;
    if (SUCCEEDED(audioClient_->GetService(IID_PPV_ARGS(&sessionCtrl)))) {
        sessionControl_ = sessionCtrl;
        sessionListener_ = new SessionEventListener(sessionDisconnected_);
        sessionControl_->RegisterAudioSessionNotification(sessionListener_);
    }
}

void RenderEngine::MeasureDrift() {
    if (!audioClock_ || deviceFrequency_ == 0) return;

    UINT64 devPosition = 0;
    UINT64 qpcPosition = 0;
    HRESULT hr;

    if (audioClock2_) {
        // IAudioClock2::GetDevicePosition provides hardware-level accuracy
        hr = audioClock2_->GetDevicePosition(&devPosition, &qpcPosition);
    } else {
        hr = audioClock_->GetPosition(&devPosition, &qpcPosition);
    }
    if (FAILED(hr) || qpcPosition == 0) return;

    // Convert device position to expected QPC time
    // devPosition / deviceFrequency_ = seconds elapsed on the device
    double deviceElapsedSec = static_cast<double>(devPosition) /
                              static_cast<double>(deviceFrequency_);

    // Time since master epoch according to QPC
    int64_t qpcNow = static_cast<int64_t>(qpcPosition);
    double wallElapsedSec = QpcToSeconds(qpcNow - masterEpochQpc_);

    // Expected frames based on wall clock
    double expectedFrames = wallElapsedSec * kSampleRate;

    // Actual frames rendered by device
    double actualFrames = deviceElapsedSec * kSampleRate;

    // Phase drift in frames, then convert to time
    double driftFrames = actualFrames - expectedFrames;
    double driftUs = (driftFrames / kSampleRate) * 1'000'000.0;

    // Store raw drift
    syncState_.phaseDriftUs.store(driftUs, std::memory_order_release);

    // EMA filter
    double filtered = syncState_.filteredDriftUs.load(std::memory_order_relaxed);
    filtered = filtered * (1.0 - kDriftFilterAlpha) + driftUs * kDriftFilterAlpha;
    syncState_.filteredDriftUs.store(filtered, std::memory_order_release);

    // QPC ticks version
    int64_t driftQpc = static_cast<int64_t>(
        (driftFrames / kSampleRate) * QpcFrequency());
    syncState_.phaseDriftQpc.store(driftQpc, std::memory_order_release);
    syncState_.lastPositionQpc.store(static_cast<uint64_t>(qpcNow),
                                     std::memory_order_release);
}

void RenderEngine::ApplyDelayLine(const int16_t* src, int16_t* dst,
                                  uint32_t frames) {
    // Smoothly adjust current delay toward target
    uint32_t target = targetDelayFrames_.load(std::memory_order_acquire);
    if (currentDelayFrames_ < target) {
        currentDelayFrames_ = std::min(currentDelayFrames_ + 1, target);
    } else if (currentDelayFrames_ > target) {
        currentDelayFrames_ = std::max(currentDelayFrames_ - 1, target);
    }

    if (currentDelayFrames_ == 0) {
        // No delay -- pass-through
        std::memcpy(dst, src, frames * kFrameSize);
        return;
    }

    // Write into circular delay buffer, read from delayed position
    for (uint32_t i = 0; i < frames; ++i) {
        // Write current frame
        delayBuffer_[delayWritePos_ * kChannels]     = src[i * kChannels];
        delayBuffer_[delayWritePos_ * kChannels + 1] = src[i * kChannels + 1];

        // Read from delayed position
        uint32_t readPos = (delayWritePos_ + kDelayBufFrames - currentDelayFrames_)
                           % kDelayBufFrames;
        dst[i * kChannels]     = delayBuffer_[readPos * kChannels];
        dst[i * kChannels + 1] = delayBuffer_[readPos * kChannels + 1];

        delayWritePos_ = (delayWritePos_ + 1) % kDelayBufFrames;
    }

    syncState_.delayFrames.store(currentDelayFrames_, std::memory_order_release);
}

void RenderEngine::ApplyResample(const int16_t* src, uint32_t srcFrames,
                                 int16_t* dst, uint32_t& dstFrames) {
    double ratio = targetResampleRatio_.load(std::memory_order_acquire);
    currentResampleRatio_ += (ratio - currentResampleRatio_) * 0.1;
    syncState_.resampleRatio.store(currentResampleRatio_, std::memory_order_release);

    if (std::abs(currentResampleRatio_ - 1.0) < 1e-6) {
        std::memcpy(dst, src, srcFrames * kFrameSize);
        dstFrames = srcFrames;
        // Save last 2 frames for history
        if (srcFrames >= 2) {
            std::memcpy(resampleHistory_, src + (srcFrames - 2) * kChannels,
                        2 * kChannels * sizeof(int16_t));
            resampleHistoryValid_ = true;
        }
        return;
    }

    uint32_t maxOut = static_cast<uint32_t>(
        static_cast<double>(srcFrames) / currentResampleRatio_) + 2;
    dstFrames = 0;
    double srcPos = resampleAccum_;

    while (srcPos < static_cast<double>(srcFrames) - 1.0 &&
           dstFrames < maxOut) {
        int32_t idx = static_cast<int32_t>(srcPos);
        double frac = srcPos - static_cast<double>(idx);

        for (uint32_t ch = 0; ch < kChannels; ++ch) {
            // 4-point Hermite: need samples at idx-1, idx, idx+1, idx+2
            double s0, s1, s2, s3;

            if (idx >= 1) {
                s0 = static_cast<double>(src[(idx - 1) * kChannels + ch]);
            } else if (resampleHistoryValid_) {
                s0 = static_cast<double>(resampleHistory_[(1 + (idx - 1)) * kChannels + ch]);
            } else {
                s0 = static_cast<double>(src[ch]);
            }

            s1 = static_cast<double>(src[idx * kChannels + ch]);

            if (idx + 1 < static_cast<int32_t>(srcFrames)) {
                s2 = static_cast<double>(src[(idx + 1) * kChannels + ch]);
            } else {
                s2 = s1;
            }

            if (idx + 2 < static_cast<int32_t>(srcFrames)) {
                s3 = static_cast<double>(src[(idx + 2) * kChannels + ch]);
            } else {
                s3 = s2;
            }

            // Hermite interpolation
            double c0 = s1;
            double c1 = 0.5 * (s2 - s0);
            double c2 = s0 - 2.5 * s1 + 2.0 * s2 - 0.5 * s3;
            double c3 = 0.5 * (s3 - s0) + 1.5 * (s1 - s2);
            double result = ((c3 * frac + c2) * frac + c1) * frac + c0;

            dst[dstFrames * kChannels + ch] =
                static_cast<int16_t>(std::clamp(result, -32768.0, 32767.0));
        }
        dstFrames++;
        srcPos += currentResampleRatio_;
    }

    // Save last 2 frames for next call's history
    if (srcFrames >= 2) {
        std::memcpy(resampleHistory_, src + (srcFrames - 2) * kChannels,
                    2 * kChannels * sizeof(int16_t));
        resampleHistoryValid_ = true;
    }

    resampleAccum_ = srcPos - static_cast<double>(srcFrames);
    if (resampleAccum_ < 0.0) resampleAccum_ = 0.0;
}

void RenderEngine::RunRenderLoopProtected() {
    // All locals are POD — safe for __try/__except (no C++ destructors)
    constexpr uint32_t kMaxFramesPerPeriod = 2048;
    constexpr uint32_t kMaxResampledFrames = static_cast<uint32_t>(
        kMaxFramesPerPeriod / 0.99) + 2;
    alignas(64) int16_t readBuf[kMaxFramesPerPeriod * kChannels]{};
    alignas(64) int16_t delayBuf[kMaxFramesPerPeriod * kChannels]{};
    alignas(64) int16_t resampleBuf[kMaxResampledFrames * kChannels]{};

    uint64_t driftMeasureCounter = 0;
    uint32_t consecutiveErrors = 0;

    __try {
        while (!stopRequested_.load(std::memory_order_acquire)) {
            DWORD waitResult = WaitForSingleObject(renderEvent_, 50);
            if (stopRequested_.load(std::memory_order_acquire)) break;
            if (waitResult == WAIT_TIMEOUT) continue;

            // Check for session disconnection
            if (sessionDisconnected_.load(std::memory_order_acquire)) {
                OutputDebugStringA("[RenderEngine] Session disconnected, exiting loop\n");
                break;
            }

            UINT32 padding = 0;
            HRESULT hr = audioClient_->GetCurrentPadding(&padding);
            if (FAILED(hr)) {
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                    OutputDebugStringA("[RenderEngine] Device invalidated, exiting\n");
                    break;
                }
                if (++consecutiveErrors >= 3) break;
                continue;
            }

            UINT32 available = bufferFrames_ - padding;
            if (available == 0) continue;
            available = std::min(available, kMaxFramesPerPeriod);

            uint32_t bytesNeeded = available * kFrameSize;
            uint32_t bytesRead = ring_.Read(cursor_, readBuf, bytesNeeded);
            uint32_t framesRead = bytesRead / kFrameSize;

            if (framesRead == 0) {
                BYTE* pData = nullptr;
                hr = renderClient_->GetBuffer(available, &pData);
                if (FAILED(hr)) {
                    if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                        OutputDebugStringA("[RenderEngine] Device invalidated, exiting\n");
                        break;
                    }
                    if (++consecutiveErrors >= 3) break;
                    continue;
                }
                std::memset(pData, 0, available * kFrameSize);
                renderClient_->ReleaseBuffer(available, AUDCLNT_BUFFERFLAGS_SILENT);
                syncState_.underrunCount.fetch_add(1, std::memory_order_relaxed);
                consecutiveErrors = 0;
                continue;
            }

            ApplyDelayLine(readBuf, delayBuf, framesRead);

            uint32_t resampledFrames = 0;
            ApplyResample(delayBuf, framesRead, resampleBuf, resampledFrames);

            uint32_t framesToWrite = std::min(resampledFrames, available);

            BYTE* pData = nullptr;
            hr = renderClient_->GetBuffer(framesToWrite, &pData);
            if (FAILED(hr)) {
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                    OutputDebugStringA("[RenderEngine] Device invalidated, exiting\n");
                    break;
                }
                if (++consecutiveErrors >= 3) break;
                continue;
            }
            std::memcpy(pData, resampleBuf, framesToWrite * kFrameSize);
            renderClient_->ReleaseBuffer(framesToWrite, 0);
            consecutiveErrors = 0;

            syncState_.framesRendered.fetch_add(framesToWrite,
                                                std::memory_order_relaxed);

            if (++driftMeasureCounter % 10 == 0) {
                MeasureDrift();
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "[RenderEngine] Device %u: SEH exception 0x%08lX, stopping\n",
                 deviceIndex_, GetExceptionCode());
        OutputDebugStringA(msg);
    }
}

void RenderEngine::ReleaseResources() {
    if (sessionControl_ && sessionListener_) {
        sessionControl_->UnregisterAudioSessionNotification(sessionListener_);
        sessionListener_->Release();
        sessionListener_ = nullptr;
    }
    sessionControl_.Reset();

    if (audioClient_) {
        audioClient_->Stop();
    }
    clockAdjust_.Reset();
    audioClock2_.Reset();
    audioClock_.Reset();
    renderClient_.Reset();
    audioClient_.Reset();
    device_.Reset();
}

bool RenderEngine::TryReconnect() {
    ReleaseResources();

    try {
        InitializeRenderClient();
    } catch (const std::runtime_error&) {
        return false;
    }

    // Reset consumer cursor
    ring_.ResetConsumer(cursor_);

    // Pre-fill with silence
    BYTE* pData = nullptr;
    HRESULT hr = renderClient_->GetBuffer(bufferFrames_, &pData);
    if (SUCCEEDED(hr)) {
        std::memset(pData, 0, bufferFrames_ * kFrameSize);
        renderClient_->ReleaseBuffer(bufferFrames_, 0);
    }

    // Restart
    hr = audioClient_->Start();
    if (FAILED(hr)) return false;

    // Reset state
    sessionDisconnected_.store(false, std::memory_order_release);
    currentDelayFrames_ = 0;
    currentResampleRatio_ = 1.0;
    resampleAccum_ = 0.0;
    resampleHistoryValid_ = false;
    deviceStartQpc_ = static_cast<uint64_t>(QpcNow());

    return true;
}

void RenderEngine::RenderThread() {
    MmcssGuard mmcss(L"Pro Audio");

    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    DWORD idealCore = (deviceIndex_ + 1) % sysInfo.dwNumberOfProcessors;
    SetThreadIdealProcessor(GetCurrentThread(), idealCore);

    try {
        InitializeRenderClient();
    } catch (const std::runtime_error&) {
        running_.store(false, std::memory_order_release);
        CoUninitialize();
        return;
    }

    ring_.ResetConsumer(cursor_);

    {
        BYTE* pData = nullptr;
        HRESULT hr = renderClient_->GetBuffer(bufferFrames_, &pData);
        if (SUCCEEDED(hr)) {
            std::memset(pData, 0, bufferFrames_ * kFrameSize);
            renderClient_->ReleaseBuffer(bufferFrames_, 0);
        }
    }

    HRESULT hr = audioClient_->Start();
    if (FAILED(hr)) {
        OutputDebugStringA("[RenderEngine] IAudioClient::Start failed\n");
        running_.store(false, std::memory_order_release);
        CoUninitialize();
        return;
    }

    running_.store(true, std::memory_order_release);
    syncState_.active.store(true, std::memory_order_release);
    deviceStartQpc_ = static_cast<uint64_t>(QpcNow());

    RunRenderLoopProtected();

    // Check if we should attempt reconnection
    bool reconnected = false;
    if (!stopRequested_.load(std::memory_order_acquire)) {
        // Device was lost (not a clean stop) — attempt reconnection
        deviceLost_.store(true, std::memory_order_release);

        uint32_t backoffMs = 3000;  // Start at 3 seconds
        for (uint32_t attempt = 0; attempt < kMaxReconnectAttempts; ++attempt) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "[RenderEngine] Device %u: reconnect attempt %u/%u in %ums\n",
                     deviceIndex_, attempt + 1, kMaxReconnectAttempts, backoffMs);
            OutputDebugStringA(msg);

            Sleep(backoffMs);

            if (stopRequested_.load(std::memory_order_acquire)) break;

            if (TryReconnect()) {
                snprintf(msg, sizeof(msg),
                         "[RenderEngine] Device %u: reconnected successfully\n",
                         deviceIndex_);
                OutputDebugStringA(msg);
                deviceLost_.store(false, std::memory_order_release);
                syncState_.active.store(true, std::memory_order_release);

                // Re-enter the render loop
                RunRenderLoopProtected();
                reconnected = true;
                break;
            }

            backoffMs = std::min(backoffMs * 2, 12000u);  // Cap at 12 seconds
        }

        if (!reconnected) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "[RenderEngine] Device %u: all reconnect attempts failed\n",
                     deviceIndex_);
            OutputDebugStringA(msg);
        }
    }

    // Final cleanup
    ReleaseResources();
    running_.store(false, std::memory_order_release);
    syncState_.active.store(false, std::memory_order_release);
    CoUninitialize();
}

} // namespace msbt
