#pragma once
// ============================================================================
// EngineOrchestrator -- Background Engine Thread
//
// Encapsulates the entire audio engine lifecycle (device enumeration, capture,
// render, sync, watchdog) on a dedicated background thread.  Communicates
// with the GUI via PostMessage and accepts commands via a thread-safe queue.
// ============================================================================

#include "Common.h"
#include "GuiMessages.h"
#include "Config.h"
#include "Logger.h"
#include "RingBuffer.h"
#include "CaptureEngine.h"
#include "RenderEngine.h"
#include "SyncController.h"
#include "DeviceManager.h"
#include "Watchdog.h"

#include <thread>
#include <deque>
#include <mutex>
#include <chrono>
#include <vector>
#include <memory>

namespace msbt {

class EngineOrchestrator {
public:
    explicit EngineOrchestrator(HWND guiHwnd);
    ~EngineOrchestrator();

    EngineOrchestrator(const EngineOrchestrator&) = delete;
    EngineOrchestrator& operator=(const EngineOrchestrator&) = delete;

    // Start the background engine thread
    void Launch();

    // Post a command from the GUI thread
    void PostCommand(EngineCommand cmd);

    // Thread-safe queries
    bool IsStreaming() const { return streaming_.load(std::memory_order_acquire); }

private:
    void EngineThreadMain();
    void ProcessCommands();
    void DoStartStreaming();
    void DoStopStreaming();
    void DoRefreshDevices();

    HWND guiHwnd_;

    // Command queue
    std::mutex cmdMutex_;
    std::deque<CommandMsg> cmdQueue_;
    HANDLE cmdEvent_ = nullptr;

    // Thread
    std::thread thread_;
    std::atomic<bool> shutdownRequested_{false};
    std::atomic<bool> streaming_{false};

    // Engine objects (only accessed on engine thread)
    std::unique_ptr<DeviceManager> deviceManager_;
    std::unique_ptr<RingBuffer> ring_;
    std::unique_ptr<SyncController> syncCtrl_;
    std::unique_ptr<CaptureEngine> capture_;
    std::vector<std::unique_ptr<RenderEngine>> renderers_;
    std::unique_ptr<Watchdog> watchdog_;

    // Device info for GUI binding
    mutable std::mutex stateMutex_;
    std::vector<AudioEndpointInfo> activeEndpoints_;

    // Hot-plug debounce state (must outlive lambda captures)
    std::mutex debounceMutex_;
    std::chrono::steady_clock::time_point lastChangeTime_{
        std::chrono::steady_clock::now() - std::chrono::seconds(10)};
};

} // namespace msbt
