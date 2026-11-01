#pragma once
//
// video_encoder.hpp — frame-fed video encoder (annotated burn-in → .mp4).
//
// Unlike record_movie() (which self-captures a device), this takes individual
// CPU VideoFrames and muxes them to an H.264 MP4. It is the "RecordSink" the
// detect loop feeds *after* burning annotations onto each frame via the shared
// media::vdetect rasterizer — and the same annotated frames can later fan out to
// a Sunshine-style StreamSink.
//
// Platform backends:
//   Win32 — Media Foundation IMFSinkWriter (RGB32 in → H.264 MP4 out).
//   Others — not yet implemented (open() returns false with an explanatory error).
//
// Threading: not internally synchronised. Drive open()/write()/close() from one
// thread (the detect loop).
//
#if defined(_WIN32) && !defined(PM_HAS_AUDIO_IO)
#define PM_HAS_AUDIO_IO 1
#endif

#if defined(FEATURE_VIDEO) && FEATURE_VIDEO

#include "pm/video/capture.hpp"


#include <memory>
#include <string>

#if defined(_WIN32) && PM_HAS_AUDIO_IO
#include "pm/audio/mixed_capture.hpp"
#if defined(FEATURE_AUDIO_FILTER) && FEATURE_AUDIO_FILTER
#  include "pm/audio/filter/audio_filter.hpp"
#endif
#endif

namespace pm::video {

#if defined(_WIN32) && PM_HAS_AUDIO_IO
struct VideoAudioOptions {
    pm::audio::MixedCaptureConfig capture;
#if defined(FEATURE_AUDIO_FILTER) && FEATURE_AUDIO_FILTER
    pm::audio::filter::Options filter;
    bool filter_enabled = false;
#endif
};
#endif

class VideoEncoder {
public:
    VideoEncoder();
    ~VideoEncoder();
    VideoEncoder(const VideoEncoder&)            = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    /// Open an output file. @p width/@p height are cropped to even numbers
    /// (H.264 requirement). @p fps clamps to [1,240]. Optional @p audio muxes
    /// mic/desktop/mix AAC when audio I/O is enabled (Win32 only).
    bool open(const std::string& path, int width, int height, int fps, std::string& err
#if defined(_WIN32) && PM_HAS_AUDIO_IO
              , const VideoAudioOptions* audio = nullptr
#endif
    );

    bool is_open() const noexcept;

    /// Timing model for output PTS. Default false = wall-clock (live capture /
    /// burn-in: playback matches real elapsed time). Set true for file transcode:
    /// PTS = frame_index / fps (constant-rate), so frames produced faster than
    /// real time still play at the correct speed. Call before the first write().
    void set_constant_rate(bool on) noexcept;

    /// Encode one frame (BGR24 or RGB24, top-down). open() must have been called
    /// first. Frames smaller than the opened size are rejected; larger frames are
    /// cropped to the opened (even) size. Returns false + @p err on failure.
    bool write(const VideoFrame& frame, std::string& err);

    /// Finalize and close the file. Safe to call repeatedly / when not open.
    bool close(std::string& err);

private:
    struct Impl;
    std::unique_ptr<Impl> m_;
};

} // namespace pm::video

#endif // FEATURE_VIDEO
