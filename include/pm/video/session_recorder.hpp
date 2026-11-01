#pragma once

#if defined(_WIN32) && !defined(PM_HAS_AUDIO_IO)
#define PM_HAS_AUDIO_IO 1
#endif

#include <cstdint>
#include <filesystem>
#include <string>

#include "pm/video/recording.hpp"
#include "pm/video/key_overlay.hpp"
#include "pm/video/creator_fx.hpp"

#if defined(_WIN32) && defined(FEATURE_SESSION_VIDEO_RECORDER)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "pm/video/win_capture_target.hpp"
#endif

#if PM_HAS_AUDIO_IO
#include "pm/audio/mixed_capture.hpp"
#endif

namespace media::win::recorder {

#if PM_HAS_AUDIO_IO
using SessionVideoAudioConfig = pm::audio::MixedCaptureConfig;
#endif

/// Window-relative sub-region (picker `rrect=l,t,w,h`) applied as a GPU crop on the
/// WGC texture before encode — OBS-style full capture + crop, not a GDI BitBlt re-grab.
struct CaptureCrop {
    int left   = 0;
    int top    = 0;
    int width  = 0;
    int height = 0;

    bool valid() const noexcept { return width > 0 && height > 0; }
};

struct VideoTransformConfig {
    int width = 0;
    int height = 0;
    std::string size_mode{"native"}; // native | pad | fit | fill
    /// Width÷height for the zoom viewport crop. 0 = derive from width/height /
    /// source only. When > 0, interactive zoom and follow keep this ratio.
    double aspect_ratio = 0.0;
    double zoom = 1.0;
    std::string follow{"none"};      // none | anchor | cursor
    double follow_speed = 0.2;
    int follow_deadzone = 3;
    int transition_ms = 300;
    int update_interval_ms = 16;
    double focus_x = -1.0;           // normalized [0,1], negative = anchor
    double focus_y = -1.0;
    bool interactive_zoom = false;
    double zoom_step = 0.15;
    double zoom_min = 1.0;
    double zoom_max = 4.0;
    bool consume_zoom_input = true;
    bool show_zoom_status = true;
    std::string hud_capture = "auto";  // auto/exclude or visible
    /// Status pill for the take: off | overlay | taskbar | both. Empty = off.
    std::string status_hud = "off";
    std::string pause_key;             // shortcut syntax, e.g. F9
    std::string stop_key;              // shortcut syntax, e.g. F10
    pm::video::KeyOverlayConfig key_overlay{};
    // Creator FX (docs/video/video-next.md) — cursor + camera PiP/mic live; audio polish reserved.
    pm::video::CursorFxConfig cursor_fx{};
    pm::video::CameraOverlayConfig camera_overlay{};
    pm::video::AudioPolishConfig audio_polish{};
    pm::video::IdlePauseConfig idle_pause{};
    pm::video::CaptionConfig captions{};
};

struct VideoTransformUpdate {
    double zoom = -1.0;              // negative = unchanged
    std::string follow;              // empty = unchanged
    double follow_speed = -1.0;
    int follow_deadzone = -1;
    int transition_ms = -1;
    double focus_x = -1.0;
    double focus_y = -1.0;
};

#if defined(_WIN32) && defined(FEATURE_SESSION_VIDEO_RECORDER)
/// H.264 MP4 of a monitor/window via Windows.Graphics.Capture + Media Foundation.
/// The target overload supports HMONITOR and HWND; the HWND overload remains for UI callers.
/// @p out_mp4 should end in `.mp4` (e.g. `%USERPROFILE%\\Videos\\session-143055.mp4` from the UI default).
/// @p crop optional window-relative region; when set, WGC still captures the full HWND and
///     CopySubresourceRegion crops before H.264 encode.
/// @return true on success (call `stop()` to finalize the file).
bool start(const pm::video::WinCaptureTarget& target,
           const std::filesystem::path& out_mp4,
           std::string& err_utf8,
           bool include_cursor = true,
           const CaptureCrop* crop = nullptr,
           int fps = 30,
           int bitrate_kbps = 0,
           const std::string& encoder = "auto",
           const VideoTransformConfig* transform = nullptr
#if PM_HAS_AUDIO_IO
           , const SessionVideoAudioConfig* audio = nullptr
#endif
           );

bool start(HWND main_window,
           const std::filesystem::path& out_mp4,
           std::string& err_utf8,
           bool include_cursor = true,
           const CaptureCrop* crop = nullptr,
           int fps = 30,
           int bitrate_kbps = 0,
           const std::string& encoder = "auto",
           const VideoTransformConfig* transform = nullptr
#if PM_HAS_AUDIO_IO
           , const SessionVideoAudioConfig* audio = nullptr
#endif
           );

/// Stop capture and finalize the MP4 (safe to call if not recording).
void stop() noexcept;

bool is_recording() noexcept;

/// Current/effective backend, encoder, counters, and last asynchronous error.
pm::video::RecordingStatus status() noexcept;

bool update(const VideoTransformUpdate& update, std::string& err_utf8) noexcept;

/// While recording: toggle pause. Paused video/audio time is omitted from the
/// MP4 timeline. No-op if not recording.
void toggle_pause() noexcept;

bool is_paused() noexcept;
#else
inline bool start(void*, const std::filesystem::path&, std::string& err, bool = true,
                  const CaptureCrop* = nullptr, int = 30, int = 0,
                  const std::string& = "auto",
                  const VideoTransformConfig* = nullptr)
{
    err = "session video recorder not available on this build.";
    return false;
}
inline void stop() {}
inline bool is_recording() { return false; }
inline pm::video::RecordingStatus status() noexcept { return {}; }
inline bool update(const VideoTransformUpdate&, std::string& err) noexcept
{
    err = "session video recorder not available on this build.";
    return false;
}
inline void toggle_pause() noexcept {}
inline bool is_paused() noexcept { return false; }
#endif

} // namespace media::win::recorder
