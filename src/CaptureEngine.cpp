// ============================================================================
// CaptureEngine.cpp -- WASAPI Loopback Capture Implementation
// ============================================================================

#include "CaptureEngine.h"

namespace msbt {

CaptureEngine::CaptureEngine(RingBuffer& ring, PacketCallback onPacket)
    : ring_(ring)
    , onPacket_(std::move(onPacket))
{
    captureEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!captureEvent_) {
        throw std::runtime_error("CreateEvent failed for capture");
    }
}

CaptureEngine::~CaptureEngine() {
    Stop();
    if (captureEvent_) {
        CloseHandle(captureEvent_);
        captureEvent_ = nullptr;
    }
}

void CaptureEngine::Start() {
    if (running_.load(std::memory_order_acquire)) return;
    stopRequested_.store(false, std::memory_order_release);
    thread_ = std::thread(&CaptureEngine::CaptureThread, this);
}

void CaptureEngine::Stop() noexcept {
    stopRequested_.store(true, std::memory_order_release);
    // Signal the event to wake the capture loop
    if (captureEvent_) {
        SetEvent(captureEvent_);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

void CaptureEngine::InitializeLoopback() {
    // CoInitializeEx for this thread
    HrCheck(CoInitializeEx(nullptr, COINIT_MULTITHREADED),
            "CoInitializeEx (capture)");

    // Get default render endpoint (we loopback from it)
    HrCheck(CoCreateInstance(
                __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                IID_PPV_ARGS(&enumerator_)),
            "CoCreateInstance MMDeviceEnumerator");

    HrCheck(enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_),
            "GetDefaultAudioEndpoint");

    // Activate audio client
    HrCheck(device_->Activate(
                __uuidof(IAudioClient), CLSCTX_ALL,
                nullptr, reinterpret_cast<void**>(audioClient_.GetAddressOf())),
            "Activate IAudioClient");

    WAVEFORMATEX wfx = MakeCaptureFormat();

    // Negotiate format: use AUTOCONVERTPCM if device doesn't support our preferred format
    DWORD streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

    WAVEFORMATEX* closestMatch = nullptr;
    HRESULT fmtHr = audioClient_->IsFormatSupported(
        AUDCLNT_SHAREMODE_SHARED, &wfx, &closestMatch);
    if (closestMatch) {
        CoTaskMemFree(closestMatch);
        closestMatch = nullptr;
    }
    if (fmtHr != S_OK) {
        // Device mix format differs from our preferred format;
        // request Windows to auto-convert for us
        streamFlags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                     | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    }

    // Initialize in shared-mode loopback, event-driven
    HrCheck(audioClient_->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                streamFlags,
                kCapturePeriod,
                0,
                &wfx,
                nullptr),
            "IAudioClient::Initialize (loopback)");

    // Set event handle
    HrCheck(audioClient_->SetEventHandle(captureEvent_),
            "IAudioClient::SetEventHandle");

    // Get capture client
    HrCheck(audioClient_->GetService(
                IID_PPV_ARGS(&captureClient_)),
            "GetService IAudioCaptureClient");
}

void CaptureEngine::RunCaptureLoopProtected() {
    // All locals are POD — safe for __try/__except (no C++ destructors)
    bool epochSet = false;

    __try {
        while (!stopRequested_.load(std::memory_order_acquire)) {
            DWORD waitResult = WaitForSingleObject(captureEvent_, 50);
            if (stopRequested_.load(std::memory_order_acquire)) break;
            if (waitResult == WAIT_TIMEOUT) continue;

            UINT32 packetLength = 0;
            HRESULT hr = captureClient_->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) {
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                    OutputDebugStringA("[CaptureEngine] Device invalidated, exiting\n");
                    deviceLost_.store(true, std::memory_order_release);
                }
                break;
            }

            while (packetLength > 0) {
                BYTE*  pData  = nullptr;
                UINT32 frames = 0;
                DWORD  flags  = 0;
                UINT64 devPos = 0;
                UINT64 qpcPos = 0;

                hr = captureClient_->GetBuffer(&pData, &frames, &flags, &devPos, &qpcPos);
                if (FAILED(hr)) {
                    if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                        OutputDebugStringA("[CaptureEngine] Device invalidated, exiting\n");
                        deviceLost_.store(true, std::memory_order_release);
                    }
                    break;
                }

                int64_t qpcStamp;
                if (qpcPos != 0) {
                    qpcStamp = static_cast<int64_t>(qpcPos);
                } else {
                    qpcStamp = QpcNow();
                }

                if (!epochSet) {
                    masterEpoch_.store(qpcStamp, std::memory_order_release);
                    epochSet = true;
                }

                if (frames > 0 && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    uint32_t byteCount = frames * kFrameSize;
                    uint32_t ringOffset = ring_.Write(pData, byteCount);

                    if (onPacket_) {
                        onPacket_(static_cast<uint64_t>(qpcStamp), frames, ringOffset);
                    }

                    totalFrames_.fetch_add(frames, std::memory_order_relaxed);
                } else if (frames > 0 && (flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    alignas(16) uint8_t silence[4096]{};
                    uint32_t byteCount = frames * kFrameSize;
                    uint32_t remaining = byteCount;
                    while (remaining > 0) {
                        uint32_t chunk = std::min(remaining, static_cast<uint32_t>(sizeof(silence)));
                        ring_.Write(silence, chunk);
                        remaining -= chunk;
                    }
                    totalFrames_.fetch_add(frames, std::memory_order_relaxed);
                }

                captureClient_->ReleaseBuffer(frames);

                hr = captureClient_->GetNextPacketSize(&packetLength);
                if (FAILED(hr)) {
                    if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                        OutputDebugStringA("[CaptureEngine] Device invalidated, exiting\n");
                        deviceLost_.store(true, std::memory_order_release);
                    }
                    break;
                }
            }

            if (deviceLost_.load(std::memory_order_acquire)) break;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "[CaptureEngine] SEH exception 0x%08lX, stopping\n",
                 GetExceptionCode());
        OutputDebugStringA(msg);
    }
}

void CaptureEngine::CaptureThread() {
    MmcssGuard mmcss(L"Pro Audio");
    SetThreadIdealProcessor(GetCurrentThread(), 0);

    try {
        InitializeLoopback();
    } catch (const std::runtime_error&) {
        running_.store(false, std::memory_order_release);
        CoUninitialize();
        return;
    }

    HRESULT hr = audioClient_->Start();
    if (FAILED(hr)) {
        OutputDebugStringA("[CaptureEngine] IAudioClient::Start failed\n");
        running_.store(false, std::memory_order_release);
        CoUninitialize();
        return;
    }

    running_.store(true, std::memory_order_release);

    RunCaptureLoopProtected();

    // Cleanup
    audioClient_->Stop();
    running_.store(false, std::memory_order_release);
    CoUninitialize();
}

} // namespace msbt
