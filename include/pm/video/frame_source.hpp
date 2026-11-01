#pragma once
//
// frame_source.hpp — Abstract frame source interface.
//
// Anything that produces VideoFrame objects implements IFrameSource.
// The threading model mirrors VideoInput: start() spawns an internal thread,
// the callback is called from that thread (keep it non-blocking), stop() joins.
//
// Current implementations:
//   WebcamSource  — wraps VideoInput (physical webcam, Media Foundation / V4L2 / AVFoundation)
//   ScreenSource  — GDI BitBlt / WGC screen/window capture (Win32 only)
//
// Future sinks (Sunshine-inspired):
//   DetectSink    — YOLO inference → NDJSON
//   RecordSink    — encode frames → AVI/MP4
//   DisplaySink   — render to window overlay
//   StreamSink    — RTP/WebRTC packetise (Sunshine-style)
//
// Factory:
//   make_frame_source("NexiGo")          → WebcamSource (name substring)
//   make_frame_source("<device id>")     → WebcamSource (symbolic link, /dev/videoN, uniqueID)
//   make_frame_source("screen:0")        → ScreenSource, monitor 0
//   make_frame_source("screen:0:wintitle=pm-image*") → ScreenSource, window glob
//   make_frame_source("")                → WebcamSource, first device
//
// Usage:
//   auto src = pm::video::make_frame_source(st.vdet_input);
//   src->start(width, height, fps, [](const VideoFrame& f){ ... });
//   ...
//   src->stop();

#if defined(FEATURE_VIDEO) && FEATURE_VIDEO

#include "pm/video/capture.hpp"
#include <functional>
#include <memory>
#include <string>

namespace pm::video {

// ── Abstract source ───────────────────────────────────────────────────────────

struct IFrameSource {
    using Callback = std::function<void(const VideoFrame&)>;

    virtual ~IFrameSource() = default;

    /// Start streaming. Spawns internal thread; callback fires on that thread.
    /// @param width/height  Preferred resolution (0 = source default).
    /// @param fps           Preferred rate (0 = source default).
    /// @throws std::runtime_error on device open failure.
    virtual void start(int width, int height, int fps, Callback cb) = 0;

    /// Stop streaming. Blocks until internal thread exits. Safe to call anytime.
    virtual void stop() noexcept = 0;

    virtual bool is_running() const noexcept = 0;

    /// Human-readable description populated after start()
    /// (e.g. "NexiGo N930E 1920x1080 @ 30fps" or "screen:0 1920x1080 @ 10fps").
    virtual std::string description() const = 0;
};

using FrameSourcePtr = std::unique_ptr<IFrameSource>;

// ── Factory ───────────────────────────────────────────────────────────────────

/// Parse an --input spec and return the matching IFrameSource implementation.
///
/// Screen specs (Win32 only, requires FEATURE_VIDEO):
///   "screen" / "screen:current"    — monitor containing the originating host HWND
///   "screen:0"                     — full desktop / primary monitor
///   "screen:1"                     — second monitor
///   "screen:0:wintitle=Notepad*"   — window whose title matches glob (case-insensitive)
///   "screen:0:wintitle*"           — shorthand: no key= prefix
///
/// Webcam specs (anything else):
///   ""                             — first/default device
///   "NexiGo"                       — case-insensitive device name substring
///
FrameSourcePtr make_frame_source(const std::string& input_spec);

/// Returns true if input_spec is a screen capture spec ("screen", "screen:…").
/// Win32: implemented in screen_input.cpp; non-Win32: inline stub (no screen capture support).
#if defined(_WIN32)
bool is_screen_input_spec(const std::string& spec) noexcept;
#else
inline bool is_screen_input_spec(const std::string& spec) noexcept {
    return spec == "screen" ||
           (spec.size() >= 7 && spec.substr(0, 7) == "screen:");
}
#endif

// ── WebcamSource ──────────────────────────────────────────────────────────────
// Thin adapter that wraps the existing VideoInput under the IFrameSource API.
// No logic changes — just delegation.

class WebcamSource final : public IFrameSource {
public:
    explicit WebcamSource(const std::string& device_name = "");

    void   start(int w, int h, int fps, Callback cb) override;
    void   stop() noexcept override;
    bool   is_running() const noexcept override;
    std::string description() const override;

private:
    std::string   device_name_;
    VideoInput    vin_;
    std::string   desc_;
};

} // namespace pm::video

#endif // FEATURE_VIDEO
