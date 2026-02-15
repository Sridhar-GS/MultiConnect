#pragma once
// ============================================================================
// VolumeController -- Per-Device Endpoint Volume Control
//
// Wraps IAudioEndpointVolume for each Bluetooth audio device, providing
// scalar volume (0.0-1.0) and mute control.  All methods are called from
// the GUI thread only.
// ============================================================================

#include "Common.h"
#include <endpointvolume.h>
#include <array>
#include <string>

namespace msbt {

class VolumeController {
public:
    VolumeController();
    ~VolumeController();

    VolumeController(const VolumeController&) = delete;
    VolumeController& operator=(const VolumeController&) = delete;

    // Bind an endpoint slot to a specific device.  Returns false if unavailable.
    bool BindDevice(uint32_t index, const std::wstring& deviceId);
    void UnbindDevice(uint32_t index);
    void UnbindAll();

    bool IsBound(uint32_t index) const;

    // Scalar volume: 0.0 (silence) to 1.0 (full)
    float GetVolume(uint32_t index) const;
    void  SetVolume(uint32_t index, float level);

    // Mute control
    bool IsMuted(uint32_t index) const;
    void SetMute(uint32_t index, bool mute);
    void ToggleMute(uint32_t index);

private:
    struct Slot {
        ComPtr<IAudioEndpointVolume> endpointVolume;
        std::wstring deviceId;
        bool bound = false;
    };

    ComPtr<IMMDeviceEnumerator> enumerator_;
    std::array<Slot, kMaxDevices> slots_{};
};

} // namespace msbt
