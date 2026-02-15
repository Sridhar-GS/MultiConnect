// ============================================================================
// DeviceManager.cpp -- Bluetooth USB Audio Endpoint Discovery
// ============================================================================

#include "DeviceManager.h"
#include <Propvarutil.h>
#include <algorithm>
#include <cwctype>

#pragma comment(lib, "Propsys.lib")

namespace msbt {

DeviceManager::DeviceManager() {
    HrCheck(CoCreateInstance(
                __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                IID_PPV_ARGS(&enumerator_)),
            "CoCreateInstance MMDeviceEnumerator (DeviceManager)");
}

DeviceManager::~DeviceManager() {
    StopMonitoring();
}

std::vector<AudioEndpointInfo> DeviceManager::EnumerateBluetoothSinks() {
    std::vector<AudioEndpointInfo> results;

    ComPtr<IMMDeviceCollection> collection;
    HrCheck(enumerator_->EnumAudioEndpoints(
                eRender,
                DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED,
                &collection),
            "EnumAudioEndpoints");

    UINT count = 0;
    HrCheck(collection->GetCount(&count), "GetCount");

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        HRESULT hr = collection->Item(i, &device);
        if (FAILED(hr)) continue;

        LPWSTR rawId = nullptr;
        hr = device->GetId(&rawId);
        if (FAILED(hr)) continue;
        std::wstring deviceId(rawId);
        CoTaskMemFree(rawId);

        DWORD state = 0;
        device->GetState(&state);

        ComPtr<IPropertyStore> props;
        hr = device->OpenPropertyStore(STGM_READ, &props);
        if (FAILED(hr)) continue;

        PROPVARIANT varName;
        PropVariantInit(&varName);
        std::wstring friendlyName;
        hr = props->GetValue(PKEY_Device_FriendlyName, &varName);
        if (SUCCEEDED(hr) && varName.vt == VT_LPWSTR && varName.pwszVal) {
            friendlyName = varName.pwszVal;
        }
        PropVariantClear(&varName);

        PROPVARIANT varInterface;
        PropVariantInit(&varInterface);
        std::wstring interfaceName;
        hr = props->GetValue(PKEY_DeviceInterface_FriendlyName, &varInterface);
        if (SUCCEEDED(hr) && varInterface.vt == VT_LPWSTR && varInterface.pwszVal) {
            interfaceName = varInterface.pwszVal;
        }
        PropVariantClear(&varInterface);

        bool isBt = IsBluetoothUsbDevice(device.Get());

        if (!isBt) {
            std::wstring nameLower = friendlyName;
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
                           [](wchar_t c) { return std::towlower(c); });
            std::wstring ifLower = interfaceName;
            std::transform(ifLower.begin(), ifLower.end(), ifLower.begin(),
                           [](wchar_t c) { return std::towlower(c); });

            isBt = (nameLower.find(L"bluetooth") != std::wstring::npos) ||
                   (ifLower.find(L"bluetooth") != std::wstring::npos) ||
                   (nameLower.find(L"hands-free") != std::wstring::npos) ||
                   (nameLower.find(L"stereo") != std::wstring::npos &&
                    ifLower.find(L"bluetooth") != std::wstring::npos);
        }

        if (isBt) {
            AudioEndpointInfo info;
            info.deviceId       = std::move(deviceId);
            info.friendlyName   = std::move(friendlyName);
            info.interfaceName  = std::move(interfaceName);
            info.isBluetoothUsb = true;
            info.isActive       = (state == DEVICE_STATE_ACTIVE);
            results.push_back(std::move(info));
        }
    }

    return results;
}

bool DeviceManager::IsBluetoothUsbDevice(IMMDevice* device) {
    LPWSTR rawId = nullptr;
    HRESULT hr = device->GetId(&rawId);
    if (FAILED(hr)) return false;

    std::wstring id(rawId);
    CoTaskMemFree(rawId);

    std::wstring idLower = id;
    std::transform(idLower.begin(), idLower.end(), idLower.begin(),
                   [](wchar_t c) { return std::towlower(c); });

    if (idLower.find(L"bthenum") != std::wstring::npos ||
        idLower.find(L"bth") != std::wstring::npos) {
        return true;
    }

    ComPtr<IPropertyStore> props;
    hr = device->OpenPropertyStore(STGM_READ, &props);
    if (FAILED(hr)) return false;

    PROPVARIANT varFormFactor;
    PropVariantInit(&varFormFactor);
    hr = props->GetValue(PKEY_AudioEndpoint_FormFactor, &varFormFactor);
    if (SUCCEEDED(hr) && varFormFactor.vt == VT_UI4) {
        UINT32 ff = varFormFactor.ulVal;
        PropVariantClear(&varFormFactor);
        if (ff == 3 || ff == 5) {
            PROPVARIANT varDesc;
            PropVariantInit(&varDesc);
            hr = props->GetValue(PKEY_Device_DeviceDesc, &varDesc);
            if (SUCCEEDED(hr) && varDesc.vt == VT_LPWSTR && varDesc.pwszVal) {
                std::wstring desc(varDesc.pwszVal);
                std::transform(desc.begin(), desc.end(), desc.begin(),
                               [](wchar_t c) { return std::towlower(c); });
                PropVariantClear(&varDesc);
                if (desc.find(L"bluetooth") != std::wstring::npos) {
                    return true;
                }
            } else {
                PropVariantClear(&varDesc);
            }
        }
    } else {
        PropVariantClear(&varFormFactor);
    }

    return false;
}

void DeviceManager::SetChangeCallback(DeviceChangeCallback cb) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    changeCallback_ = std::move(cb);
}

void DeviceManager::StartMonitoring() {
    if (monitoring_) return;
    HRESULT hr = enumerator_->RegisterEndpointNotificationCallback(this);
    if (SUCCEEDED(hr)) {
        monitoring_ = true;
    }
}

void DeviceManager::StopMonitoring() {
    if (!monitoring_) return;
    enumerator_->UnregisterEndpointNotificationCallback(this);
    monitoring_ = false;
}

// -- IUnknown --

ULONG STDMETHODCALLTYPE DeviceManager::AddRef() {
    return refCount_.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE DeviceManager::Release() {
    ULONG count = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    // Stack-owned; never self-delete.
    return count;
}

HRESULT STDMETHODCALLTYPE DeviceManager::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
        *ppv = static_cast<IMMNotificationClient*>(this);
        AddRef();
        return S_OK;
    }

    *ppv = nullptr;
    return E_NOINTERFACE;
}

// -- IMMNotificationClient --

HRESULT STDMETHODCALLTYPE DeviceManager::OnDeviceStateChanged(
        LPCWSTR pwstrDeviceId, DWORD dwNewState) {
    DeviceChangeCallback cbCopy;
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        cbCopy = changeCallback_;
    }
    if (cbCopy && pwstrDeviceId) {
        bool added = (dwNewState == DEVICE_STATE_ACTIVE);
        cbCopy(std::wstring(pwstrDeviceId), added);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceManager::OnDeviceAdded(LPCWSTR pwstrDeviceId) {
    DeviceChangeCallback cbCopy;
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        cbCopy = changeCallback_;
    }
    if (cbCopy && pwstrDeviceId) {
        cbCopy(std::wstring(pwstrDeviceId), true);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceManager::OnDeviceRemoved(LPCWSTR pwstrDeviceId) {
    DeviceChangeCallback cbCopy;
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        cbCopy = changeCallback_;
    }
    if (cbCopy && pwstrDeviceId) {
        cbCopy(std::wstring(pwstrDeviceId), false);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceManager::OnDefaultDeviceChanged(
        EDataFlow /*flow*/, ERole /*role*/, LPCWSTR /*pwstrDefaultDeviceId*/) {
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceManager::OnPropertyValueChanged(
        LPCWSTR /*pwstrDeviceId*/, const PROPERTYKEY /*key*/) {
    return S_OK;
}

} // namespace msbt
