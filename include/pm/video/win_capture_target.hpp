#pragma once

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#if defined(FEATURE_VIDEO) && FEATURE_VIDEO
#include "pm/video/screen.hpp"
#endif

#include <string>

namespace pm::video {

struct WinCaptureTarget {
    HWND window = nullptr;
    HMONITOR monitor = nullptr;
    bool has_crop = false;
    int crop_left = 0;
    int crop_top = 0;
    int crop_width = 0;
    int crop_height = 0;

    bool valid() const noexcept { return window || monitor; }
    bool is_monitor() const noexcept { return monitor != nullptr; }
};

inline bool resolve_wgc_capture_target(const std::string& input,
                                       WinCaptureTarget& target,
                                       std::string& error)
{
#if defined(FEATURE_VIDEO) && FEATURE_VIDEO
    target = {};
    error.clear();
    ScreenSpec spec;
    if (!parse_screen_spec(input, spec)) {
        error = "fast capture needs a screen/window input";
        return false;
    }
    if (spec.has_rect) {
        error = "fixed rect= capture requires the compatibility backend; "
                "use a monitor or window-relative rrect= for fast capture";
        return false;
    }

    HWND win = resolve_screen_window(spec);
    const bool wanted_window = spec.hwnd != 0 || !spec.wintitle_glob.empty();
    if (win && screen_spec_promotes_desktop_hwnd(spec, win)) {
        target.monitor = resolve_screen_monitor(resolve_effective_monitor_index(spec));
    } else if (win) {
        target.window = win;
    } else if (!wanted_window) {
        target.monitor = resolve_screen_monitor(resolve_effective_monitor_index(spec));
    }
    if (!target.valid()) {
        error = spec.hwnd != 0 || !spec.wintitle_glob.empty()
            ? "fast capture could not resolve the requested window"
            : "fast capture could not resolve the requested monitor";
        return false;
    }
    if (target.window && spec.has_rrect && spec.rr_w > 0 && spec.rr_h > 0) {
        target.has_crop = true;
        target.crop_left = spec.rr_l;
        target.crop_top = spec.rr_t;
        target.crop_width = spec.rr_w;
        target.crop_height = spec.rr_h;
    }
    return true;
#else
    (void)input;
    target = {};
    error = "screen/window target resolution is unavailable in this build";
    return false;
#endif
}

} // namespace pm::video

#endif
