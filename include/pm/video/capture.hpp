#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "pm/video/frame.hpp"

namespace pm::video {

// ── Device enumeration ────────────────────────────────────────────────────────

struct DeviceInfo {
    std::string id;          // opaque symbolic-link string (Win32 MF activation)
    std::string name;        // human-readable friendly name
    bool        is_default = false;
};

// Returns all video capture devices visible to the OS.
// Empty on failure (no camera, driver error, or unsupported platform).
std::vector<DeviceInfo> enumerate_capture_devices();

// ── Device mode enumeration ───────────────────────────────────────────────────

struct DeviceMode {
    int         width    = 0;
    int         height   = 0;
    int         fps_num  = 0;  // frame-rate numerator   (e.g. 30)
    int         fps_den  = 1;  // frame-rate denominator (e.g. 1)
    std::string format;        // "MJPEG", "NV12", "YUY2", "RGB24", …
};

// Return all native media types advertised by the named capture device.
// device_name: exact DeviceInfo::id, or a case-insensitive substring of the
// friendly name; empty = first device.
// Empty result on failure (device not found, no types, driver error).
std::vector<DeviceMode> enumerate_device_modes(const std::string& device_name = "");

// Larger area, then higher fps, then format name. Same order as `video info --modes`.
void sort_device_modes(std::vector<DeviceMode>& modes);
std::vector<DeviceMode> sorted_device_modes(const std::string& device_name);

// mode_idx < 0 leaves width, height, and fps unchanged.
// On range failure, err is the `video info --modes` message and the outs are unchanged.
bool resolve_device_mode(const std::string& device_name,
                         int mode_idx,
                         int& width,
                         int& height,
                         int& fps,
                         std::string& err);

// Empty needle matches. Otherwise a case-insensitive substring of the friendly name.
bool capture_device_present(const std::string& needle);

// Friendly name of the camera `camera_input` names. Empty input prefers the default.
std::string friendly_capture_name(const std::string& camera_input);

// Frame model (PixelFormat, VideoFrame) lives in pm-video.

// ── VideoInput ────────────────────────────────────────────────────────────────
// Streams frames from a webcam/capture device.
// Win32: Media Foundation IMFSourceReader.
// Linux: V4L2 mmap streaming (video_linux.cpp).
// macOS: AVFoundation (video_osx.mm).
//
// Threading:
//   - start() spawns an internal capture thread.
//   - on_frame() is called from that thread — do not block it.
//   - stop() joins the thread; safe to call from any thread.
//
class VideoInput {
public:
    VideoInput();
    ~VideoInput();

    VideoInput(const VideoInput&)            = delete;
    VideoInput& operator=(const VideoInput&) = delete;

    // Begin capture.
    //   device_name  — exact DeviceInfo::id (symbolic link), or a case-insensitive
    //                  substring of DeviceInfo::name. Empty → first / default device.
    //   width/height — preferred resolution; 0 = device chooses.
    //   fps          — preferred frame rate; 0 = device chooses.
    //   on_frame     — frame callback. Frames are BGR24 or MJPEG depending on
    //                  what the device negotiates (check VideoFrame::format).
    //
    // Throws std::runtime_error if the device cannot be opened.
    void start(const std::string&                         device_name,
               int                                        width,
               int                                        height,
               int                                        fps,
               std::function<void(const VideoFrame&)>    on_frame);

    // Stop capture and block until the internal thread exits.
    void stop();

    bool is_running() const;

    // Name of the opened device (populated after start()).
    const std::string& opened_device_name() const;

    // Actual resolution negotiated after start() (populated after first frame).
    int actual_width()  const;
    int actual_height() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_;
};

// ── Helpers ───────────────────────────────────────────────────────────────────

#if defined(_WIN32)
// Initialize COM on the calling thread (MTA when possible), once per thread.
// WIC (encode_jpeg / save_frame) requires it. If the thread already owns STA
// (UI thread after OleInitialize), the call is a no-op — do not force MTA there.
void ensure_com_initialized();
#endif

// Capture a single frame from the named device.
// Opens the device, waits up to timeout_ms for a frame, then closes it.
// cancel_fn is polled every ~80 ms; returning true aborts with an exception.
// Throws std::runtime_error on failure, timeout, or cancellation.
VideoFrame capture_still(const std::string& device_name = "",
                         int width      = 0,
                         int height     = 0,
                         int timeout_ms = 5000,
                         std::function<bool()> cancel_fn = {});

// Encode a BGR24 VideoFrame to JPEG bytes in memory.
// quality = 1..100.  Returns empty on failure.
// Win32: WIC.  Linux: libjpeg.  macOS: ImageIO.
std::vector<uint8_t> encode_jpeg(const VideoFrame& frame, int quality = 90);

// Save a VideoFrame to disk.  Format is inferred from the file extension.
// Win32: .jpg/.jpeg/.png/.bmp via WIC.
// Linux: .jpg/.jpeg via libjpeg.
// Returns true on success, or false + sets err.
bool save_frame(const VideoFrame& frame, const std::string& path,
                std::string& err, int jpeg_quality = 90);

// Decode an image file (.jpg/.png/.bmp/…) into a tightly-packed BGR24 frame —
// the inverse of save_frame, and the on-disk entry point for the filter blocks.
// Win32: WIC. Other platforms: not yet implemented (returns false + sets err).
bool load_frame(const std::string& path, VideoFrame& out, std::string& err);

// Normalise/repack a frame to tightly-packed BGR24 (stride == width*3) — the
// canonical layout every filter (and a zero-copy cv::Mat wrap) expects.
// True if a frame is already tightly-packed BGR24 (stride == width*3) — the
// canonical filter layout. Lets a consumer skip to_bgr24() entirely and move the
// buffer instead of copying it (the frame-pump fast path).
inline bool is_packed_bgr24(const VideoFrame& f) noexcept {
    return f.format == PixelFormat::BGR24 && f.width > 0 && f.height > 0 &&
           f.stride == f.width * 3 &&
           f.data.size() >= static_cast<size_t>(f.width) * f.height * 3;
}

// Handles BGR24 (repack to remove row padding) and RGB24 (channel swap).
// Returns false for NV12/MJPEG/Unknown (decode those upstream). Pure; header-
// inline so it is available on every platform without a backend TU.
inline bool to_bgr24(const VideoFrame& in, VideoFrame& out) {
    if (in.width <= 0 || in.height <= 0 || in.data.empty()) return false;
    const bool is_rgb = in.format == PixelFormat::RGB24;
    const bool is_bgr = in.format == PixelFormat::BGR24;
    if (!is_rgb && !is_bgr) return false;
    const int w = in.width, h = in.height;
    const int src_stride = in.stride > 0 ? in.stride : w * 3;
    if (static_cast<size_t>(src_stride) * static_cast<size_t>(h) > in.data.size()) return false;
    out.width        = w;
    out.height       = h;
    out.stride       = w * 3;
    out.format       = PixelFormat::BGR24;
    out.timestamp_ms = in.timestamp_ms;
    out.data.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = in.data.data() + static_cast<size_t>(y) * src_stride;
        uint8_t*       d = out.data.data() + static_cast<size_t>(y) * out.stride;
        if (is_bgr) {
            std::memcpy(d, s, static_cast<size_t>(w) * 3);
        } else { // RGB → BGR
            for (int x = 0; x < w; ++x) {
                d[x * 3 + 0] = s[x * 3 + 2];
                d[x * 3 + 1] = s[x * 3 + 1];
                d[x * 3 + 2] = s[x * 3 + 0];
            }
        }
    }
    return true;
}

#if defined(__APPLE__) && __APPLE__
// ── macOS native recorder ─────────────────────────────────────────────────────
// Records directly to H.264 MP4 (or H.265 HEVC) using AVCaptureMovieFileOutput.
// path extension should be .mp4 or .mov.
//
// device_name  — case-insensitive substring; empty = first/default device.
// width/height — preferred resolution; 0 = pick the highest available.
// fps          — preferred frame rate; 0 = device default.
// duration_ms  — stop after this many ms; 0 = run until stop_fn returns true.
// stop_fn      — polled ~20 Hz; return true to stop early.
// on_status    — called ~1 Hz from the capture thread: (elapsed_ms, frame_count).
//
// Blocks until recording finishes.
// Throws std::runtime_error on error.
void record_movie(const std::string& device_name,
                  int                width,
                  int                height,
                  int                fps,
                  const std::string& path,
                  int                duration_ms,
                  std::function<bool()>                   stop_fn,
                  std::function<void(int64_t, int)>       on_status = {});
#endif // __APPLE__

} // namespace pm::video

