// ============================================================================
// Gui.cpp -- MainWindow Implementation (Direct2D rendering)
//
// Icons via Segoe MDL2 Assets (Win10+) with text fallback (Win7/8).
// Logo rendered as D2D bitmap loaded from asset/logo.png via GDI+.
// ============================================================================

#pragma warning(push)
#pragma warning(disable: 4365 4458 4514 4820 4868 5039 5204)
#include <d2d1.h>
#include <dwrite.h>
#include <shellapi.h>
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#pragma warning(pop)

#include "Gui.h"
#include "EngineOrchestrator.h"

#include <cstdio>
#include <algorithm>
#include <cmath>

namespace msbt {

// ============================================================================
// Window Registration
// ============================================================================

bool MainWindow::RegisterWindowClass(HINSTANCE hInstance) {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = StaticWndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    return RegisterClassExW(&wc) != 0;
}

MainWindow* MainWindow::Create(HINSTANCE hInstance, int nCmdShow) {
    auto* self = new MainWindow();
    self->hInstance_ = hInstance;

    // Per-monitor DPI awareness (Win10 1703+); fall back to system DPI (Vista+)
    using SetDpiCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    auto fn = reinterpret_cast<SetDpiCtxFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"),
                       "SetProcessDpiAwarenessContext"));
    if (fn) {
        fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    } else {
        SetProcessDPIAware();
    }

    HRESULT hr = self->CreateDeviceIndependentResources();
    if (FAILED(hr)) {
        delete self;
        return nullptr;
    }

    HDC hdc = GetDC(nullptr);
    self->dpiScale_ = static_cast<float>(GetDeviceCaps(hdc, LOGPIXELSX)) / 96.0f;
    ReleaseDC(nullptr, hdc);

    int w = static_cast<int>(560.0f * self->dpiScale_);
    int h = static_cast<int>(640.0f * self->dpiScale_);

    HWND hwnd = CreateWindowExW(
        0, kClassName, L"Multi Connect",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, w, h,
        nullptr, nullptr, hInstance,
        self);

    if (!hwnd) {
        delete self;
        return nullptr;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    return self;
}

int MainWindow::RunMessageLoop() {
    MSG msg{};
    BOOL ret;
    while ((ret = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (ret == 0) ? static_cast<int>(msg.wParam) : 1;
}

MainWindow::~MainWindow() {
    engine_.reset();
    volumeCtrl_.reset();
    if (hIconBig_)   { DestroyIcon(hIconBig_);   hIconBig_  = nullptr; }
    if (hIconSmall_) { DestroyIcon(hIconSmall_); hIconSmall_ = nullptr; }
}

// ============================================================================
// WndProc
// ============================================================================

LRESULT CALLBACK MainWindow::StaticWndProc(HWND hwnd, UINT msg,
                                            WPARAM wParam, LPARAM lParam) {
    MainWindow* self = nullptr;

    if (msg == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = static_cast<MainWindow*>(cs->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->OnCreate();
        return 0;
    }

    self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        return self->HandleMessage(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT:
            OnPaint();
            return 0;

        case WM_SIZE:
            OnResize(LOWORD(lParam), HIWORD(lParam));
            return 0;

        case WM_LBUTTONDOWN:
            OnLButtonDown(static_cast<int>(GET_X_LPARAM(lParam)),
                          static_cast<int>(GET_Y_LPARAM(lParam)));
            return 0;

        case WM_LBUTTONUP:
            OnLButtonUp(static_cast<int>(GET_X_LPARAM(lParam)),
                        static_cast<int>(GET_Y_LPARAM(lParam)));
            return 0;

        case WM_MOUSEMOVE:
            OnMouseMove(static_cast<int>(GET_X_LPARAM(lParam)),
                        static_cast<int>(GET_Y_LPARAM(lParam)));
            return 0;

        case WM_MOUSEWHEEL:
            OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
            return 0;

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = static_cast<LONG>(480 * dpiScale_);
            mmi->ptMinTrackSize.y = static_cast<LONG>(400 * dpiScale_);
            return 0;
        }

        case WM_DPICHANGED: {
            dpiScale_ = static_cast<float>(HIWORD(wParam)) / 96.0f;
            auto* rc = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd_, nullptr,
                         rc->left, rc->top,
                         rc->right - rc->left, rc->bottom - rc->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            DiscardDeviceDependentResources();
            RecalculateLayout();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        case WM_ENGINE_DRIFT_REPORT:
            OnDriftReportReceived(reinterpret_cast<DriftReportPayload*>(lParam));
            return 0;

        case WM_ENGINE_DEVICE_LIST:
            OnDeviceListReceived(reinterpret_cast<DeviceListPayload*>(lParam));
            return 0;

        case WM_ENGINE_STATE_CHANGE:
            OnEngineStateChanged(wParam);
            return 0;

        case WM_ENGINE_DEVICE_CHANGE:
            OnDeviceChangeReceived(reinterpret_cast<DeviceChangePayload*>(lParam));
            return 0;

        case WM_CLOSE:
            engine_.reset();
            DestroyWindow(hwnd_);
            return 0;

        case WM_DESTROY:
            OnDestroy();
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

// ============================================================================
// Lifecycle
// ============================================================================

void MainWindow::OnCreate() {
    startTick_ = GetTickCount();
    LoadAndSetIcon();
    volumeCtrl_ = std::make_unique<VolumeController>();
    engine_ = std::make_unique<EngineOrchestrator>(hwnd_);
    engine_->Launch();
}

void MainWindow::OnDestroy() {
    PostQuitMessage(0);
}

void MainWindow::LoadAndSetIcon() {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';
    std::wstring logoPath = std::wstring(exePath) + L"asset\\logo.png";

    auto loadIcon = [](const wchar_t* path, int size) -> HICON {
        Gdiplus::Bitmap bmp(path);
        if (bmp.GetLastStatus() != Gdiplus::Ok) return nullptr;
        Gdiplus::Bitmap resized(size, size, PixelFormat32bppARGB);
        {
            Gdiplus::Graphics g(&resized);
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.DrawImage(&bmp, 0, 0, size, size);
        }
        HICON hIcon = nullptr;
        resized.GetHICON(&hIcon);
        return hIcon;
    };

    hIconBig_   = loadIcon(logoPath.c_str(), 32);
    hIconSmall_ = loadIcon(logoPath.c_str(), 16);
    if (!hIconBig_) {
        hIconBig_   = loadIcon(L"asset\\logo.png", 32);
        hIconSmall_ = loadIcon(L"asset\\logo.png", 16);
    }

    if (hIconBig_)
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIconBig_));
    if (hIconSmall_)
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIconSmall_));
}

void MainWindow::LoadLogoBitmap() {
    if (logoBitmap_ || !renderTarget_) return;

    auto tryLoad = [&](const wchar_t* path) -> bool {
        Gdiplus::Bitmap bmp(path);
        if (bmp.GetLastStatus() != Gdiplus::Ok) return false;

        UINT w = bmp.GetWidth(), h = bmp.GetHeight();
        Gdiplus::BitmapData data{};
        Gdiplus::Rect rect(0, 0, static_cast<INT>(w), static_cast<INT>(h));
        if (bmp.LockBits(&rect, Gdiplus::ImageLockModeRead,
                          PixelFormat32bppPARGB, &data) != Gdiplus::Ok)
            return false;

        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_PREMULTIPLIED));
        HRESULT hr = renderTarget_->CreateBitmap(
            D2D1::SizeU(w, h), data.Scan0, data.Stride,
            props, logoBitmap_.GetAddressOf());
        bmp.UnlockBits(&data);
        return SUCCEEDED(hr);
    };

    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';
    std::wstring path = std::wstring(exePath) + L"asset\\logo.png";
    if (!tryLoad(path.c_str())) {
        tryLoad(L"asset\\logo.png");
    }
}

// ============================================================================
// Icon rendering helper
// ============================================================================

void MainWindow::DrawGlyph(D2D1_RECT_F bounds, wchar_t glyph,
                            const wchar_t* fallback, ID2D1Brush* brush) {
    if (hasIconFont_) {
        wchar_t str[] = { glyph, L'\0' };
        renderTarget_->DrawText(str, 1, iconFont_.Get(), bounds, brush);
    } else {
        bodyFont_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        bodyFont_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        renderTarget_->DrawText(fallback, static_cast<UINT32>(wcslen(fallback)),
                                 bodyFont_.Get(), bounds, brush);
        bodyFont_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        bodyFont_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
}

// ============================================================================
// Direct2D Resources
// ============================================================================

HRESULT MainWindow::CreateDeviceIndependentResources() {
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                    d2dFactory_.GetAddressOf());
    if (FAILED(hr)) return hr;

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                              __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf()));
    if (FAILED(hr)) return hr;

    // Title font: Segoe UI 14pt Semibold
    hr = dwriteFactory_->CreateTextFormat(
        L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-US",
        titleFont_.GetAddressOf());
    if (FAILED(hr)) return hr;
    titleFont_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // Body font: Segoe UI 11pt
    hr = dwriteFactory_->CreateTextFormat(
        L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"en-US",
        bodyFont_.GetAddressOf());
    if (FAILED(hr)) return hr;
    bodyFont_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // Monospace font: Consolas 10pt (telemetry)
    hr = dwriteFactory_->CreateTextFormat(
        L"Consolas", nullptr,
        DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 10.0f, L"en-US",
        monoFont_.GetAddressOf());
    if (FAILED(hr)) return hr;
    monoFont_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // Small font: Segoe UI 9pt (footer)
    hr = dwriteFactory_->CreateTextFormat(
        L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 9.0f, L"en-US",
        smallFont_.GetAddressOf());
    if (FAILED(hr)) return hr;
    smallFont_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // Icon font: Segoe MDL2 Assets (Windows 10+)
    ComPtr<IDWriteFontCollection> fontCollection;
    dwriteFactory_->GetSystemFontCollection(fontCollection.GetAddressOf());
    UINT32 fontIndex = 0;
    BOOL fontExists = FALSE;
    if (fontCollection) {
        fontCollection->FindFamilyName(L"Segoe MDL2 Assets", &fontIndex, &fontExists);
    }
    hasIconFont_ = (fontExists == TRUE);
    if (hasIconFont_) {
        hr = dwriteFactory_->CreateTextFormat(
            L"Segoe MDL2 Assets", nullptr,
            DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-US",
            iconFont_.GetAddressOf());
        if (FAILED(hr)) {
            hasIconFont_ = false;
        } else {
            iconFont_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            iconFont_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            iconFont_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    }

    return S_OK;
}

HRESULT MainWindow::CreateDeviceDependentResources() {
    if (renderTarget_) return S_OK;

    RECT rc;
    GetClientRect(hwnd_, &rc);
    D2D1_SIZE_U size = D2D1::SizeU(
        static_cast<UINT32>(rc.right - rc.left),
        static_cast<UINT32>(rc.bottom - rc.top));

    auto props = D2D1::RenderTargetProperties();
    props.dpiX = 96.0f * dpiScale_;
    props.dpiY = 96.0f * dpiScale_;

    HRESULT hr = d2dFactory_->CreateHwndRenderTarget(
        props,
        D2D1::HwndRenderTargetProperties(hwnd_, size),
        renderTarget_.GetAddressOf());
    if (FAILED(hr)) return hr;

    auto makeBrush = [&](const D2D1_COLOR_F& c, ComPtr<ID2D1SolidColorBrush>& b) {
        return renderTarget_->CreateSolidColorBrush(c, b.GetAddressOf());
    };

    hr = makeBrush(Colors::Background,    bgBrush_);           if (FAILED(hr)) return hr;
    hr = makeBrush(Colors::CardBg,        cardBgBrush_);       if (FAILED(hr)) return hr;
    hr = makeBrush(Colors::CardHover,     cardHoverBrush_);    if (FAILED(hr)) return hr;
    hr = makeBrush(Colors::CardBorder,    cardBorderBrush_);   if (FAILED(hr)) return hr;
    hr = makeBrush(Colors::TextPrimary,   textBrush_);         if (FAILED(hr)) return hr;
    hr = makeBrush(Colors::TextSecondary, textSecBrush_);      if (FAILED(hr)) return hr;
    hr = makeBrush(Colors::TextDim,       textDimBrush_);      if (FAILED(hr)) return hr;
    hr = makeBrush(Colors::Connected,     connBrush_);         if (FAILED(hr)) return hr;
    hr = makeBrush(Colors::Disconnected,  discBrush_);         if (FAILED(hr)) return hr;
    hr = makeBrush(Colors::AccentBlue,    accentBlueBrush_);   if (FAILED(hr)) return hr;
    hr = makeBrush(Colors::SliderTrack,   sliderTrackBrush_);  if (FAILED(hr)) return hr;
    hr = makeBrush(Colors::SliderFill,    sliderFillBrush_);   if (FAILED(hr)) return hr;
    hr = makeBrush(Colors::SliderMuted,   sliderMutedBrush_);  if (FAILED(hr)) return hr;
    hr = makeBrush(Colors::ButtonBg,      btnBrush_);          if (FAILED(hr)) return hr;
    hr = makeBrush(Colors::ButtonHover,   btnHoverBrush_);     if (FAILED(hr)) return hr;
    hr = makeBrush(Colors::HeaderBg,      headerBrush_);       if (FAILED(hr)) return hr;
    hr = makeBrush(Colors::Footer,        footerBrush_);       if (FAILED(hr)) return hr;
    hr = makeBrush(Colors::Separator,     separatorBrush_);    if (FAILED(hr)) return hr;

    // Load logo as D2D bitmap
    LoadLogoBitmap();

    return S_OK;
}

void MainWindow::DiscardDeviceDependentResources() {
    renderTarget_.Reset();
    bgBrush_.Reset();
    cardBgBrush_.Reset();
    cardHoverBrush_.Reset();
    cardBorderBrush_.Reset();
    textBrush_.Reset();
    textSecBrush_.Reset();
    textDimBrush_.Reset();
    connBrush_.Reset();
    discBrush_.Reset();
    accentBlueBrush_.Reset();
    sliderTrackBrush_.Reset();
    sliderFillBrush_.Reset();
    sliderMutedBrush_.Reset();
    btnBrush_.Reset();
    btnHoverBrush_.Reset();
    headerBrush_.Reset();
    footerBrush_.Reset();
    separatorBrush_.Reset();
    logoBitmap_.Reset();
}

// ============================================================================
// Painting
// ============================================================================

void MainWindow::OnPaint() {
    HRESULT hr = CreateDeviceDependentResources();
    if (FAILED(hr)) return;

    renderTarget_->BeginDraw();
    renderTarget_->Clear(Colors::Background);

    float hdrH = HeaderHeight();
    float abH  = ActionBarHeight();
    float ftH  = FooterHeight();

    PaintHeader(0.0f);

    float scrollTop    = hdrH;
    float scrollBottom = clientH_ - abH - ftH;

    if (scrollBottom > scrollTop) {
        renderTarget_->PushAxisAlignedClip(
            D2D1::RectF(0, scrollTop, clientW_, scrollBottom),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        if (cards_.empty()) {
            PaintEmptyState(scrollTop, scrollBottom);
        } else {
            PaintDeviceCards(scrollTop, scrollBottom);
        }

        renderTarget_->PopAxisAlignedClip();
    }

    PaintActionBar(clientH_ - abH - ftH);
    PaintFooter(clientH_ - ftH);

    hr = renderTarget_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardDeviceDependentResources();
    }
    ValidateRect(hwnd_, nullptr);
}

void MainWindow::OnResize(UINT width, UINT height) {
    clientW_ = static_cast<float>(width) / dpiScale_;
    clientH_ = static_cast<float>(height) / dpiScale_;

    if (renderTarget_) {
        D2D1_SIZE_U sz = D2D1::SizeU(width, height);
        renderTarget_->Resize(sz);
    }

    RecalculateLayout();
}

// ============================================================================
// Header (logo + app name + status)
// ============================================================================

void MainWindow::PaintHeader(float y) {
    D2D1_RECT_F rect = D2D1::RectF(0, y, clientW_, y + HeaderHeight());
    renderTarget_->FillRectangle(rect, headerBrush_.Get());

    float pad = Dip(12.0f);
    float centerY = y + HeaderHeight() / 2.0f;

    // Logo bitmap
    float logoSize = Dip(26.0f);
    if (logoBitmap_) {
        D2D1_RECT_F logoRect = D2D1::RectF(
            pad, centerY - logoSize / 2.0f,
            pad + logoSize, centerY + logoSize / 2.0f);
        renderTarget_->DrawBitmap(logoBitmap_.Get(), logoRect, 1.0f,
                                   D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    // "Multi Connect" title
    float titleX = pad + logoSize + Dip(10.0f);
    D2D1_RECT_F titleRect = D2D1::RectF(
        titleX, y + Dip(6.0f),
        titleX + Dip(150.0f), y + HeaderHeight() - Dip(6.0f));
    titleFont_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    renderTarget_->DrawText(L"Multi Connect", 13,
                             titleFont_.Get(), titleRect, textBrush_.Get());
    titleFont_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    // Status indicator (right side, line 1)
    const wchar_t* statusLabel = engineRunning_ ? L"\u2022 STREAMING" : L"\u2022 STOPPED";
    auto* statusBrush = engineRunning_ ? connBrush_.Get() : discBrush_.Get();

    D2D1_RECT_F statusRect = D2D1::RectF(
        clientW_ - Dip(220.0f), y + Dip(7.0f),
        clientW_ - pad, y + Dip(23.0f));
    bodyFont_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    renderTarget_->DrawText(statusLabel, static_cast<UINT32>(wcslen(statusLabel)),
                             bodyFont_.Get(), statusRect, statusBrush);

    // Format info (right side, line 2)
    D2D1_RECT_F infoRect = D2D1::RectF(
        clientW_ - Dip(220.0f), y + Dip(26.0f),
        clientW_ - pad, y + Dip(42.0f));
    smallFont_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    renderTarget_->DrawText(L"48 kHz / 16-bit / Stereo", 24,
                             smallFont_.Get(), infoRect, textDimBrush_.Get());
    smallFont_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    bodyFont_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

    // Separator line at bottom
    renderTarget_->DrawLine(
        D2D1::Point2F(0, y + HeaderHeight() - 0.5f),
        D2D1::Point2F(clientW_, y + HeaderHeight() - 0.5f),
        separatorBrush_.Get(), 1.0f);
}

// ============================================================================
// Device Cards
// ============================================================================

void MainWindow::PaintDeviceCards(float top, float /*bottom*/) {
    renderTarget_->SetTransform(
        D2D1::Matrix3x2F::Translation(0.0f, top - scrollOffset_));

    for (int i = 0; i < static_cast<int>(cards_.size()); ++i) {
        PaintSingleCard(cards_[static_cast<size_t>(i)], i);
    }

    renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
}

void MainWindow::PaintSingleCard(const DeviceCardState& card, int cardIdx) {
    float x   = CardMarginH();
    float y   = card.top;
    float w   = clientW_ - 2.0f * CardMarginH();
    float h   = card.height;
    float pad = CardPadding();
    float cr  = CardCornerRadius();

    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
        D2D1::RectF(x, y, x + w, y + h), cr, cr);

    // Card background + subtle border
    auto* bgBr = (cardIdx == hoveredCard_) ? cardHoverBrush_.Get() : cardBgBrush_.Get();
    renderTarget_->FillRoundedRectangle(rr, bgBr);
    renderTarget_->DrawRoundedRectangle(rr, cardBorderBrush_.Get(), 1.0f);

    float row1Y = y + pad;

    // ---- Row 1: BT icon, device name, status, chevron ----

    // Bluetooth icon
    auto* btBrush = card.endpoint.isActive ? accentBlueBrush_.Get() : textDimBrush_.Get();
    D2D1_RECT_F btRect = D2D1::RectF(
        x + pad, row1Y, x + pad + Dip(20.0f), row1Y + Dip(20.0f));
    DrawGlyph(btRect, Icons::Bluetooth, L"BT", btBrush);

    // Device name
    D2D1_RECT_F nameRect = D2D1::RectF(
        x + pad + Dip(28.0f), row1Y,
        x + w - Dip(150.0f), row1Y + Dip(20.0f));
    renderTarget_->DrawText(
        card.endpoint.friendlyName.c_str(),
        static_cast<UINT32>(card.endpoint.friendlyName.size()),
        titleFont_.Get(), nameRect, textBrush_.Get());

    // Status dot
    float dotX  = x + w - Dip(130.0f);
    float dotCY = row1Y + Dip(10.0f);
    float dotR  = Dip(4.0f);
    D2D1_ELLIPSE statusDot = D2D1::Ellipse(
        D2D1::Point2F(dotX, dotCY), dotR, dotR);
    if (card.endpoint.isActive) {
        renderTarget_->FillEllipse(statusDot, connBrush_.Get());
    } else {
        renderTarget_->DrawEllipse(statusDot, discBrush_.Get(), 1.5f);
    }

    // Status label
    const wchar_t* statusTxt = card.endpoint.isActive ? L"Connected" : L"Disconnected";
    auto* sBrush = card.endpoint.isActive ? connBrush_.Get() : discBrush_.Get();
    D2D1_RECT_F statusRect = D2D1::RectF(
        dotX + Dip(10.0f), row1Y,
        x + w - Dip(32.0f), row1Y + Dip(20.0f));
    renderTarget_->DrawText(
        statusTxt, static_cast<UINT32>(wcslen(statusTxt)),
        bodyFont_.Get(), statusRect, sBrush);

    // Chevron (only for connected devices)
    if (card.endpoint.isActive) {
        D2D1_RECT_F chevRect = D2D1::RectF(
            x + w - Dip(28.0f), row1Y,
            x + w - pad, row1Y + Dip(20.0f));
        wchar_t chevGlyph = card.expanded ? Icons::ChevronUp : Icons::ChevronDown;
        const wchar_t* chevFallback = card.expanded ? L"\x25B2" : L"\x25BC";
        DrawGlyph(chevRect, chevGlyph, chevFallback, textDimBrush_.Get());
    }

    // ---- Row 2: Volume (for connected devices) ----
    float volY = y + Dip(42.0f);
    if (card.endpoint.isActive) {
        // Speaker icon (left - decorative label)
        D2D1_RECT_F spkRect = D2D1::RectF(
            x + pad, volY, x + pad + Dip(20.0f), volY + Dip(20.0f));
        auto* spkBrush = card.muted ? sliderMutedBrush_.Get() : accentBlueBrush_.Get();
        DrawGlyph(spkRect, Icons::Speaker, L"Vol", spkBrush);

        // Volume slider
        float sliderLeft  = x + pad + Dip(28.0f);
        float sliderRight = x + w - pad - Dip(72.0f);
        float sliderCY    = volY + Dip(10.0f);
        PaintVolumeSlider(
            D2D1::RectF(sliderLeft, sliderCY - Dip(3.0f),
                         sliderRight, sliderCY + Dip(3.0f)),
            card.volume, card.muted);

        // Percentage text
        wchar_t pct[8];
        swprintf_s(pct, L"%d%%", static_cast<int>(card.volume * 100.0f));
        D2D1_RECT_F pctRect = D2D1::RectF(
            sliderRight + Dip(6.0f), volY,
            sliderRight + Dip(40.0f), volY + Dip(20.0f));
        renderTarget_->DrawText(pct, static_cast<UINT32>(wcslen(pct)),
                                 bodyFont_.Get(), pctRect, textBrush_.Get());

        // Mute toggle icon (right)
        D2D1_RECT_F muteRect = D2D1::RectF(
            x + w - pad - Dip(24.0f), volY,
            x + w - pad, volY + Dip(20.0f));
        wchar_t muteGlyph = card.muted ? Icons::Mute : Icons::Speaker;
        auto* muteBrush = card.muted ? discBrush_.Get() : textDimBrush_.Get();
        DrawGlyph(muteRect, muteGlyph, card.muted ? L"M" : L"S", muteBrush);
    }

    // ---- Expanded telemetry details ----
    if (card.expanded && card.endpoint.isActive) {
        float detailsY = y + Dip(72.0f);

        // Separator
        renderTarget_->DrawLine(
            D2D1::Point2F(x + pad, detailsY),
            D2D1::Point2F(x + w - pad, detailsY),
            separatorBrush_.Get(), 1.0f);

        D2D1_RECT_F detailBounds = D2D1::RectF(
            x + pad, detailsY + Dip(6.0f),
            x + w - pad, y + h - pad);
        PaintExpandedDetails(detailBounds, card.lastReport);
    }
}

void MainWindow::PaintVolumeSlider(D2D1_RECT_F bounds, float volume, bool muted) {
    float trackH = bounds.bottom - bounds.top;
    float trackW = bounds.right - bounds.left;
    float centerY = (bounds.top + bounds.bottom) / 2.0f;
    float r = trackH / 2.0f;
    float thumbR = Dip(7.0f);

    // Track background
    D2D1_ROUNDED_RECT track = D2D1::RoundedRect(bounds, r, r);
    renderTarget_->FillRoundedRectangle(track, sliderTrackBrush_.Get());

    // Fill
    float fillX = bounds.left + trackW * volume;
    auto* fillBrush = muted ? sliderMutedBrush_.Get() : sliderFillBrush_.Get();

    D2D1_ROUNDED_RECT fill = D2D1::RoundedRect(
        D2D1::RectF(bounds.left, bounds.top, fillX, bounds.bottom), r, r);
    renderTarget_->FillRoundedRectangle(fill, fillBrush);

    // Thumb
    D2D1_ELLIPSE thumb = D2D1::Ellipse(
        D2D1::Point2F(fillX, centerY), thumbR, thumbR);
    renderTarget_->FillEllipse(thumb, fillBrush);
}

void MainWindow::PaintExpandedDetails(D2D1_RECT_F bounds,
                                       const DeviceDriftReport& r) {
    float lineH = Dip(18.0f);
    float y = bounds.top;
    float midX = (bounds.left + bounds.right) / 2.0f;
    float labelW = Dip(110.0f);

    auto drawRow = [&](const wchar_t* l1, const wchar_t* v1,
                       const wchar_t* l2, const wchar_t* v2) {
        D2D1_RECT_F lr1 = D2D1::RectF(bounds.left, y, bounds.left + labelW, y + lineH);
        renderTarget_->DrawText(l1, static_cast<UINT32>(wcslen(l1)),
                                 bodyFont_.Get(), lr1, textDimBrush_.Get());

        D2D1_RECT_F vr1 = D2D1::RectF(bounds.left + labelW, y, midX - Dip(4.0f), y + lineH);
        renderTarget_->DrawText(v1, static_cast<UINT32>(wcslen(v1)),
                                 monoFont_.Get(), vr1, textBrush_.Get());

        D2D1_RECT_F lr2 = D2D1::RectF(midX, y, midX + labelW, y + lineH);
        renderTarget_->DrawText(l2, static_cast<UINT32>(wcslen(l2)),
                                 bodyFont_.Get(), lr2, textDimBrush_.Get());

        D2D1_RECT_F vr2 = D2D1::RectF(midX + labelW, y, bounds.right, y + lineH);
        renderTarget_->DrawText(v2, static_cast<UINT32>(wcslen(v2)),
                                 monoFont_.Get(), vr2, textBrush_.Get());
        y += lineH;
    };

    wchar_t raw[32], filt[32], delay[16], ratio[16];
    wchar_t frames[24], under[12], over[12], underOver[32];

    swprintf_s(raw,   L"%+.1f us", r.rawDriftUs);
    swprintf_s(filt,  L"%+.1f us", r.filteredDriftUs);
    swprintf_s(delay, L"%u fr",    r.delayFrames);
    swprintf_s(ratio, L"%.6f",     r.resampleRatio);
    swprintf_s(frames, L"%llu",    static_cast<unsigned long long>(r.framesRendered));
    swprintf_s(under,  L"%llu",    static_cast<unsigned long long>(r.underrunCount));
    swprintf_s(over,   L"%llu",    static_cast<unsigned long long>(r.overrunCount));
    swprintf_s(underOver, L"%s / %s", under, over);

    drawRow(L"Raw Drift",    raw,    L"Filtered",    filt);
    drawRow(L"Delay",        delay,  L"Ratio",       ratio);
    drawRow(L"Rendered",     frames, L"Undr / Ovrn", underOver);
}

// ============================================================================
// Action Bar
// ============================================================================

void MainWindow::PaintActionBar(float y) {
    D2D1_RECT_F barRect = D2D1::RectF(0, y, clientW_, y + ActionBarHeight());
    renderTarget_->FillRectangle(barRect, bgBrush_.Get());

    // Separator at top
    renderTarget_->DrawLine(
        D2D1::Point2F(0, y), D2D1::Point2F(clientW_, y),
        separatorBrush_.Get(), 1.0f);

    float btnW   = Dip(120.0f);
    float btnH   = Dip(32.0f);
    float btnY   = y + (ActionBarHeight() - btnH) / 2.0f;
    float gap    = Dip(16.0f);
    float totalW = 3.0f * btnW + 2.0f * gap;
    float startX = (clientW_ - totalW) / 2.0f;

    struct BtnDef { const wchar_t* label; wchar_t icon; };
    BtnDef btns[] = {
        { L"Refresh",   Icons::Refresh   },
        { L"Bluetooth", Icons::Bluetooth },
        { L"Sound",     Icons::Speaker   },
    };

    for (int i = 0; i < 3; ++i) {
        float bx = startX + static_cast<float>(i) * (btnW + gap);
        D2D1_RECT_F btnRect = D2D1::RectF(bx, btnY, bx + btnW, btnY + btnH);
        PaintButton(btnRect, btns[i].label, hoveredButton_ == i, btns[i].icon);
    }
}

void MainWindow::PaintButton(D2D1_RECT_F bounds, const wchar_t* text,
                              bool hovered, wchar_t icon) {
    float cr = Dip(6.0f);
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(bounds, cr, cr);
    renderTarget_->FillRoundedRectangle(rr, hovered ? btnHoverBrush_.Get() : btnBrush_.Get());
    renderTarget_->DrawRoundedRectangle(rr, cardBorderBrush_.Get(), 0.5f);

    if (icon && hasIconFont_) {
        // Icon on left, text right of icon
        float iconLeft = bounds.left + Dip(10.0f);
        float iconW = Dip(18.0f);
        D2D1_RECT_F iconRect = D2D1::RectF(
            iconLeft, bounds.top, iconLeft + iconW, bounds.bottom);
        wchar_t gStr[] = { icon, L'\0' };
        renderTarget_->DrawText(gStr, 1, iconFont_.Get(), iconRect, textSecBrush_.Get());

        D2D1_RECT_F textRect = D2D1::RectF(
            iconLeft + iconW + Dip(4.0f), bounds.top,
            bounds.right - Dip(6.0f), bounds.bottom);
        bodyFont_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        renderTarget_->DrawText(text, static_cast<UINT32>(wcslen(text)),
                                 bodyFont_.Get(), textRect, textBrush_.Get());
        bodyFont_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    } else {
        // Text only, centered
        bodyFont_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        bodyFont_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        renderTarget_->DrawText(text, static_cast<UINT32>(wcslen(text)),
                                 bodyFont_.Get(), bounds, textBrush_.Get());
        bodyFont_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        bodyFont_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
}

// ============================================================================
// Footer
// ============================================================================

void MainWindow::PaintFooter(float y) {
    D2D1_RECT_F rect = D2D1::RectF(0, y, clientW_, y + FooterHeight());
    renderTarget_->FillRectangle(rect, footerBrush_.Get());

    DWORD elapsed = GetTickCount() - startTick_;
    DWORD secs = elapsed / 1000;
    DWORD hrs  = secs / 3600;
    DWORD mins = (secs % 3600) / 60;
    DWORD s    = secs % 60;

    wchar_t footer[128];
    swprintf_s(footer,
        L"  Engine: %s   |   Devices: %u/%u active   |   Uptime: %02u:%02u:%02u",
        engineRunning_ ? L"Running" : L"Stopped",
        activeDeviceCount_, totalDeviceCount_,
        hrs, mins, s);

    D2D1_RECT_F textRect = D2D1::RectF(
        Dip(4.0f), y + Dip(6.0f),
        clientW_ - Dip(4.0f), y + FooterHeight());
    renderTarget_->DrawText(
        footer, static_cast<UINT32>(wcslen(footer)),
        smallFont_.Get(), textRect, textSecBrush_.Get());
}

// ============================================================================
// Empty State
// ============================================================================

void MainWindow::PaintEmptyState(float top, float bottom) {
    float centerX = clientW_ / 2.0f;
    float centerY = (top + bottom) / 2.0f - Dip(20.0f);

    // Large Bluetooth icon (scaled up via transform)
    if (hasIconFont_) {
        float scale = 3.0f;
        D2D1_POINT_2F pivot = D2D1::Point2F(centerX, centerY);
        renderTarget_->SetTransform(
            D2D1::Matrix3x2F::Scale(scale, scale, pivot));

        float halfGlyph = Dip(8.0f);
        D2D1_RECT_F glyphRect = D2D1::RectF(
            centerX - halfGlyph, centerY - halfGlyph,
            centerX + halfGlyph, centerY + halfGlyph);
        wchar_t btStr[] = { Icons::Bluetooth, L'\0' };
        renderTarget_->DrawText(btStr, 1, iconFont_.Get(),
                                 glyphRect, textDimBrush_.Get());
        renderTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    float textTop = centerY + Dip(30.0f);
    const wchar_t* msg = L"No Bluetooth audio devices found.\n"
                         L"Click Refresh or check Bluetooth settings.";
    D2D1_RECT_F msgRect = D2D1::RectF(
        Dip(20.0f), textTop,
        clientW_ - Dip(20.0f), bottom);
    bodyFont_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    bodyFont_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    renderTarget_->DrawText(msg, static_cast<UINT32>(wcslen(msg)),
                             bodyFont_.Get(), msgRect, textDimBrush_.Get());
    bodyFont_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    bodyFont_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
}

// ============================================================================
// Layout
// ============================================================================

void MainWindow::RecalculateLayout() {
    float y = CardMarginV();
    for (auto& card : cards_) {
        card.top = y;
        if (!card.endpoint.isActive) {
            card.height = CardDisconnectedH();
        } else if (card.expanded) {
            card.height = CardExpandedH();
        } else {
            card.height = CardCollapsedH();
        }
        y += card.height + CardMarginV();
    }

    float scrollableH = clientH_ - HeaderHeight() - ActionBarHeight() - FooterHeight();
    maxScroll_ = (std::max)(0.0f, y - scrollableH);
    scrollOffset_ = std::clamp(scrollOffset_, 0.0f, maxScroll_);
}

// ============================================================================
// Hit Testing
// ============================================================================

int MainWindow::HitTestCard(float x, float y) const {
    float contentY = y - HeaderHeight() + scrollOffset_;

    if (y < HeaderHeight() || y > clientH_ - ActionBarHeight() - FooterHeight()) {
        return -1;
    }

    for (int i = 0; i < static_cast<int>(cards_.size()); ++i) {
        const auto& c = cards_[static_cast<size_t>(i)];
        if (contentY >= c.top && contentY < c.top + c.height &&
            x >= CardMarginH() && x < clientW_ - CardMarginH()) {
            return i;
        }
    }
    return -1;
}

HitZone MainWindow::HitTestZone(int cardIndex, float x, float y) const {
    if (cardIndex < 0 || cardIndex >= static_cast<int>(cards_.size())) {
        return HitZone::None;
    }

    const auto& card = cards_[static_cast<size_t>(cardIndex)];
    float contentY = y - HeaderHeight() + scrollOffset_;
    float relY = contentY - card.top;
    float cardRight = clientW_ - CardMarginH();
    float pad = CardPadding();

    // Chevron zone: top-right area (connected devices only)
    if (card.endpoint.isActive && relY < Dip(28.0f) && x > cardRight - Dip(32.0f)) {
        return HitZone::Chevron;
    }

    // Volume area (connected devices, row2 from ~36 to ~62 DIP)
    if (card.endpoint.isActive && relY >= Dip(36.0f) && relY < Dip(64.0f)) {
        // Mute button (right side)
        if (x > cardRight - pad - Dip(28.0f)) {
            return HitZone::MuteButton;
        }
        // Slider track
        float sliderLeft  = CardMarginH() + pad + Dip(28.0f);
        float sliderRight = cardRight - pad - Dip(72.0f);
        if (x >= sliderLeft - Dip(10.0f) && x <= sliderRight + Dip(10.0f)) {
            return HitZone::SliderTrack;
        }
    }

    return HitZone::CardBody;
}

int MainWindow::HitTestActionButton(float x, float y) const {
    float abY = clientH_ - ActionBarHeight() - FooterHeight();
    if (y < abY || y > abY + ActionBarHeight()) return -1;

    float btnW   = Dip(120.0f);
    float btnH   = Dip(32.0f);
    float btnY   = abY + (ActionBarHeight() - btnH) / 2.0f;
    float gap    = Dip(16.0f);
    float totalW = 3.0f * btnW + 2.0f * gap;
    float startX = (clientW_ - totalW) / 2.0f;

    if (y < btnY || y > btnY + btnH) return -1;

    for (int i = 0; i < 3; ++i) {
        float bx = startX + static_cast<float>(i) * (btnW + gap);
        if (x >= bx && x <= bx + btnW) return i;
    }
    return -1;
}

void MainWindow::GetSliderBounds(int cardIndex, float& left, float& right,
                                  float& centerY) const {
    const auto& card = cards_[static_cast<size_t>(cardIndex)];
    float cardLeft  = CardMarginH();
    float cardRight = clientW_ - CardMarginH();
    float pad = CardPadding();

    left  = cardLeft + pad + Dip(28.0f);
    right = cardRight - pad - Dip(72.0f);

    float volY = card.top + Dip(42.0f);
    centerY = volY + Dip(10.0f);
}

// ============================================================================
// Input
// ============================================================================

void MainWindow::OnLButtonDown(int mx, int my) {
    float x = static_cast<float>(mx) / dpiScale_;
    float y = static_cast<float>(my) / dpiScale_;

    // Check action buttons
    int btn = HitTestActionButton(x, y);
    if (btn >= 0) {
        switch (btn) {
            case 0: // Refresh
                if (engine_) engine_->PostCommand(EngineCommand::RefreshDevices);
                break;
            case 1: // Bluetooth Settings
                ShellExecuteW(nullptr, L"open", L"ms-settings:bluetooth",
                              nullptr, nullptr, SW_SHOWNORMAL);
                break;
            case 2: // Sound Settings
                ShellExecuteW(nullptr, L"open", L"ms-settings:sound",
                              nullptr, nullptr, SW_SHOWNORMAL);
                break;
        }
        return;
    }

    // Check device cards
    int cardIdx = HitTestCard(x, y);
    if (cardIdx < 0) return;

    HitZone zone = HitTestZone(cardIdx, x, y);
    auto idx = static_cast<size_t>(cardIdx);

    switch (zone) {
        case HitZone::Chevron:
            cards_[idx].expanded = !cards_[idx].expanded;
            RecalculateLayout();
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;

        case HitZone::SliderTrack: {
            draggingSlider_ = cardIdx;
            SetCapture(hwnd_);

            float slLeft, slRight, slCY;
            GetSliderBounds(cardIdx, slLeft, slRight, slCY);
            float newVol = std::clamp((x - slLeft) / (slRight - slLeft), 0.0f, 1.0f);
            cards_[idx].volume = newVol;

            if (volumeCtrl_ && cards_[idx].engineIndex < kMaxDevices) {
                volumeCtrl_->SetVolume(cards_[idx].engineIndex, newVol);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        }

        case HitZone::MuteButton:
            cards_[idx].muted = !cards_[idx].muted;
            if (volumeCtrl_ && cards_[idx].engineIndex < kMaxDevices) {
                volumeCtrl_->SetMute(cards_[idx].engineIndex, cards_[idx].muted);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;

        case HitZone::CardBody:
            cards_[idx].expanded = !cards_[idx].expanded;
            RecalculateLayout();
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;

        case HitZone::None:
            break;
    }
}

void MainWindow::OnLButtonUp(int /*mx*/, int /*my*/) {
    if (draggingSlider_ >= 0) {
        ReleaseCapture();
        draggingSlider_ = -1;
    }
}

void MainWindow::OnMouseMove(int mx, int my) {
    float x = static_cast<float>(mx) / dpiScale_;
    float y = static_cast<float>(my) / dpiScale_;

    int newHover = HitTestCard(x, y);
    int newBtn   = HitTestActionButton(x, y);

    if (newHover != hoveredCard_ || newBtn != hoveredButton_) {
        hoveredCard_   = newHover;
        hoveredButton_ = newBtn;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // Slider dragging
    if (draggingSlider_ >= 0 && draggingSlider_ < static_cast<int>(cards_.size())) {
        float slLeft, slRight, slCY;
        GetSliderBounds(draggingSlider_, slLeft, slRight, slCY);
        float newVol = std::clamp((x - slLeft) / (slRight - slLeft), 0.0f, 1.0f);

        auto idx = static_cast<size_t>(draggingSlider_);
        cards_[idx].volume = newVol;

        if (volumeCtrl_ && cards_[idx].engineIndex < kMaxDevices) {
            volumeCtrl_->SetVolume(cards_[idx].engineIndex, newVol);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::OnMouseWheel(short delta) {
    float scroll = -static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA) * Dip(40.0f);
    scrollOffset_ = std::clamp(scrollOffset_ + scroll, 0.0f, maxScroll_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ============================================================================
// Engine Message Handlers
// ============================================================================

void MainWindow::OnDeviceListReceived(DeviceListPayload* payload) {
    if (!payload) return;

    cards_.clear();
    activeDeviceCount_ = 0;
    totalDeviceCount_ = static_cast<uint32_t>(payload->devices.size());

    uint32_t engineIdx = 0;
    for (auto& ep : payload->devices) {
        DeviceCardState card;
        card.endpoint = std::move(ep);
        if (card.endpoint.isActive) {
            card.engineIndex = engineIdx;
            if (volumeCtrl_) {
                volumeCtrl_->BindDevice(engineIdx, card.endpoint.deviceId);
                card.volume = volumeCtrl_->GetVolume(engineIdx);
                card.muted = volumeCtrl_->IsMuted(engineIdx);
            }
            ++engineIdx;
            ++activeDeviceCount_;
        }
        cards_.push_back(std::move(card));
    }

    RecalculateLayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
    delete payload;
}

void MainWindow::OnDriftReportReceived(DriftReportPayload* payload) {
    if (!payload) return;

    for (uint32_t i = 0; i < payload->count; ++i) {
        const auto& report = payload->reports[i];
        for (auto& card : cards_) {
            if (card.engineIndex == report.deviceIndex) {
                card.lastReport = report;
                break;
            }
        }
    }

    InvalidateRect(hwnd_, nullptr, FALSE);
    delete payload;
}

void MainWindow::OnEngineStateChanged(WPARAM running) {
    engineRunning_ = (running != 0);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::OnDeviceChangeReceived(DeviceChangePayload* payload) {
    if (!payload) return;

    for (auto& card : cards_) {
        if (card.endpoint.deviceId == payload->deviceId) {
            card.endpoint.isActive = payload->added;
            if (!payload->added && card.engineIndex < kMaxDevices) {
                if (volumeCtrl_) {
                    volumeCtrl_->UnbindDevice(card.engineIndex);
                }
                card.engineIndex = UINT32_MAX;
                --activeDeviceCount_;
            }
            break;
        }
    }

    RecalculateLayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
    delete payload;
}

} // namespace msbt
