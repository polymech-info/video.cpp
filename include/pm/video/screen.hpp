#pragma once
//
// screen_input.hpp — Win32 screen / window capture as a VideoFrame source.
//
// Implements IFrameSource for display/window capture.
// Backend: GDI BitBlt for monitor/rect capture; PrintWindow(PW_RENDERFULLCONTENT)
// for hwnd=/wintitle= targets (with rrect= crop), BitBlt fallback if that fails.
//
// Upgrade path (noted for later):
//   - DXGI Desktop Duplication (D3D11) — GPU-accelerated, no DWM round-trip,
//     lower latency, supports HDR. Best for full-monitor capture.
//   - Windows.Graphics.Capture (WGC) — per-window capture with DWM awareness,
//     available Windows 10 1803+. Best for app-window scenarios (app_use.cpp).
//   Both would be drop-in replacements for the GDI backend once added.
//
// Input spec format (parsed by make_frame_source / ScreenInput::parse_spec):
//
//   screen / screen:current         → monitor containing the originating host HWND
//                                     (UI frame that launched the run, or PM_IMAGE_HOST_HWND)
//   screen:0                        → primary monitor (or monitor index 0)
//   screen:1                        → second monitor
//   screen:0:wintitle=pm-image*     → window with title matching glob
//   screen:0:wintitle*              → shorthand (no key= required)
//   screen:0:hwnd=1517682           → capture a specific window by HWND (decimal)
//   screen:0:rect=2234,337,346,615  → capture a fixed screen rectangle (l,t,w,h)
//   screen:0:hwnd=1517682:rrect=12,80,346,615
//                                   → window-relative sub-rect (follows the window)
//   screen:0:wintitle=Chrome*:rrect=12,80,346,615
//                                   → same, resolving the window by title glob
//   screen:0:layer=monitor          → display composition (open apps included)
//   screen:0:hwnd=…:layer=desktop   → wallpaper only (PrintWindow of Progman/WorkerW)
//   screen:0:pid=1234               → future: capture by process ID
//
// hwnd= / rect= / rrect= are produced by `assistant pick`:
//   - rect=  : precise "coords fallback" — a FIXED screen region (e.g. a sub-window
//              UIA element like a video player). Does NOT track window moves.
//   - hwnd=  : track a specific window's rect (follows moves/resizes).
//   - rrect= : a sub-rectangle expressed RELATIVE to the resolved window's
//              top-left (from hwnd= or wintitle=). Each frame the absolute rect is
//              recomputed from the live window rect, so it FOLLOWS window moves —
//              the best of both: element-precise *and* movement-tracking.
//
// Glob matching:  case-insensitive, * matches any substring, ? matches one char.
//
// Threading:  start() spawns one thread that loops at the requested FPS,
//             BitBlts the target rect, fills VideoFrame{BGR24}, fires callback.
//             stop() signals and joins the thread.

#if defined(_WIN32) && defined(FEATURE_VIDEO) && FEATURE_VIDEO

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>  // RECT (resolve_screen_rect)

#include "pm/video/frame_source.hpp"
#include <memory>
#include <string>

namespace pm::video {

// ── Parsed screen capture spec ────────────────────────────────────────────────

struct ScreenSpec {
    int           monitor_idx = 0;      ///< 0-based monitor index
    bool          use_current = false;  ///< screen:current — monitor of originating HWND
    std::string   wintitle_glob;        ///< empty = full monitor/desktop
    unsigned long long hwnd = 0;        ///< explicit HWND (decimal); 0 = unset
    bool          has_rect = false;     ///< true when an explicit rect= was given
    int           rect_l = 0, rect_t = 0, rect_w = 0, rect_h = 0; ///< rect= region (absolute)
    bool          has_rrect = false;    ///< true when a window-relative rrect= was given
    int           rr_l = 0, rr_t = 0, rr_w = 0, rr_h = 0; ///< rrect= offset from window TL
    bool          include_cursor = true; ///< draw mouse cursor into GDI capture
    /// How a desktop-shell HWND (Progman/WorkerW) should be captured.
    /// Auto promotes wallpaper HWNDs to the monitor (open apps included).
    /// Monitor = display composition. Desktop = PrintWindow of the shell (wallpaper).
    enum class CaptureLayer { Auto, Monitor, Desktop };
    CaptureLayer  layer = CaptureLayer::Auto;
    // future: int pid; std::string process_name;
};

/// Parse "screen:[idx|current][:wintitle=GLOB]" into ScreenSpec.
/// Also accepts bare "screen" / "screen:" as aliases for screen:current.
/// Returns false if the string is not a screen spec.
bool parse_screen_spec(const std::string& input_spec, ScreenSpec& out) noexcept;

/// Case-insensitive glob match (`*`, `?`) of a window title against a wintitle
/// glob — the *same* matcher used to resolve the capture target. Callers
/// positioning an overlay over the captured window must use this (not an ad-hoc
/// prefix scan) so a leading-'*' glob like "*Chrome*" picks the same window that
/// is actually captured.
bool wintitle_glob_match(const std::string& title, const std::string& glob) noexcept;

/// Resolve the explicit hwnd= or wintitle= portion of a screen spec.
/// Returns nullptr for monitor-only/rect-only specs or when the window is gone.
HWND resolve_screen_window(const ScreenSpec& spec) noexcept;

/// Pin the host HWND used by screen:current (typically the UI root that launched
/// the capture). Also readable from env `PM_IMAGE_HOST_HWND` when unset.
void set_originating_capture_hwnd(HWND hwnd) noexcept;
HWND originating_capture_hwnd() noexcept;

/// Desktop-shell hwnd + Auto/Monitor layer → capture the display (apps included)
/// instead of PrintWindow of Progman/WorkerW (wallpaper only).
bool screen_spec_promotes_desktop_hwnd(const ScreenSpec& spec, HWND hwnd) noexcept;

/// Resolve screen:current / use_current to a concrete 0-based monitor index.
/// Falls back to 0 when no originating HWND can be resolved.
int resolve_effective_monitor_index(const ScreenSpec& spec) noexcept;

/// Resolve a monitor index using the same clamping/order as screen capture.
HMONITOR resolve_screen_monitor(int monitor_idx) noexcept;

/// Resolve a parsed ScreenSpec to the absolute screen rectangle that will be
/// captured *right now* — the single source of truth shared by ScreenSource,
/// `assistant pick`, and every visualize/overlay path so the overlay always
/// lands exactly on the captured pixels. Resolution order matches the capture
/// loop: rect= (fixed) → hwnd=/wintitle= [+ rrect= sub-region] → monitor.
/// Returns an empty rect ({0,0,0,0}) only if nothing could be resolved (e.g. a
/// window glob with no match and no monitors). Callers that care about
/// physical-pixel alignment should be per-monitor DPI aware first.
RECT resolve_screen_rect(const ScreenSpec& spec) noexcept;

// ── ScreenSource ──────────────────────────────────────────────────────────────

class ScreenSource final : public IFrameSource {
public:
    explicit ScreenSource(ScreenSpec spec);

    /// Alternate ctor: parse spec inline.
    explicit ScreenSource(const std::string& input_spec);

    ~ScreenSource() override;

    void   start(int width, int height, int fps, Callback cb) override;
    void   stop() noexcept override;
    bool   is_running() const noexcept override;
    std::string description() const override;

    // ── Enumeration helpers ───────────────────────────────────────────────────

    /// List all top-level visible windows: [{hwnd, title, pid}] for tooling /
    /// `video info --screens` output.
    struct WindowInfo {
        void*       hwnd  = nullptr;  ///< HWND (opaque for the public API)
        std::string title;
        int         pid   = 0;
        int         x = 0, y = 0, w = 0, h = 0;
    };
    static std::vector<WindowInfo> enumerate_windows();

    /// List physical display monitors: [{index, name, x,y,w,h, is_primary}].
    struct MonitorInfo {
        int         index      = 0;
        std::string name;           ///< e.g. "\\\\.\\DISPLAY1"
        int         x = 0, y = 0, w = 0, h = 0;
        bool        is_primary = false;
    };
    static std::vector<MonitorInfo> enumerate_monitors();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ── Single-shot screen capture ────────────────────────────────────────────────

/// Capture one frame from a screen or window described by @p input_spec
/// (same format as ScreenSource — "screen:0", "screen:0:wintitle*", …).
///
/// This is a one-shot GDI BitBlt with no background thread; faster than
/// spinning up a full ScreenSource for a single still image.
///
/// Returns a BGR24 VideoFrame on success.  On failure @p err is set and the
/// returned frame has width == 0.
///
/// @p include_layered controls the CAPTUREBLT flag: true (default) pulls in
/// layered/transparent topmost windows for a faithful screenshot; pass false
/// when a live overlay (DetectOverlay/HighlightOverlay) sits over the captured
/// region so its drawing isn't captured and re-painted (double-paint), which
/// WDA_EXCLUDEFROMCAPTURE only prevents on Windows 10 2004+.
///
/// @p screen_composition skips PrintWindow and BitBlts the screen (needed to
/// include a live picker HUD after PickerHudCaptureScope clears affinity).
VideoFrame capture_still_screen(const std::string& input_spec, std::string& err,
                                bool include_layered = true,
                                bool screen_composition = false);

} // namespace pm::video

#endif // _WIN32 && FEATURE_VIDEO
