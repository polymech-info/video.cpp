#pragma once
// frame_pump.hpp — pull-based frame producer for the videoSource "flat pipe".
//
// A FramePump is the streaming generalization of capture_still / load_frame: it
// yields VideoFrame objects one at a time via next() until the stream ends. The
// xblox `videoSource` block drives one of these as a frame pump — for every frame
// it caches the buffer and runs its child items (filters / writer / detect), so a
// document needs no for/while loop and no storeAs to transcode a clip.
//
// Three source kinds, one interface (so downstream blocks never branch on type):
//   • video file  ("clip.mp4", .mov/.mkv/.avi/.webm…) — decode to EOF   (finite)
//   • still image ("frame.png", .jpg/.bmp…)           — one frame, EOF  (finite)
//   • live source ("" / "NexiGo" webcam, "screen:0")  — until stop      (open)
//
// "stream.end()" is therefore EOF for files/images, and a stop condition for live
// sources: max_frames, duration_ms, or the cancel predicate (e.g. Ctrl+C / xblox
// cancellation). next() blocks until a frame is ready or the stream has ended.
//
// Backend: Win32 uses Media Foundation (video files) + the existing
// make_frame_source() (webcam/screen) + load_frame() (images). Other platforms
// get a stub until a non-MF decoder is wired in. No OpenCV.

#if defined(FEATURE_VIDEO) && FEATURE_VIDEO

#include "pm/video/capture.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <memory>
#include <string>
#include "pm/video/text.hpp"

namespace pm::video {

// ── Stop / negotiation knobs ────────────────────────────────────────────────────
struct FramePumpOptions {
    int width  = 0;   // preferred capture width  (live hint; 0 = source default)
    int height = 0;   // preferred capture height (live hint; 0 = source default)
    int fps    = 0;   // preferred rate           (live hint; 0 = source default)
    bool include_cursor = true; // screen/window capture only

    // Stop conditions. 0 = unbounded → files/images end at EOF; live sources run
    // until the cancel predicate fires.
    long long max_frames  = 0;   // stop after N frames (any source)
    long long duration_ms = 0;   // stop after wall-clock budget (live sources)

    // Polled between frames; returning true ends the stream cleanly (no error).
    std::function<bool()> cancel;

    // Max ms to wait for the *first* frame of a live source before failing open().
    int open_timeout_ms = 5000;
};

// ── Pull-based source ───────────────────────────────────────────────────────────
class IFramePump {
public:
    virtual ~IFramePump() = default;

    // Block for the next frame.
    //   returns true  → `out` holds a frame, keep pumping.
    //   returns false → stream ended. `err` empty = clean end (EOF / stop);
    //                   `err` non-empty = a decode/capture error.
    virtual bool next(VideoFrame& out, std::string& err) = 0;

    // Human-readable source description (filled after open).
    virtual std::string describe() const = 0;

    // Frames delivered so far.
    virtual long long frames_read() const = 0;

    // Nominal source frame rate (fps), if known (video files: container metadata;
    // live: requested rate). 0 = unknown. Used to transcode at the right speed.
    virtual double source_fps() const { return 0.0; }
};

// True if `spec` names a decodable video container (matched by extension). Still
// images and device/screen specs return false. Pure + header-inline so the xblox
// block and the backend agree on routing without a platform TU.
inline bool is_video_file_spec(const std::string& spec) noexcept {
    const auto dot = spec.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= spec.size()) return false;
    std::string ext = spec.substr(dot + 1);
    ext = pm::video::lower_ascii(std::move(ext));
    static constexpr const char* kVideoExt[] = {
        "mp4", "m4v", "mov", "mkv", "webm", "avi", "wmv",
        "flv", "mpg", "mpeg", "m2ts", "mts", "ts", "3gp", "ogv",
    };
    for (const char* e : kVideoExt)
        if (ext == e) return true;
    return false;
}

// Build a pump for an input spec, opened and ready (first frame negotiated):
//   "<path>.mp4|mov|…"  → video-file decoder        (Win32: Media Foundation)
//   "<path>.png|jpg|…"  → single-frame image source (load_frame)
//   "screen:…"          → screen / window capture   (Win32)
//   "" / device name    → webcam capture
// Returns nullptr + sets `err` if the source cannot be opened.
std::unique_ptr<IFramePump> make_frame_pump(const std::string& spec,
                                            const FramePumpOptions& opts,
                                            std::string& err);

} // namespace pm::video

#endif // FEATURE_VIDEO
