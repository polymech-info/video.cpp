#pragma once
//
// mf_audio_writer.hpp — PCM s16le → AAC stream on an IMFSinkWriter (Win32).


#if defined(_WIN32) && !defined(PM_HAS_AUDIO_IO)
#define PM_HAS_AUDIO_IO 1
#endif

#if defined(FEATURE_VIDEO) && FEATURE_VIDEO && defined(_WIN32) && PM_HAS_AUDIO_IO

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "pm/audio/mixed_capture.hpp"
#if defined(FEATURE_AUDIO_FILTER) && FEATURE_AUDIO_FILTER
#  include "pm/audio/filter/audio_filter.hpp"
#endif

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

struct IMFSinkWriter;

namespace pm::video {

struct VideoAudioMuxConfig {
    pm::audio::MixedCaptureConfig capture;
#if defined(FEATURE_AUDIO_FILTER) && FEATURE_AUDIO_FILTER
    pm::audio::filter::Options filter;
    bool filter_enabled = false;
#endif
};

/// Owns an AAC stream on a shared IMFSinkWriter and optional live MixedAudioCapture.
class MfVideoAudioMux {
public:
    MfVideoAudioMux();
    ~MfVideoAudioMux();

    MfVideoAudioMux(const MfVideoAudioMux&)            = delete;
    MfVideoAudioMux& operator=(const MfVideoAudioMux&) = delete;

    /// Register the AAC output + PCM input types on @p sink (call before BeginWriting).
    /// Sets @p out_stream_index. Returns false when capture.source == none (no stream added).
    bool prepare_stream(IMFSinkWriter* sink, DWORD& out_stream_index, const VideoAudioMuxConfig& cfg, std::string& err);

    /// Start MixedAudioCapture. PCM is held until arm_video_sync() (the first
    /// picture). Samples before that instant are dropped; the rest start at PTS 0.
    /// Later chunks advance by sample duration so a pause does not open a hole.
    bool start_capture(IMFSinkWriter* sink,
                       DWORD stream_index,
                       const VideoAudioMuxConfig& cfg,
                       std::chrono::steady_clock::time_point t0,
                       std::string& err);

    void stop_capture() noexcept;
    bool is_capturing() const noexcept;
    /// Drop captured PCM writes while paused. Callback still runs for meters.
    void set_paused(bool paused) noexcept;

    /// Speech-mic tap (not desktop). Used by auto-pause.
    void set_speech_meter(std::function<void(const int16_t*, size_t, uint32_t, uint32_t)> fn);

    /// Optional 16 kHz mono dump of encoded-timeline audio (captions).
    void enable_caption_dump(const std::filesystem::path& wav_16k) noexcept;
    void finish_caption_dump() noexcept;

    /// IMFSinkWriter is not thread-safe. Hold this around every WriteSample on the
    /// shared sink (video encode worker + audio capture callback).
    std::mutex& sink_write_mutex() noexcept;

    /// Caller holds sink_write_mutex(). Drops audio captured before @p video_origin.
    void arm_video_sync(std::chrono::steady_clock::time_point video_origin) noexcept;
    bool video_sync_armed() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_;
};

} // namespace pm::video

#endif
