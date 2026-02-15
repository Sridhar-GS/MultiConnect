#pragma once
// ============================================================================
// Gui.h -- MainWindow: Direct2D-Rendered GUI for MultiConnect
//
// A Win32 window with Direct2D rendering showing:
//   - Header with app logo, name, and engine status
//   - Scrollable list of Bluetooth device cards with icons
//   - Per-device volume sliders with speaker/mute icons
//   - Expandable telemetry details
//   - Action bar with icon buttons (Refresh, Bluetooth, Sound)
//   - Footer with runtime info
//
// Icons: Segoe MDL2 Assets on Windows 10+; text fallback on Windows 7/8.
// ============================================================================

#pragma warning(push)
#pragma warning(disable: 4365 4868 5039 5204)
#include <d2d1.h>
#include <dwrite.h>
#pragma warning(pop)

#include "Common.h"
#include "GuiMessages.h"
#include "VolumeController.h"

#include <vector>
#include <memory>
#include <string>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace msbt {

class EngineOrchestrator;

// ---- Color palette (dark theme, Windows 11 style) ----
namespace Colors {
    inline constexpr D2D1_COLOR_F Background    = {0.118f, 0.118f, 0.118f, 1.0f};
    inline constexpr D2D1_COLOR_F CardBg        = {0.176f, 0.176f, 0.188f, 1.0f};
    inline constexpr D2D1_COLOR_F CardHover     = {0.220f, 0.220f, 0.235f, 1.0f};
    inline constexpr D2D1_COLOR_F CardBorder    = {0.260f, 0.260f, 0.280f, 0.600f};
    inline constexpr D2D1_COLOR_F TextPrimary   = {1.0f,   1.0f,   1.0f,   1.0f};
    inline constexpr D2D1_COLOR_F TextSecondary = {0.750f, 0.750f, 0.750f, 1.0f};
    inline constexpr D2D1_COLOR_F TextDim       = {0.500f, 0.500f, 0.500f, 1.0f};
    inline constexpr D2D1_COLOR_F Connected     = {0.306f, 0.788f, 0.690f, 1.0f};
    inline constexpr D2D1_COLOR_F Disconnected  = {0.957f, 0.278f, 0.278f, 1.0f};
    inline constexpr D2D1_COLOR_F AccentBlue    = {0.200f, 0.530f, 0.900f, 1.0f};
    inline constexpr D2D1_COLOR_F SliderTrack   = {0.333f, 0.333f, 0.333f, 1.0f};
    inline constexpr D2D1_COLOR_F SliderFill    = {0.0f,   0.478f, 0.800f, 1.0f};
    inline constexpr D2D1_COLOR_F ButtonBg      = {0.220f, 0.220f, 0.235f, 1.0f};
    inline constexpr D2D1_COLOR_F ButtonHover   = {0.310f, 0.310f, 0.330f, 1.0f};
    inline constexpr D2D1_COLOR_F HeaderBg      = {0.082f, 0.082f, 0.090f, 1.0f};
    inline constexpr D2D1_COLOR_F Footer        = {0.082f, 0.082f, 0.090f, 1.0f};
    inline constexpr D2D1_COLOR_F Separator     = {0.280f, 0.280f, 0.280f, 1.0f};
    inline constexpr D2D1_COLOR_F SliderMuted   = {0.400f, 0.400f, 0.400f, 1.0f};
}

// ---- Segoe MDL2 Assets icon glyphs (Windows 10+) ----
namespace Icons {
    inline constexpr wchar_t Bluetooth   = L'\xE702';
    inline constexpr wchar_t Speaker     = L'\xE767';
    inline constexpr wchar_t Mute        = L'\xE74F';
    inline constexpr wchar_t Refresh     = L'\xE72C';
    inline constexpr wchar_t Settings    = L'\xE713';
    inline constexpr wchar_t ChevronDown = L'\xE70D';
    inline constexpr wchar_t ChevronUp   = L'\xE70E';
}

// ---- Per-device card state ----
struct DeviceCardState {
    AudioEndpointInfo endpoint;
    DeviceDriftReport lastReport{};
    float   volume   = 1.0f;
    bool    muted    = false;
    bool    expanded = false;
    uint32_t engineIndex = UINT32_MAX;

    // Computed layout (in DIPs from top of scrollable content)
    float top    = 0.0f;
    float height = 0.0f;
};

// ---- Hit-test zones within a card ----
enum class HitZone { None, Chevron, SliderTrack, MuteButton, CardBody };

class MainWindow {
public:
    static constexpr const wchar_t* kClassName = L"MultiConnectMainWindow";

    static bool RegisterWindowClass(HINSTANCE hInstance);
    static MainWindow* Create(HINSTANCE hInstance, int nCmdShow);

    HWND Handle() const { return hwnd_; }
    int  RunMessageLoop();

private:
    MainWindow() = default;
    ~MainWindow();

    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    // ---- Lifecycle ----
    void OnCreate();
    void OnDestroy();
    void LoadAndSetIcon();
    void LoadLogoBitmap();

    // ---- Direct2D resources ----
    HRESULT CreateDeviceIndependentResources();
    HRESULT CreateDeviceDependentResources();
    void    DiscardDeviceDependentResources();

    // ---- Painting ----
    void OnPaint();
    void OnResize(UINT width, UINT height);

    void PaintHeader(float y);
    void PaintDeviceCards(float top, float bottom);
    void PaintSingleCard(const DeviceCardState& card, int cardIdx);
    void PaintVolumeSlider(D2D1_RECT_F bounds, float volume, bool muted);
    void PaintExpandedDetails(D2D1_RECT_F bounds, const DeviceDriftReport& report);
    void PaintActionBar(float y);
    void PaintFooter(float y);
    void PaintButton(D2D1_RECT_F bounds, const wchar_t* text, bool hovered,
                     wchar_t icon = 0);
    void PaintEmptyState(float top, float bottom);

    // ---- Icon rendering helper ----
    void DrawGlyph(D2D1_RECT_F bounds, wchar_t glyph,
                   const wchar_t* fallback, ID2D1Brush* brush);

    // ---- Layout ----
    void RecalculateLayout();
    float Dip(float px) const { return px * dpiScale_; }
    float HeaderHeight() const      { return Dip(48.0f); }
    float ActionBarHeight() const   { return Dip(48.0f); }
    float FooterHeight() const      { return Dip(28.0f); }
    float CardMarginH() const       { return Dip(10.0f); }
    float CardMarginV() const       { return Dip(8.0f); }
    float CardPadding() const       { return Dip(12.0f); }
    float CardCornerRadius() const  { return Dip(8.0f); }
    float CardCollapsedH() const    { return Dip(84.0f); }
    float CardDisconnectedH() const { return Dip(48.0f); }
    float CardExpandedH() const     { return Dip(210.0f); }

    // ---- Hit testing ----
    int     HitTestCard(float x, float y) const;
    HitZone HitTestZone(int cardIndex, float x, float y) const;
    int     HitTestActionButton(float x, float y) const;

    // Slider geometry for a specific card
    void GetSliderBounds(int cardIndex, float& left, float& right, float& centerY) const;

    // ---- Input ----
    void OnLButtonDown(int x, int y);
    void OnLButtonUp(int x, int y);
    void OnMouseMove(int x, int y);
    void OnMouseWheel(short delta);

    // ---- Engine message handlers ----
    void OnDeviceListReceived(DeviceListPayload* payload);
    void OnDriftReportReceived(DriftReportPayload* payload);
    void OnEngineStateChanged(WPARAM running);
    void OnDeviceChangeReceived(DeviceChangePayload* payload);

    // ---- Members ----
    HWND      hwnd_      = nullptr;
    HINSTANCE hInstance_  = nullptr;
    float     dpiScale_  = 1.0f;
    float     clientW_   = 0.0f;
    float     clientH_   = 0.0f;
    DWORD     startTick_ = 0;
    HICON     hIconBig_  = nullptr;
    HICON     hIconSmall_ = nullptr;

    // Direct2D
    ComPtr<ID2D1Factory>          d2dFactory_;
    ComPtr<ID2D1HwndRenderTarget> renderTarget_;

    // DirectWrite
    ComPtr<IDWriteFactory>    dwriteFactory_;
    ComPtr<IDWriteTextFormat> titleFont_;
    ComPtr<IDWriteTextFormat> bodyFont_;
    ComPtr<IDWriteTextFormat> monoFont_;
    ComPtr<IDWriteTextFormat> smallFont_;
    ComPtr<IDWriteTextFormat> iconFont_;      // Segoe MDL2 Assets (Win10+)
    bool                      hasIconFont_ = false;

    // Logo (D2D bitmap loaded from PNG via GDI+)
    ComPtr<ID2D1Bitmap> logoBitmap_;

    // Brushes (device-dependent)
    ComPtr<ID2D1SolidColorBrush> bgBrush_;
    ComPtr<ID2D1SolidColorBrush> cardBgBrush_;
    ComPtr<ID2D1SolidColorBrush> cardHoverBrush_;
    ComPtr<ID2D1SolidColorBrush> cardBorderBrush_;
    ComPtr<ID2D1SolidColorBrush> textBrush_;
    ComPtr<ID2D1SolidColorBrush> textSecBrush_;
    ComPtr<ID2D1SolidColorBrush> textDimBrush_;
    ComPtr<ID2D1SolidColorBrush> connBrush_;
    ComPtr<ID2D1SolidColorBrush> discBrush_;
    ComPtr<ID2D1SolidColorBrush> accentBlueBrush_;
    ComPtr<ID2D1SolidColorBrush> sliderTrackBrush_;
    ComPtr<ID2D1SolidColorBrush> sliderFillBrush_;
    ComPtr<ID2D1SolidColorBrush> sliderMutedBrush_;
    ComPtr<ID2D1SolidColorBrush> btnBrush_;
    ComPtr<ID2D1SolidColorBrush> btnHoverBrush_;
    ComPtr<ID2D1SolidColorBrush> headerBrush_;
    ComPtr<ID2D1SolidColorBrush> footerBrush_;
    ComPtr<ID2D1SolidColorBrush> separatorBrush_;

    // UI state
    std::vector<DeviceCardState> cards_;
    float scrollOffset_    = 0.0f;
    float maxScroll_       = 0.0f;
    int   draggingSlider_  = -1;
    int   hoveredCard_     = -1;
    int   hoveredButton_   = -1;
    bool  engineRunning_   = false;
    uint32_t activeDeviceCount_ = 0;
    uint32_t totalDeviceCount_  = 0;

    // Subsystems
    std::unique_ptr<VolumeController>   volumeCtrl_;
    std::unique_ptr<EngineOrchestrator> engine_;
};

} // namespace msbt
