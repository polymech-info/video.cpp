#pragma once

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <vector>

#ifndef WDA_NONE
#  define WDA_NONE 0x00000000
#endif
#ifndef WDA_EXCLUDEFROMCAPTURE
#  define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace media::win {

/// True while a capture asked for overlay visibility (hudCapture=visible).
bool overlays_visible_to_capture() noexcept;

/// Picker/highlight/status: apply the current capture affinity. Idle and
/// hudCapture=visible = WDA_NONE (OBS / other-process recorders / Snap see
/// them). Our own auto/exclude takes set WDA_EXCLUDEFROMCAPTURE for the scope.
void exclude_window_from_capture(HWND hwnd) noexcept;

/// Block status pills (overlay HUD, transcribe HUD). Idle = capture-visible
/// so Windows Snap / Snipping Tool can see them. Our screenshot/record scopes
/// exclude them unless hudCapture=visible.
void register_status_overlay_hwnd(HWND hwnd) noexcept;
void unregister_status_overlay_hwnd(HWND hwnd) noexcept;

using LivePickerCaptureHwndsFn = std::vector<HWND> (*)();
void set_live_picker_capture_hwnds_fn(LivePickerCaptureHwndsFn fn) noexcept;

/// Briefly clear WDA_EXCLUDEFROMCAPTURE on the given windows (picker HUD,
/// highlight). Restores exclusion on destroy.
class IncludeFromCaptureScope {
public:
    explicit IncludeFromCaptureScope(const std::vector<HWND>& src);
    ~IncludeFromCaptureScope();
    IncludeFromCaptureScope(const IncludeFromCaptureScope&) = delete;
    IncludeFromCaptureScope& operator=(const IncludeFromCaptureScope&) = delete;

private:
    std::vector<HWND> hwnds;
};

/// Capture session: always arms. `enable` includes picker + status HUDs in
/// WGC/BitBlt (hudCapture=visible). Otherwise status HUDs are excluded for
/// the take, then restored so Snap can see them again.
class PickerHudCaptureScope {
public:
    explicit PickerHudCaptureScope(bool enable);
    ~PickerHudCaptureScope();
    PickerHudCaptureScope(const PickerHudCaptureScope&) = delete;
    PickerHudCaptureScope& operator=(const PickerHudCaptureScope&) = delete;

private:
    bool include_ = false;
};

} // namespace media::win

#endif // _WIN32
