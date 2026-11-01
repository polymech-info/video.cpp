#include "pm/video/idle.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace pm::video {
namespace {

constexpr int kLumaR = 54;
constexpr int kLumaG = 183;
constexpr int kLumaB = 19;

std::chrono::milliseconds ms_since(
    std::chrono::steady_clock::time_point then,
    std::chrono::steady_clock::time_point now)
{
    if (then.time_since_epoch().count() == 0)
        return std::chrono::milliseconds::max();
    if (now < then)
        return std::chrono::milliseconds{0};
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - then);
}

} // namespace

bool default_audio_activity(
    const std::int16_t* pcm, std::size_t frames, int channels, int sample_rate,
    float threshold_db)
{
    (void)sample_rate;
    if (!pcm || frames == 0)
        return false;
    const int ch = std::max(1, channels);
    std::int32_t peak = 0;
    const std::size_t n = frames * static_cast<std::size_t>(ch);
    for (std::size_t i = 0; i < n; ++i) {
        const int v = std::abs(static_cast<int>(pcm[i]));
        if (v > peak)
            peak = v;
    }
    if (peak <= 0)
        return false;
    const float dbfs = 20.f * std::log10(static_cast<float>(peak) / 32768.f);
    return dbfs >= threshold_db;
}

void CaptureIdleDetector::configure(const IdlePauseConfig& cfg) noexcept
{
    std::lock_guard<std::mutex> lock(mu_);
    cfg_ = cfg;
    probe_.assign(static_cast<size_t>(cfg_.probe_w * cfg_.probe_h), 0);
    prev_probe_.assign(probe_.size(), 0);
    have_prev_ = false;
    if (!audio_fn_)
        audio_fn_ = default_audio_activity;
}

void CaptureIdleDetector::set_audio_activity_fn(AudioActivityFn fn)
{
    std::lock_guard<std::mutex> lock(mu_);
    audio_fn_ = fn ? std::move(fn) : AudioActivityFn{default_audio_activity};
}

void CaptureIdleDetector::reset() noexcept
{
    std::lock_guard<std::mutex> lock(mu_);
    have_prev_ = false;
    have_mouse_ = false;
    idle_paused_ = false;
    visual_latched_ = false;
    last_coverage_ = 0.f;
    last_changed_tiles_ = 0;
    last_key_ = {};
    last_click_ = {};
    last_mouse_ = {};
    last_speech_ = {};
    last_visual_ = {};
    last_activity_ = std::chrono::steady_clock::now();
    visual_since_ = {};
}

void CaptureIdleDetector::note_key() noexcept
{
    std::lock_guard<std::mutex> lock(mu_);
    last_key_ = std::chrono::steady_clock::now();
}

void CaptureIdleDetector::note_click_or_wheel() noexcept
{
    std::lock_guard<std::mutex> lock(mu_);
    last_click_ = std::chrono::steady_clock::now();
}

void CaptureIdleDetector::note_mouse_screen(int x, int y) noexcept
{
    std::lock_guard<std::mutex> lock(mu_);
    if (have_mouse_) {
        const int dx = x - last_mouse_x_;
        const int dy = y - last_mouse_y_;
        if (dx * dx + dy * dy >= cfg_.mouse_delta_px * cfg_.mouse_delta_px)
            last_mouse_ = std::chrono::steady_clock::now();
    }
    last_mouse_x_ = x;
    last_mouse_y_ = y;
    have_mouse_ = true;
}

void CaptureIdleDetector::note_last_input_tick() noexcept
{
    std::lock_guard<std::mutex> lock(mu_);
    // Coarse: treat as a key-class hold so typing still works without LL hooks.
    last_key_ = std::chrono::steady_clock::now();
}

void CaptureIdleDetector::note_speech_pcm(
    const std::int16_t* pcm, std::size_t frames, int channels, int sample_rate)
{
    AudioActivityFn fn;
    float db = -40.f;
    bool use = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        use = cfg_.enabled() && cfg_.uses_audio() && cfg_.mic_wake;
        fn = audio_fn_;
        db = cfg_.mic_activity_db;
    }
    if (!use || !fn)
        return;
    if (!fn(pcm, frames, channels, sample_rate, db))
        return;
    std::lock_guard<std::mutex> lock(mu_);
    last_speech_ = std::chrono::steady_clock::now();
}

void CaptureIdleDetector::feed_bgr24(
    const std::uint8_t* data, int width, int height, int stride,
    const IdleDirtyRect* dirty, int dirty_count)
{
    if (!data || width < 2 || height < 2)
        return;
    std::lock_guard<std::mutex> lock(mu_);
    if (!cfg_.uses_visual())
        return;
    const int pw = cfg_.probe_w;
    const int ph = cfg_.probe_h;
    probe_.assign(static_cast<size_t>(pw * ph), 0);
    const int row = stride > 0 ? stride : width * 3;
    for (int py = 0; py < ph; ++py) {
        const int sy = std::min(height - 1, py * height / ph);
        const std::uint8_t* src = data + static_cast<size_t>(sy) * static_cast<size_t>(row);
        for (int px = 0; px < pw; ++px) {
            const int sx = std::min(width - 1, px * width / pw);
            const std::uint8_t* p = src + static_cast<size_t>(sx) * 3u;
            const int y = (p[2] * kLumaR + p[1] * kLumaG + p[0] * kLumaB) >> 8;
            probe_[static_cast<size_t>(py * pw + px)] =
                static_cast<std::uint8_t>(std::clamp(y, 0, 255));
        }
    }
    score_probe(dirty, dirty_count);
}

void CaptureIdleDetector::feed_bgra(
    const std::uint8_t* data, int width, int height, int stride,
    const IdleDirtyRect* dirty, int dirty_count)
{
    if (!data || width < 2 || height < 2)
        return;
    std::lock_guard<std::mutex> lock(mu_);
    if (!cfg_.uses_visual())
        return;
    const int pw = cfg_.probe_w;
    const int ph = cfg_.probe_h;
    probe_.assign(static_cast<size_t>(pw * ph), 0);
    const int row = stride > 0 ? stride : width * 4;
    for (int py = 0; py < ph; ++py) {
        const int sy = std::min(height - 1, py * height / ph);
        const std::uint8_t* src = data + static_cast<size_t>(sy) * static_cast<size_t>(row);
        for (int px = 0; px < pw; ++px) {
            const int sx = std::min(width - 1, px * width / pw);
            const std::uint8_t* p = src + static_cast<size_t>(sx) * 4u;
            const int y = (p[2] * kLumaR + p[1] * kLumaG + p[0] * kLumaB) >> 8;
            probe_[static_cast<size_t>(py * pw + px)] =
                static_cast<std::uint8_t>(std::clamp(y, 0, 255));
        }
    }
    score_probe(dirty, dirty_count);
}

void CaptureIdleDetector::feed_luma(
    const std::uint8_t* y, int width, int height,
    const IdleDirtyRect* dirty, int dirty_count)
{
    if (!y || width <= 0 || height <= 0)
        return;
    std::lock_guard<std::mutex> lock(mu_);
    if (!cfg_.uses_visual())
        return;
    const int pw = cfg_.probe_w;
    const int ph = cfg_.probe_h;
    probe_.assign(static_cast<size_t>(pw * ph), 0);
    for (int py = 0; py < ph; ++py) {
        const int sy = std::min(height - 1, py * height / std::max(1, ph));
        const std::uint8_t* src = y + static_cast<size_t>(sy) * static_cast<size_t>(width);
        for (int px = 0; px < pw; ++px) {
            const int sx = std::min(width - 1, px * width / std::max(1, pw));
            probe_[static_cast<size_t>(py * pw + px)] = src[sx];
        }
    }
    score_probe(dirty, dirty_count);
}

void CaptureIdleDetector::score_probe(
    const IdleDirtyRect* dirty, int dirty_count)
{
    const int pw = cfg_.probe_w;
    const int ph = cfg_.probe_h;
    const int gw = cfg_.grid_w;
    const int gh = cfg_.grid_h;
    if (pw <= 0 || ph <= 0 || gw <= 0 || gh <= 0)
        return;
    if (static_cast<int>(probe_.size()) < pw * ph)
        return;
    if (!have_prev_ || prev_probe_.size() != probe_.size()) {
        prev_probe_ = probe_;
        have_prev_ = true;
        last_coverage_ = 0.f;
        last_changed_tiles_ = 0;
        return;
    }

    const int tile_count = gw * gh;
    std::vector<char> check(static_cast<size_t>(tile_count), 1);
    if (dirty && dirty_count > 0) {
        std::fill(check.begin(), check.end(), 0);
        for (int i = 0; i < dirty_count; ++i) {
            const auto& r = dirty[i];
            if (r.w <= 0 || r.h <= 0)
                continue;
            const int x0 = std::clamp(r.x * gw / std::max(1, pw), 0, gw - 1);
            const int y0 = std::clamp(r.y * gh / std::max(1, ph), 0, gh - 1);
            const int x1 = std::clamp((r.x + r.w - 1) * gw / std::max(1, pw), 0, gw - 1);
            const int y1 = std::clamp((r.y + r.h - 1) * gh / std::max(1, ph), 0, gh - 1);
            for (int ty = y0; ty <= y1; ++ty) {
                for (int tx = x0; tx <= x1; ++tx)
                    check[static_cast<size_t>(ty * gw + tx)] = 1;
            }
        }
    }

    const int delta = cfg_.pixel_delta;
    const float tile_frac = static_cast<float>(cfg_.tile_percent) / 100.f;
    float coverage_sum = 0.f;
    int changed_tiles = 0;
    for (int ty = 0; ty < gh; ++ty) {
        for (int tx = 0; tx < gw; ++tx) {
            const int ti = ty * gw + tx;
            if (!check[static_cast<size_t>(ti)])
                continue;
            const int x0 = tx * pw / gw;
            const int x1 = (tx + 1) * pw / gw;
            const int y0 = ty * ph / gh;
            const int y1 = (ty + 1) * ph / gh;
            int samples = 0;
            int changed = 0;
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    const size_t i = static_cast<size_t>(y * pw + x);
                    const int d = std::abs(
                        static_cast<int>(probe_[i]) - static_cast<int>(prev_probe_[i]));
                    ++samples;
                    if (d >= delta)
                        ++changed;
                }
            }
            if (samples <= 0)
                continue;
            const float frac = static_cast<float>(changed) / static_cast<float>(samples);
            if (frac >= tile_frac) {
                coverage_sum += frac;
                ++changed_tiles;
            }
        }
    }
    last_coverage_ = coverage_sum / static_cast<float>(std::max(1, tile_count));
    last_changed_tiles_ = changed_tiles;
    prev_probe_ = probe_;

    const bool visual =
        last_changed_tiles_ >= cfg_.min_tiles &&
        (last_coverage_ * 100.f) >= cfg_.min_coverage_percent;
    if (visual)
        last_visual_ = std::chrono::steady_clock::now();
}

bool CaptureIdleDetector::visual_hot(
    std::chrono::steady_clock::time_point now) noexcept
{
    if (!cfg_.uses_visual())
        return false;
    const auto age = ms_since(last_visual_, now);
    if (idle_paused_) {
        const int recent_ms =
            cfg_.resume_ms + std::max(50, 2000 / std::max(1, cfg_.sample_fps));
        const bool recent = age.count() <= recent_ms;
        if (recent) {
            if (!visual_latched_) {
                visual_since_ = now;
                visual_latched_ = true;
            }
            return ms_since(visual_since_, now).count() >= cfg_.resume_ms;
        }
        visual_latched_ = false;
        return false;
    }
    visual_latched_ = false;
    const int slack_ms = std::max(50, 2000 / std::max(1, cfg_.sample_fps));
    return age.count() <= slack_ms;
}

bool CaptureIdleDetector::input_hot(
    std::chrono::steady_clock::time_point now) const noexcept
{
    if (cfg_.wake_key &&
        ms_since(last_key_, now).count() <= cfg_.key_hold_ms)
        return true;
    if (cfg_.wake_key &&
        ms_since(last_click_, now).count() <= cfg_.key_hold_ms)
        return true;
    if (cfg_.wake_mouse &&
        ms_since(last_mouse_, now).count() <= cfg_.mouse_hold_ms)
        return true;
    return false;
}

bool CaptureIdleDetector::speech_hot(
    std::chrono::steady_clock::time_point now) const noexcept
{
    if (!cfg_.uses_audio() || !cfg_.mic_wake)
        return false;
    return ms_since(last_speech_, now).count() <= cfg_.mic_hangover_ms;
}

IdleTick CaptureIdleDetector::tick(
    std::chrono::steady_clock::time_point now) noexcept
{
    IdleTick t;
    std::lock_guard<std::mutex> lock(mu_);
    if (!cfg_.enabled())
        return t;

#if defined(_WIN32)
    if (!cfg_.wake_key && !cfg_.wake_mouse) {
        // still allow GetLastInputInfo from the caller
    }
#endif

    t.visual = visual_hot(now);
    t.input = input_hot(now);
    t.speech = speech_hot(now);
    t.coverage = last_coverage_;
    t.changed_tiles = last_changed_tiles_;
    t.activity = t.visual || t.input || t.speech;

    if (t.activity)
        last_activity_ = now;

    const bool idle_for = ms_since(last_activity_, now).count() >= cfg_.idle_ms;
    if (idle_paused_) {
        t.want_idle_pause = !t.activity;
    } else {
        t.want_idle_pause = idle_for && !t.activity;
    }
    idle_paused_ = t.want_idle_pause;
    return t;
}

} // namespace pm::video
