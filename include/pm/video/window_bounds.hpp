#pragma once
//
// window_bounds.hpp — DWM-aware visible window rectangles for pick/highlight/capture.
//
// GetWindowRect includes a few pixels of invisible extended frame on composited
// windows. DWMWA_EXTENDED_FRAME_BOUNDS matches what the user sees (and what
// WGC/BitBlt should frame). Use this for highlight overlays, picker specs, and
// screen capture — not for coarse WindowFromPoint hit-testing.

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace media::win {

/// Visible on-screen bounds of @p hwnd in physical pixels (forces PerMonitorV2
/// for the call so DPI-unaware threads do not get half-size logical rects at
/// 200% scaling). Returns false when @p hwnd is invalid or both DWM and
/// GetWindowRect fail.
bool visible_window_bounds(HWND hwnd, RECT& out) noexcept;

/// Progman / WorkerW / desktop list-view — PrintWindow of these is wallpaper
/// only. Full-monitor BitBlt / WGC CreateForMonitor includes open apps.
bool hwnd_is_desktop_shell(HWND hwnd) noexcept;

} // namespace media::win
