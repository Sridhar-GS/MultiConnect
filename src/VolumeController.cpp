// ============================================================================
// VolumeController.cpp -- Per-Device Endpoint Volume Control
// ============================================================================

#include "VolumeController.h"

#include <algorithm>

namespace msbt {

VolumeController::VolumeController() {
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(&enumerator_));
    if (FAILED(hr)) {
        OutputDebugStringA("[VolumeController] Failed to create MMDeviceEnumerator\n");
    }
}

VolumeController::~VolumeController() {
    UnbindAll();
}

bool VolumeController::BindDevice(uint32_t index, const std::wstring& deviceId) {
    if (index >= kMaxDevices || !enumerator_) return false;

    auto& slot = slots_[index];
    slot.endpointVolume.Reset();
    slot.bound = false;
    slot.deviceId.clear();

    ComPtr<IMMDevice> device;
    HRESULT hr = enumerator_->GetDevice(deviceId.c_str(), &device);
    if (FAILED(hr)) return false;

    hr = device->Activate(
        __uuidof(IAudioEndpointVolume), CLSCTX_ALL,
        nullptr, reinterpret_cast<void**>(slot.endpointVolume.GetAddressOf()));
    if (FAILED(hr)) return false;

    slot.deviceId = deviceId;
    slot.bound = true;
    return true;
}

void VolumeController::UnbindDevice(uint32_t index) {
    if (index >= kMaxDevices) return;
    auto& slot = slots_[index];
    slot.endpointVolume.Reset();
    slot.deviceId.clear();
    slot.bound = false;
}

void VolumeController::UnbindAll() {
    for (uint32_t i = 0; i < kMaxDevices; ++i) {
        UnbindDevice(i);
    }
}

bool VolumeController::IsBound(uint32_t index) const {
    if (index >= kMaxDevices) return false;
    return slots_[index].bound;
}

float VolumeController::GetVolume(uint32_t index) const {
    if (index >= kMaxDevices || !slots_[index].bound) return 1.0f;

    float level = 1.0f;
    HRESULT hr = slots_[index].endpointVolume->GetMasterVolumeLevelScalar(&level);
    if (FAILED(hr)) return 1.0f;
    return level;
}

void VolumeController::SetVolume(uint32_t index, float level) {
    if (index >= kMaxDevices || !slots_[index].bound) return;
    level = std::clamp(level, 0.0f, 1.0f);
    slots_[index].endpointVolume->SetMasterVolumeLevelScalar(level, nullptr);
}

bool VolumeController::IsMuted(uint32_t index) const {
    if (index >= kMaxDevices || !slots_[index].bound) return false;

    BOOL muted = FALSE;
    HRESULT hr = slots_[index].endpointVolume->GetMute(&muted);
    if (FAILED(hr)) return false;
    return muted != FALSE;
}

void VolumeController::SetMute(uint32_t index, bool mute) {
    if (index >= kMaxDevices || !slots_[index].bound) return;
    slots_[index].endpointVolume->SetMute(mute ? TRUE : FALSE, nullptr);
}

void VolumeController::ToggleMute(uint32_t index) {
    SetMute(index, !IsMuted(index));
}

} // namespace msbt
