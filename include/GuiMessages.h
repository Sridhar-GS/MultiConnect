#pragma once
// ============================================================================
// GuiMessages.h -- Cross-Thread Message Definitions for GUI <-> Engine
//
// The engine runs on a background thread and communicates with the GUI on the
// main thread via PostMessage with heap-allocated payloads.  The receiver
// (MainWindow::HandleMessage) owns and deletes the payload after processing.
// ============================================================================

#include "Common.h"
#include "DeviceManager.h"
#include "SyncController.h"
#include <vector>
#include <string>

namespace msbt {

// ---- Custom window messages (engine thread -> GUI thread) ----
constexpr UINT WM_ENGINE_DRIFT_REPORT  = WM_APP + 1;
constexpr UINT WM_ENGINE_DEVICE_LIST   = WM_APP + 2;
constexpr UINT WM_ENGINE_STATE_CHANGE  = WM_APP + 3;   // wParam: 1=running, 0=stopped
constexpr UINT WM_ENGINE_DEVICE_CHANGE = WM_APP + 4;

// ---- Heap-allocated payloads (PostMessage LPARAM) ----

struct DriftReportPayload {
    DeviceDriftReport reports[kMaxDevices];
    uint32_t count = 0;
};

struct DeviceListPayload {
    std::vector<AudioEndpointInfo> devices;
};

struct DeviceChangePayload {
    std::wstring deviceId;
    bool added = false;
};

// ---- Engine commands (GUI thread -> engine thread) ----

enum class EngineCommand : uint32_t {
    StartStreaming,
    StopStreaming,
    RefreshDevices,
    Shutdown,
};

struct CommandMsg {
    EngineCommand cmd;
};

} // namespace msbt
