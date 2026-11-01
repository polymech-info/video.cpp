#pragma once

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "pm/video/creator_fx.hpp"

#include <memory>
#include <string>

namespace pm::video {

/// Live capture-visible webcam PiP (slice 2). Parallel MF capture + layered
/// HWND; intentionally not WDA_EXCLUDEFROMCAPTURE so WGC screen recordings
/// include the overlay. Post-export burn-in (Recordly-style) is a later path.
class WinCameraOverlayController {
public:
    WinCameraOverlayController();
    ~WinCameraOverlayController();

    WinCameraOverlayController(const WinCameraOverlayController&) = delete;
    WinCameraOverlayController& operator=(const WinCameraOverlayController&) = delete;

    /// Start webcam + overlay. `anchor_screen` is the capture monitor/window
    /// bounds used for corner placement. Returns false on failure.
    bool start(const CameraOverlayConfig& config,
               const RECT& anchor_screen,
               std::string* error = nullptr);

    /// Update placement when the capture target moves/resizes.
    void update_anchor(const RECT& anchor_screen);

    void stop() noexcept;

    bool running() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pm::video

#endif
