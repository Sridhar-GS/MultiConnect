#pragma once
// ============================================================================
// DeviceManager -- USB Bluetooth Audio Endpoint Enumeration & Binding
//
// Uses IMMDeviceEnumerator to discover audio render endpoints, filters for
// Bluetooth audio sinks on USB adapters, and provides device ID strings
// for RenderEngine instantiation.
//
// Also implements IMMNotificationClient for hot-plug detection.
// ============================================================================

#include "Common.h"
#include <vector>
#include <string>
#include <functional>
#include <mutex>

namespace msbt {

struct AudioEndpointInfo {
    std::wstring deviceId;          // WASAPI endpoint ID
    std::wstring friendlyName;      // human-readable name
    std::wstring interfaceName;     // device interface path
    bool         isBluetoothUsb;    // true if BT over USB adapter
    bool         isActive;          // DEVICE_STATE_ACTIVE
};

// Callback for device arrival / removal
using DeviceChangeCallback = std::function<void(const std::wstring& deviceId, bool added)>;

class DeviceManager final : public IMMNotificationClient {
public:
    DeviceManager();
    ~DeviceManager();

    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;

    // Enumerate and return all Bluetooth audio render endpoints
    std::vector<AudioEndpointInfo> EnumerateBluetoothSinks();

    // Register for hot-plug notifications
    void SetChangeCallback(DeviceChangeCallback cb);

    // Start / stop monitoring
    void StartMonitoring();
    void StopMonitoring();

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;

    // IMMNotificationClient
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState) override;
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR pwstrDeviceId) override;
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR pwstrDeviceId) override;
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDefaultDeviceId) override;
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR pwstrDeviceId, const PROPERTYKEY key) override;

private:
    bool IsBluetoothUsbDevice(IMMDevice* device);

    ComPtr<IMMDeviceEnumerator> enumerator_;
    DeviceChangeCallback       changeCallback_;
    std::atomic<ULONG>         refCount_{1};
    std::atomic<bool>          monitoring_{false};
    std::mutex                 callbackMutex_;
};

} // namespace msbt
