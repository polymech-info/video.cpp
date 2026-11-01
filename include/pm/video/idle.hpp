#pragma once
//
// capture_idle_detector.hpp — tiled luma + input/speech activity for auto-pause.
// Dirty rects are ROI hints only. Coverage (not bbox area) is the visual gate.

#include "pm/video/creator_fx.hpp"
#include "pm/video/pause.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace pm::video {

struct IdleDirtyRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct IdleTick {
    bool activity = false;
    bool visual = false;
    bool input = false;
    bool speech = false;
    bool want_idle_pause = false;
    float coverage = 0.f;
    int changed_tiles = 0;
};

/// Default speech-mic probe: peak dBFS over the buffer (not VAD).
/// Swap for WebRTC VAD later without changing the idle state machine.
using AudioActivityFn = std::function<bool(
    const std::int16_t* pcm, std::size_t frames, int channels, int sample_rate,
    float threshold_db)>;

bool default_audio_activity(
    const std::int16_t* pcm, std::size_t frames, int channels, int sample_rate,
    float threshold_db);

class CaptureIdleDetector {
public:
    void configure(const IdlePauseConfig& cfg) noexcept;
    void set_audio_activity_fn(AudioActivityFn fn);

    void reset() noexcept;

    void note_key() noexcept;
    void note_click_or_wheel() noexcept;
    void note_mouse_screen(int x, int y) noexcept;
    void note_last_input_tick() noexcept; // GetLastInputInfo fallback
    void note_speech_pcm(const std::int16_t* pcm, std::size_t frames,
                         int channels, int sample_rate);

    /// Packed BGR24 (webcam/compat). Downsamples to the probe plane.
    void feed_bgr24(const std::uint8_t* data, int width, int height, int stride,
                    const IdleDirtyRect* dirty, int dirty_count);

    /// Packed BGRA (WGC mapped probe).
    void feed_bgra(const std::uint8_t* data, int width, int height, int stride,
                   const IdleDirtyRect* dirty, int dirty_count);

    /// Already-sized luma probe (probeW × probeH).
    void feed_luma(const std::uint8_t* y, int width, int height,
                   const IdleDirtyRect* dirty, int dirty_count);

    IdleTick tick(std::chrono::steady_clock::time_point now) noexcept;

    bool enabled() const noexcept { return cfg_.enabled(); }

    int probe_w() const noexcept { return cfg_.probe_w; }
    int probe_h() const noexcept { return cfg_.probe_h; }

private:
    void score_probe(const IdleDirtyRect* dirty, int dirty_count);
    bool visual_hot(std::chrono::steady_clock::time_point now) noexcept;
    bool input_hot(std::chrono::steady_clock::time_point now) const noexcept;
    bool speech_hot(std::chrono::steady_clock::time_point now) const noexcept;

    IdlePauseConfig cfg_{};
    AudioActivityFn audio_fn_{};

    mutable std::mutex mu_;
    std::vector<std::uint8_t> probe_;
    std::vector<std::uint8_t> prev_probe_;
    bool have_prev_ = false;

    std::chrono::steady_clock::time_point last_key_{};
    std::chrono::steady_clock::time_point last_click_{};
    std::chrono::steady_clock::time_point last_mouse_{};
    std::chrono::steady_clock::time_point last_speech_{};
    std::chrono::steady_clock::time_point last_visual_{};
    std::chrono::steady_clock::time_point last_activity_{};
    std::chrono::steady_clock::time_point visual_since_{};
    bool visual_latched_ = false;
    bool idle_paused_ = false;

    int last_mouse_x_ = 0;
    int last_mouse_y_ = 0;
    bool have_mouse_ = false;

    float last_coverage_ = 0.f;
    int last_changed_tiles_ = 0;
};

} // namespace pm::video
