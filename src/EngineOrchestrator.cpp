// ============================================================================
// EngineOrchestrator.cpp -- Background Engine Thread
// ============================================================================

#include "EngineOrchestrator.h"
#include <chrono>

namespace msbt {

EngineOrchestrator::EngineOrchestrator(HWND guiHwnd)
    : guiHwnd_(guiHwnd)
{
    cmdEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

EngineOrchestrator::~EngineOrchestrator() {
    PostCommand(EngineCommand::Shutdown);
    if (thread_.joinable()) {
        thread_.join();
    }
    if (cmdEvent_) {
        CloseHandle(cmdEvent_);
        cmdEvent_ = nullptr;
    }
}

void EngineOrchestrator::Launch() {
    thread_ = std::thread(&EngineOrchestrator::EngineThreadMain, this);
}

void EngineOrchestrator::PostCommand(EngineCommand cmd) {
    {
        std::lock_guard<std::mutex> lock(cmdMutex_);
        cmdQueue_.push_back(CommandMsg{cmd});
    }
    if (cmdEvent_) {
        SetEvent(cmdEvent_);
    }
}

void EngineOrchestrator::ProcessCommands() {
    std::deque<CommandMsg> cmds;
    {
        std::lock_guard<std::mutex> lock(cmdMutex_);
        cmds.swap(cmdQueue_);
    }

    for (const auto& cmd : cmds) {
        switch (cmd.cmd) {
            case EngineCommand::StartStreaming:
                DoStartStreaming();
                break;
            case EngineCommand::StopStreaming:
                DoStopStreaming();
                break;
            case EngineCommand::RefreshDevices:
                DoRefreshDevices();
                break;
            case EngineCommand::Shutdown:
                DoStopStreaming();
                shutdownRequested_.store(true, std::memory_order_release);
                break;
        }
    }
}

void EngineOrchestrator::EngineThreadMain() {
    HrCheck(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx (engine)");

    // Auto-start streaming on launch
    DoStartStreaming();

    // Process commands until shutdown
    while (!shutdownRequested_.load(std::memory_order_acquire)) {
        WaitForSingleObject(cmdEvent_, 500);
        ProcessCommands();
    }

    CoUninitialize();
}

void EngineOrchestrator::DoStartStreaming() {
    if (streaming_.load(std::memory_order_acquire)) return;

    try {
        // Phase 1: Enumerate BT endpoints
        deviceManager_ = std::make_unique<DeviceManager>();
        auto endpoints = deviceManager_->EnumerateBluetoothSinks();

        // Post full device list to GUI (active + inactive)
        {
            auto* payload = new DeviceListPayload();
            payload->devices = endpoints;
            PostMessage(guiHwnd_, WM_ENGINE_DEVICE_LIST, 0,
                        reinterpret_cast<LPARAM>(payload));
        }

        // Filter active
        std::vector<AudioEndpointInfo> active;
        for (auto& ep : endpoints) {
            if (ep.isActive) {
                active.push_back(std::move(ep));
            }
        }

        if (active.empty()) {
            OutputDebugStringA("[Engine] No active Bluetooth audio sinks found\n");
            PostMessage(guiHwnd_, WM_ENGINE_STATE_CHANGE, 0, 0);
            return;
        }

        if (active.size() > kMaxDevices) {
            active.resize(kMaxDevices);
        }

        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            activeEndpoints_ = active;
        }

        // Phase 2: Ring buffer
        ring_ = std::make_unique<RingBuffer>();

        // Phase 3: SyncController + Watchdog
        syncCtrl_ = std::make_unique<SyncController>();
        watchdog_ = std::make_unique<Watchdog>();

        // Phase 4: CaptureEngine
        capture_ = std::make_unique<CaptureEngine>(
            *ring_, [](uint64_t, uint32_t, uint32_t) {});
        capture_->Start();

        for (int retries = 0; retries < 50 && !capture_->IsRunning(); ++retries) {
            Sleep(100);
        }
        if (!capture_->IsRunning()) {
            OutputDebugStringA("[Engine] CaptureEngine failed to start\n");
            PostMessage(guiHwnd_, WM_ENGINE_STATE_CHANGE, 0, 0);
            return;
        }

        syncCtrl_->SetMasterEpoch(capture_->MasterEpochQpc());

        // Phase 5: RenderEngines
        renderers_.clear();
        renderers_.reserve(active.size());

        for (uint32_t i = 0; i < static_cast<uint32_t>(active.size()); ++i) {
            auto& syncState = syncCtrl_->RegisterDevice(i);
            auto renderer = std::make_unique<RenderEngine>(
                i, active[i].deviceId, *ring_, syncState);

            syncCtrl_->BindRenderEngine(i, renderer.get());
            renderer->Start(capture_->MasterEpochQpc());

            for (int retries = 0; retries < 30 && !renderer->IsRunning(); ++retries) {
                Sleep(100);
            }

            renderers_.push_back(std::move(renderer));
        }

        // Phase 6: Initial alignment
        Sleep(500);
        syncCtrl_->ComputeInitialAlignment();

        // Phase 7: Telemetry callback -> post to GUI
        syncCtrl_->SetReportCallback(
            [this](const DeviceDriftReport* reports, uint32_t count) {
                auto* p = new DriftReportPayload();
                for (uint32_t i = 0; i < count && i < kMaxDevices; ++i) {
                    p->reports[i] = reports[i];
                }
                p->count = count;
                PostMessage(guiHwnd_, WM_ENGINE_DRIFT_REPORT, 0,
                            reinterpret_cast<LPARAM>(p));
            });
        syncCtrl_->Start();

        // Hot-plug monitoring (uses class members for debounce to avoid dangling refs)
        lastChangeTime_ = std::chrono::steady_clock::now() - std::chrono::seconds(10);

        deviceManager_->SetChangeCallback(
            [this](const std::wstring& deviceId, bool added) {
                auto now = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> lock(debounceMutex_);
                if (std::chrono::duration_cast<std::chrono::seconds>(
                        now - lastChangeTime_).count() < 3) {
                    return;
                }
                lastChangeTime_ = now;

                if (!added) {
                    // Find and stop matching render engine
                    for (uint32_t i = 0; i < static_cast<uint32_t>(renderers_.size()); ++i) {
                        if (renderers_[i] && renderers_[i]->DeviceId() == deviceId) {
                            renderers_[i]->Stop();
                            syncCtrl_->BindRenderEngine(i, nullptr);
                            break;
                        }
                    }
                }

                auto* p = new DeviceChangePayload{deviceId, added};
                PostMessage(guiHwnd_, WM_ENGINE_DEVICE_CHANGE, 0,
                            reinterpret_cast<LPARAM>(p));
            });
        deviceManager_->StartMonitoring();

        // Watchdog
        watchdog_->RegisterSlot("Capture");
        for (uint32_t i = 0; i < static_cast<uint32_t>(active.size()); ++i) {
            watchdog_->RegisterSlot("Render");
        }
        watchdog_->Start();

        streaming_.store(true, std::memory_order_release);
        PostMessage(guiHwnd_, WM_ENGINE_STATE_CHANGE, 1, 0);

    } catch (const std::exception& e) {
        char msg[256];
        snprintf(msg, sizeof(msg), "[Engine] Start failed: %s\n", e.what());
        OutputDebugStringA(msg);
        PostMessage(guiHwnd_, WM_ENGINE_STATE_CHANGE, 0, 0);
    }
}

void EngineOrchestrator::DoStopStreaming() {
    if (!streaming_.load(std::memory_order_acquire)) return;

    if (watchdog_) watchdog_->Stop();
    if (deviceManager_) deviceManager_->StopMonitoring();
    if (syncCtrl_) syncCtrl_->Stop();

    for (auto& r : renderers_) {
        if (r) r->Stop();
    }
    renderers_.clear();

    if (capture_) capture_->Stop();

    watchdog_.reset();
    syncCtrl_.reset();
    capture_.reset();
    ring_.reset();
    deviceManager_.reset();

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        activeEndpoints_.clear();
    }

    streaming_.store(false, std::memory_order_release);
    PostMessage(guiHwnd_, WM_ENGINE_STATE_CHANGE, 0, 0);
}

void EngineOrchestrator::DoRefreshDevices() {
    DoStopStreaming();
    DoStartStreaming();
}

} // namespace msbt
