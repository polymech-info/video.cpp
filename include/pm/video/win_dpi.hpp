#pragma once
//
// dpi_thread.hpp — RAII PerMonitorV2 for Win32 geometry / capture calls.
//
// WGC textures and BitBlt screen DCs are always in physical pixels. Callers that
// are DPI-unaware (CLI hosts without a PerMonitorV2 manifest, or free-threaded
// WGC callbacks) see GetWindowRect / GetCursorPos / GetMonitorInfo in logical
// pixels — typically half size at 200% DPI — which undersizes the encoder and
// makes recordings look pixelated when played back.

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace media::win {

struct ThreadDpiPerMonitorScope {
    using SetCtx_t = DPI_AWARENESS_CONTEXT (WINAPI*)(DPI_AWARENESS_CONTEXT);

    SetCtx_t              fn     = nullptr;
    DPI_AWARENESS_CONTEXT prev   = nullptr;
    bool                  active = false;

    ThreadDpiPerMonitorScope() noexcept
    {
        if (HMODULE h = ::GetModuleHandleW(L"user32.dll")) {
            fn = reinterpret_cast<SetCtx_t>(
                ::GetProcAddress(h, "SetThreadDpiAwarenessContext"));
        }
        if (!fn)
            return;
        prev = fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        if (!prev)
            prev = fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
        active = (prev != nullptr);
    }

    ~ThreadDpiPerMonitorScope()
    {
        if (active && fn)
            fn(prev);
    }

    ThreadDpiPerMonitorScope(const ThreadDpiPerMonitorScope&)            = delete;
    ThreadDpiPerMonitorScope& operator=(const ThreadDpiPerMonitorScope&) = delete;
};

/// Effective DPI for the monitor containing @p screen_rect (96 if unknown).
inline UINT monitor_dpi_for_rect(const RECT& screen_rect) noexcept
{
    using GetDpiForMonitor_t = HRESULT (WINAPI*)(HMONITOR, int, UINT*, UINT*);
    HMONITOR mon =
        ::MonitorFromRect(&screen_rect, MONITOR_DEFAULTTONEAREST);
    if (!mon)
        return 96;
    HMODULE shcore = ::GetModuleHandleW(L"Shcore.dll");
    if (!shcore)
        shcore = ::LoadLibraryW(L"Shcore.dll");
    if (!shcore)
        return 96;
    auto get_dpi = reinterpret_cast<GetDpiForMonitor_t>(
        ::GetProcAddress(shcore, "GetDpiForMonitor"));
    UINT x = 96, y = 96;
    // MDT_EFFECTIVE_DPI = 0
    if (get_dpi && SUCCEEDED(get_dpi(mon, 0, &x, &y)) && x > 0)
        return x;
    return 96;
}

inline UINT monitor_dpi_for_point(POINT pt) noexcept
{
    RECT r{pt.x, pt.y, pt.x + 1, pt.y + 1};
    return monitor_dpi_for_rect(r);
}

inline int system_metric_for_dpi(int index, UINT dpi) noexcept
{
    using Fn = int (WINAPI*)(int, UINT);
    static Fn fn = []() -> Fn {
        if (HMODULE h = ::GetModuleHandleW(L"user32.dll")) {
            return reinterpret_cast<Fn>(
                ::GetProcAddress(h, "GetSystemMetricsForDpi"));
        }
        return nullptr;
    }();
    if (fn && dpi > 0)
        return fn(index, dpi);
    return ::GetSystemMetrics(index);
}

inline int scale_px_for_dpi(int px_at_96, UINT dpi) noexcept
{
    if (px_at_96 <= 0)
        return px_at_96;
    return MulDiv(px_at_96, static_cast<int>(dpi > 0 ? dpi : 96), 96);
}

} // namespace media::win
