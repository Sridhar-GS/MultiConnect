// ============================================================================
// main.cpp -- MultiConnect Bluetooth Audio Engine
//
// Windows GUI entry point.  Creates the MainWindow (Direct2D GUI) which
// in turn launches the EngineOrchestrator on a background thread.
// All audio engine logic runs in the background; the main thread drives
// the Win32 message pump and Direct2D rendering.
// ============================================================================

#include "Common.h"
#include "Config.h"
#include "Logger.h"
#include "Gui.h"

#pragma warning(push)
#pragma warning(disable: 4365 4458 4514 4820 5039 5204)
#include <objidl.h>
#include <gdiplus.h>
#pragma warning(pop)

#include <cstdio>

using namespace msbt;

// RAII COM initializer
struct ComInit {
    ComInit()  { HrCheck(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx"); }
    ~ComInit() { CoUninitialize(); }
};

// RAII GDI+ initializer
struct GdiplusInit {
    ULONG_PTR token_ = 0;
    GdiplusInit() {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&token_, &input, nullptr);
    }
    ~GdiplusInit() {
        if (token_) Gdiplus::GdiplusShutdown(token_);
    }
};

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/,
                    LPWSTR /*lpCmdLine*/, int nCmdShow) {
    try {
        ComInit comInit;
        GdiplusInit gdipInit;

#ifdef _DEBUG
        // In debug builds, allocate a console for fprintf/OutputDebugString
        AllocConsole();
        FILE* dummy = nullptr;
        freopen_s(&dummy, "CONOUT$", "w", stderr);
#endif

        // Load optional configuration
        Config config;
        config.Load("config.ini");

        const auto logFile = config.GetString("log_file");
        if (!logFile.empty()) {
            Logger::Instance().SetLogFile(logFile.c_str());
        }
        if (config.GetBool("debug_logging", false)) {
            Logger::Instance().SetLevel(LogLevel::Debug);
        }

        // Register and create the main window
        if (!MainWindow::RegisterWindowClass(hInstance)) {
            OutputDebugStringA("[FATAL] Failed to register window class\n");
            return 1;
        }

        auto* window = MainWindow::Create(hInstance, nCmdShow);
        if (!window) {
            OutputDebugStringA("[FATAL] Failed to create main window\n");
            return 1;
        }

        // Run the GUI message loop (blocks until window closes)
        int result = window->RunMessageLoop();

        delete window;
        return result;

    } catch (const std::exception& e) {
        char msg[512];
        snprintf(msg, sizeof(msg), "[FATAL] Unhandled exception: %s\n", e.what());
        OutputDebugStringA(msg);
        MessageBoxA(nullptr, e.what(), "MultiConnect - Fatal Error", MB_ICONERROR);
        return 1;
    } catch (...) {
        OutputDebugStringA("[FATAL] Unknown exception\n");
        MessageBoxA(nullptr, "Unknown fatal error", "MultiConnect", MB_ICONERROR);
        return 1;
    }
}
