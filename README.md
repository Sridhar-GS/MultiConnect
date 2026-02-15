# MultiConnect — Bluetooth Audio Engine

A high-performance Windows application that streams system audio to multiple Bluetooth audio devices simultaneously with microsecond-precision synchronization and active drift compensation.

## Overview

MultiConnect captures the system's audio output via WASAPI loopback and distributes it to up to 10 independent Bluetooth audio sinks in real time. Each device receives the same audio stream with phase-aligned playback, compensating for the differing latencies and clock drifts inherent to Bluetooth A2DP connections.

### Key Capabilities

- **Multi-device playback** -- stream to 1-10 Bluetooth speakers/headphones at once
- **Sub-millisecond sync** -- PI controller + adaptive resampling keeps inter-device drift below 500us
- **Lock-free audio pipeline** -- SPMC ring buffer with zero-copy semantics, no mutex in the hot path
- **Hot-plug support** -- automatic detection of device connect/disconnect with 3-second debounce
- **Auto-reconnection** -- exponential backoff reconnection (3s, 6s, 12s) on device loss
- **Crash resilience** -- SEH safety nets around WASAPI/COM calls, device invalidation handling
- **Direct2D GUI** -- dark-themed window with device cards, per-device volume sliders, expandable telemetry
- **Per-device volume** -- independent volume and mute control for each Bluetooth endpoint

## Installation

### Download Installer
1. Go to the [Releases](https://github.com/Sridhar-GS/MultiConnect/releases) page.
2. Download the latest `MultiConnect-*-win64.exe` installer.
3. Run the installer and follow the prompts.

### Portable Zip
1. Go to the [Releases](https://github.com/Sridhar-GS/MultiConnect/releases) page.
2. Download the latest `MultiConnect-*-win64.zip`.
3. Extract the contents to a folder and run `MultiConnect.exe`.

## Architecture

```
                    +------------------+
                    | System Audio Mix |
                    +--------+---------+
                             |
                    WASAPI Loopback (shared, event-driven)
                             |
                    +--------v---------+
                    |  CaptureEngine   |  QPC-timestamped packets
                    |  (Pro Audio pri) |  48kHz / 16-bit / Stereo
                    +--------+---------+
                             |
                    Lock-free Write (release)
                             |
              +--------------v--------------+
              |     RingBuffer (128KB)      |  SPMC, power-of-2, cache-aligned
              +-+----------+----------+-----+
                |          |          |
          Lock-free Read (acquire)
                |          |          |
         +------v--+ +----v----+ +---v------+
         | Render   | | Render  | | Render   |   Per-device consumer
         | Engine 0 | | Engine 1| | Engine N |   threads
         +----+-----+ +----+---+ +-----+----+
              |             |           |
         Delay Line    Delay Line   Delay Line    Phase alignment
              |             |           |
         Hermite       Hermite      Hermite       Adaptive resampling
         Resampler     Resampler    Resampler
              |             |           |
         WASAPI        WASAPI       WASAPI        Bluetooth endpoints
         Render        Render       Render
              |             |           |
         +----v-----+ +----v---+ +-----v----+
         | BT Sink  | | BT Sink| | BT Sink  |
         +----------+ +--------+ +----------+

    +-------------------+     +-------------------+
    | SyncController    |     | Watchdog          |
    | ~10Hz PI control  |     | 2s health checks  |
    | drift -> resample |     | stall detection   |
    +-------------------+     +-------------------+

    +-------------------+     +-------------------+
    | MainWindow (D2D)  |     | VolumeController  |
    | Device cards, UI  |     | IAudioEndpointVol |
    +-------------------+     +-------------------+
```

### Component Roles

| Component | Thread | Role |
|-----------|--------|------|
| **MainWindow** | Main (GUI) | Direct2D-rendered window: device cards, volume sliders, telemetry, action buttons |
| **EngineOrchestrator** | Background | Manages engine lifecycle, bridges GUI and audio pipeline via PostMessage |
| **VolumeController** | Main (GUI) | Per-device volume/mute control via `IAudioEndpointVolume` |
| **CaptureEngine** | Dedicated, MMCSS Pro Audio | WASAPI loopback capture on default render endpoint. Writes QPC-timestamped PCM into the ring buffer. |
| **RingBuffer** | Shared (lock-free) | 128KB single-producer multiple-consumer circular buffer. Power-of-two masking, 64-byte aligned cursors. |
| **RenderEngine** | 1 per device, MMCSS Pro Audio | Reads from ring buffer, applies delay line + Hermite cubic resampling, pushes to WASAPI render endpoint. |
| **SyncController** | Dedicated, normal priority | 10Hz control loop. Reads drift metrics, computes PI corrections, pushes delay/ratio adjustments. |
| **DeviceManager** | COM callback thread | Enumerates Bluetooth audio sinks. Implements `IMMNotificationClient` for hot-plug events. |
| **Watchdog** | Dedicated, normal priority | Monitors heartbeat counters from audio threads. Logs warnings if any thread stalls for 4+ seconds. |

## Building

### Prerequisites

- **OS**: Windows 10 or later (targets `_WIN32_WINNT=0x0A00`)
- **Compiler**: MSVC (Visual Studio 2022 recommended) with C++23 support
- **Build system**: CMake 3.24+

### Build Commands

```bash
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
```

The build uses `/W4 /WX` (all warnings, treat as errors), `/permissive-` (strict conformance), and `/arch:AVX2`.

### Linked Libraries

| Library | Purpose |
|---------|---------|
| `ole32` | COM runtime |
| `oleaut32` | COM automation |
| `uuid` | GUID/UUID support |
| `winmm` | Windows multimedia |
| `avrt` | MMCSS thread scheduling |
| `ksuser` | Kernel streaming |
| `d2d1` | Direct2D rendering |
| `dwrite` | DirectWrite text rendering |
| `gdiplus` | GDI+ (PNG icon loading) |
| `Propsys` | Property system (device enumeration) |

## Usage

### Running

1. Pair and connect one or more Bluetooth audio devices in Windows Settings
2. Run `MultiConnect.exe`
3. The GUI window shows all Bluetooth devices with connected/disconnected status
4. Audio from all system applications is mirrored to all connected Bluetooth sinks
5. Close the window for clean shutdown

### GUI Features

- **Device cards** -- each Bluetooth device shown as a card with status badge
- **Volume sliders** -- drag to adjust per-device volume in real-time
- **Mute toggle** -- click [M]/[S] to mute/unmute individual devices
- **Expand details** -- click a card or the chevron to see live telemetry (drift, delay, ratio, underruns)
- **Refresh** -- re-enumerate Bluetooth devices
- **BT Settings** -- opens Windows Bluetooth settings (`ms-settings:bluetooth`)
- **Sound Settings** -- opens Windows Sound settings (`ms-settings:sound`)

### Telemetry (Expanded Card)

| Field | Meaning |
|-------|---------|
| Raw Drift | Instantaneous phase drift in microseconds |
| Filtered | EMA-filtered drift (smoothed) |
| Delay | Current delay line depth in frames |
| Ratio | Adaptive resampling ratio (1.0 = no adjustment) |
| Rendered | Total frames rendered to this device |
| Undr / Ovrn | Underrun / overrun counts |

### Configuration

Create a `config.ini` file in the working directory to override defaults:

```ini
# Logging
log_file = multiconnect.log
debug_logging = true
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `log_file` | string | (none) | Path to append log output |
| `debug_logging` | bool | false | Enable DEBUG-level log messages |

## Audio Pipeline Details

### Format

| Parameter | Value |
|-----------|-------|
| Sample rate | 48,000 Hz |
| Bit depth | 16-bit signed PCM |
| Channels | 2 (stereo) |
| Frame size | 4 bytes |

If the system's audio format doesn't match, the engine requests `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM` for automatic conversion by Windows.

### Ring Buffer

- **Size**: 32,768 frames (131,072 bytes, ~682ms at 48kHz)
- **Type**: Lock-free SPMC with atomic publish/acquire semantics
- **Alignment**: Buffer and consumer cursors aligned to 64 bytes (cache-line isolation)
- **Overrun handling**: Consumers automatically snap forward if the producer laps them

### Drift Compensation

Each Bluetooth device has an independent crystal oscillator that drifts relative to the system clock. The engine uses a two-stage compensation approach:

1. **Initial alignment**: Query `IAudioClient::GetStreamLatency()` per device. The slowest device gets zero delay; faster devices get proportionally more delay to align playback start times.

2. **Continuous correction** (PI controller at 10Hz):
   - Measure phase drift via `IAudioClock2::GetDevicePosition()` (or `IAudioClock::GetPosition()` fallback)
   - Proportional gain: `0.000002` (linear scaling, +-0.01 at max drift)
   - Integral gain: `0.0000001` (slow steady-state elimination, ~10s settling)
   - Anti-windup: integral decays by 50% when output is clamped
   - Output: resampling ratio clamped to [0.99, 1.01]
   - Large drift (>2ms): also adjusts the delay line

3. **Hardware rate adjustment**: If the device supports `IAudioClockAdjustment::SetSampleRate` (via `AUDCLNT_STREAMFLAGS_RATEADJUST`), the engine uses hardware-level rate correction. Falls back to manual Hermite cubic resampling otherwise.

### Resampling

The adaptive resampler uses **4-point Hermite cubic interpolation** for smooth, artifact-free rate adjustment:

- Computes coefficients from 4 neighboring samples (s0..s3)
- Maintains a 2-sample history buffer across buffer boundaries for continuity
- At ratio ~1.0 (within 1e-6), short-circuits to a direct memcpy

## Reliability Features

### Crash Prevention

| Feature | Mechanism |
|---------|-----------|
| SEH safety net | `__try/__except` around WASAPI calls in POD-only functions |
| Device invalidation | `AUDCLNT_E_DEVICE_INVALIDATED` detection with graceful exit |
| Consecutive error limit | 3 WASAPI failures in a row triggers thread exit |
| Session disconnect | `IAudioSessionEvents::OnSessionDisconnected` triggers reconnection |
| Exception handling | Top-level `try/catch` in `wWinMain()` prevents unhandled crashes |

### Reconnection

When a Bluetooth device disconnects:

1. The render loop exits (device invalidation or session disconnect)
2. All COM resources are released (`ReleaseResources()`)
3. Reconnection attempts begin with exponential backoff: **3s, 6s, 12s, 12s, 12s** (max 5 attempts)
4. On success: re-initializes WASAPI, resets consumer cursor, resumes rendering
5. On failure: logs and stops the render engine for that device

### Hot-Plug Handling

- `IMMNotificationClient` monitors device state changes
- **Debounce**: 3-second cooldown prevents BT connection flapping from causing rapid start/stop cycles
- On removal: matching `RenderEngine` is stopped and unbound from `SyncController`
- On addition: event is logged (auto-start available for future enhancement)

### Thread Safety

| Shared Resource | Protection |
|-----------------|------------|
| RingBuffer | Lock-free atomics (release/acquire) |
| DeviceSyncState | Per-field atomics, 64-byte aligned (no false sharing) |
| SyncController engines/callback | `std::mutex controlMutex_` |
| DeviceManager callback | `std::mutex callbackMutex_` |
| Logger output | `std::mutex` on all sinks |
| Engine command queue | `std::mutex cmdMutex_` + Win32 event |

## Project Structure

```
MultiConnect/
├── CMakeLists.txt
├── config.ini                  (optional, user-created)
├── asset/
│   ├── logo.png                Application logo
│   └── logo_large.png          High-resolution logo
├── include/
│   ├── Common.h                Constants, DeviceSyncState, QPC utilities, MmcssGuard
│   ├── RingBuffer.h            Lock-free SPMC circular buffer
│   ├── CaptureEngine.h         WASAPI loopback producer
│   ├── RenderEngine.h          Per-device consumer + SessionEventListener
│   ├── SyncController.h        PI controller + telemetry
│   ├── DeviceManager.h         BT endpoint enumeration + IMMNotificationClient
│   ├── Watchdog.h              Thread health monitoring
│   ├── Logger.h                Thread-safe severity-based logging
│   ├── Config.h                INI configuration parser
│   ├── Gui.h                   MainWindow (Direct2D dark-themed GUI)
│   ├── GuiMessages.h           Cross-thread message definitions
│   ├── VolumeController.h      Per-device endpoint volume control
│   └── EngineOrchestrator.h    Background engine thread + command queue
└── src/
    ├── main.cpp                wWinMain entry point, window creation
    ├── Gui.cpp                 Direct2D rendering, input handling
    ├── VolumeController.cpp    IAudioEndpointVolume per device
    ├── EngineOrchestrator.cpp  Engine lifecycle, telemetry routing
    ├── CaptureEngine.cpp       Loopback capture with SEH protection
    ├── RenderEngine.cpp        Render loop, drift measurement, reconnection
    ├── SyncController.cpp      PI control loop, initial alignment
    └── DeviceManager.cpp       COM device enumeration, hot-plug callbacks
```

## Constants Reference

| Constant | Value | Purpose |
|----------|-------|---------|
| `kSampleRate` | 48,000 Hz | Audio sample rate |
| `kBitsPerSample` | 16 | Bit depth |
| `kChannels` | 2 | Stereo |
| `kMaxDevices` | 10 | Maximum simultaneous Bluetooth sinks |
| `kRingFrames` | 32,768 | Ring buffer size in frames (~682ms) |
| `kCapturePeriod` | 10ms | WASAPI capture buffer period |
| `kDriftThresholdUs` | 500 us | Minimum drift before correction activates |
| `kMaxCompensationUs` | 5,000 us | Maximum correctable drift |
| `kDelayLineMaxFrames` | 2,400 | Max delay line depth (50ms) |
| `kDriftFilterAlpha` | 0.02 | EMA smoothing factor |

## License

This project is proprietary software. All rights reserved.
