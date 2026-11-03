#include "fs/path_sanitizer.hpp"

#if defined(FEATURE_VIDEO) && FEATURE_VIDEO && defined(_WIN32) && PM_HAS_AUDIO_IO

#include "pm/video/mf_audio_mux.hpp"

#include "pm/audio/devices.hpp"
#include "pm/video/hooks.hpp"
#include "pm/audio/constants.hpp"
#if defined(FEATURE_AUDIO_FILTER) && FEATURE_AUDIO_FILTER
#  include "pm/audio/filter/deepfilter.hpp"
#endif
#if defined(FEATURE_STT) && FEATURE_STT
#include "pm/audio/stt/wav.hpp"
#endif

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <thread>
#include <vector>

#if defined(_MSC_VER)
#  pragma warning(push, 0)
#endif
#include "miniaudio.h"
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

namespace pm::video {
namespace {

constexpr UINT32 kMuxSampleRate  = 48000; // Windows MF AAC encoder requirement
constexpr UINT32 kAacBytesPerSec = 24000; // MF AAC-LC max (192 kbps)
constexpr LONGLONG kHnsPerMuxSample =
    10'000'000LL / static_cast<LONGLONG>(kMuxSampleRate);
// Hold at most this much PCM while waiting for the first video frame.
constexpr size_t kMaxPendingSeconds = 3;

std::string hr_hex(HRESULT hr)
{
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08lx", static_cast<unsigned long>(hr));
    return b;
}

void pcm_format_from_cfg(const pm::audio::MixedCaptureConfig& cfg,
                         UINT32& rate,
                         UINT32& channels)
{
    uint32_t sr = 0;
    uint32_t ch = 0;
    pm::audio::resolve_capture_pcm_format(cfg, sr, ch);
    rate     = sr;
    channels = std::max<UINT32>(1, std::min<UINT32>(ch, 2));
}

} // namespace

struct MfVideoAudioMux::Impl {
    pm::audio::MixedAudioCapture capture;
    std::mutex                   write_mu;
    IMFSinkWriter*               sink = nullptr;
    DWORD                        stream = 0;
    LONGLONG                     last_pts = 0;
    bool                         timeline_anchored = false;
    std::chrono::steady_clock::time_point t0{};
    /// Set when the first picture is known. Audio captured before that is dropped.
    std::atomic<bool>            video_armed{false};
    std::chrono::steady_clock::time_point video_origin{};
    std::vector<int16_t>         pending_pcm;
    std::chrono::steady_clock::time_point pending_end{};
    bool                         pending_have = false;
    std::atomic<bool>            active{false};
    std::atomic<bool>            paused{false};
    std::function<void(const int16_t*, size_t, uint32_t, uint32_t)> speech_meter;
    std::filesystem::path        dump_path;
    std::mutex                   dump_mu;
    std::vector<int16_t>         dump_pcm;
    uint32_t                     dump_accum = 0;

    UINT32 capture_rate     = pm::audio::constants::k_recording_sample_rate;
    UINT32 capture_channels = pm::audio::constants::k_recording_channels;
    UINT32 mux_channels     = pm::audio::constants::k_recording_channels;

    ma_resampler                 resampler{};
    bool                         resampler_open = false;
    std::vector<int16_t>         resample_out;

#if defined(FEATURE_AUDIO_FILTER) && FEATURE_AUDIO_FILTER
    struct FilterChunk {
        std::vector<int16_t> pcm;
        size_t frames = 0;
        bool passthrough = false;
    };
    std::unique_ptr<pm::audio::filter::dfn::StreamSession> dfn;
    std::thread              filter_thread;
    std::mutex               q_mu;
    std::condition_variable  q_cv;
    std::deque<FilterChunk>  q;
    size_t                   q_frames = 0;
    std::atomic<bool>        filter_run{false};
    std::atomic<bool>        filter_overflow{false};
    bool                     filter_on = false;
    static constexpr size_t  kMaxFilterQueueFrames = 48000;
#endif

    // Diagnostics (audio callback / encode path — keep cheap).
    std::atomic<uint64_t> chunks_ok{0};
    std::atomic<uint64_t> frames_ok{0};
    std::atomic<uint64_t> burst_drops{0};
    std::atomic<uint64_t> write_fails{0};
    std::atomic<uint64_t> resample_fails{0};
    std::atomic<bool>     logged_first_chunk{false};
    std::atomic<bool>     logged_write_fail{false};
    std::atomic<bool>     logged_burst{false};
    LONGLONG              first_pts_hns = 0;
    std::string           source_name;

    void reset_stats() noexcept
    {
        chunks_ok.store(0, std::memory_order_relaxed);
        frames_ok.store(0, std::memory_order_relaxed);
        burst_drops.store(0, std::memory_order_relaxed);
        write_fails.store(0, std::memory_order_relaxed);
        resample_fails.store(0, std::memory_order_relaxed);
        logged_first_chunk.store(false, std::memory_order_relaxed);
        logged_write_fail.store(false, std::memory_order_relaxed);
        logged_burst.store(false, std::memory_order_relaxed);
        first_pts_hns = 0;
    }

    void init_resampler()
    {
        if (resampler_open) return;
        if (capture_rate == kMuxSampleRate && capture_channels == mux_channels)
            return;
        const ma_resampler_config rc = ma_resampler_config_init(
            ma_format_s16,
            capture_channels,
            capture_rate,
            kMuxSampleRate,
            ma_resample_algorithm_linear);
        resampler_open = (ma_resampler_init(&rc, nullptr, &resampler) == MA_SUCCESS);
        if (!resampler_open) {
            pm::video::log_error("[mf-audio] resampler init failed capture=" +
                          std::to_string(capture_rate) + "Hz/" +
                          std::to_string(capture_channels) + "ch → mux=" +
                          std::to_string(kMuxSampleRate) + "Hz/" +
                          std::to_string(mux_channels) + "ch");
        } else {
            pm::video::log_warn("[mf-audio] resampling " + std::to_string(capture_rate) +
                         "Hz/" + std::to_string(capture_channels) + "ch → " +
                         std::to_string(kMuxSampleRate) + "Hz/" +
                         std::to_string(mux_channels) + "ch");
        }
    }

    void uninit_resampler() noexcept
    {
        if (!resampler_open) return;
        ma_resampler_uninit(&resampler, nullptr);
        resampler_open = false;
    }

    static LONGLONG to_hns(std::chrono::steady_clock::duration d)
    {
        return std::chrono::duration_cast<
            std::chrono::duration<LONGLONG, std::ratio<1, 10'000'000>>>(d).count();
    }

    /// OBS win-wasapi (!useDeviceTiming): the callback time is the end of the
    /// packet, so a startup burst is stamped backwards from now. Samples that
    /// fall before the first picture are dropped (obs-output prune).
    void hold_until_video(const int16_t* pcm, size_t frames)
    {
        const size_t n = frames * mux_channels;
        pending_pcm.insert(pending_pcm.end(), pcm, pcm + n);
        pending_end = std::chrono::steady_clock::now();
        pending_have = true;
        const size_t max_frames = static_cast<size_t>(kMuxSampleRate) * kMaxPendingSeconds;
        const size_t have = pending_pcm.size() / mux_channels;
        if (have > max_frames) {
            const size_t drop = have - max_frames;
            pending_pcm.erase(pending_pcm.begin(),
                              pending_pcm.begin() + static_cast<std::ptrdiff_t>(drop * mux_channels));
            burst_drops.fetch_add(drop, std::memory_order_relaxed);
        }
    }

    bool emit_pcm(const int16_t* pcm, size_t frames)
    {
        const DWORD nbytes =
            static_cast<DWORD>(frames * mux_channels * sizeof(int16_t));

        IMFMediaBuffer* buf = nullptr;
        if (FAILED(MFCreateMemoryBuffer(nbytes, &buf))) {
            pm::video::log_error("[mf-audio] MFCreateMemoryBuffer failed nbytes=" +
                          std::to_string(nbytes));
            return false;
        }
        BYTE* dst = nullptr;
        DWORD maxlen = 0, curlen = 0;
        if (FAILED(buf->Lock(&dst, &maxlen, &curlen))) {
            buf->Release();
            pm::video::log_error("[mf-audio] IMFMediaBuffer::Lock failed");
            return false;
        }
        std::memcpy(dst, pcm, nbytes);
        buf->Unlock();
        buf->SetCurrentLength(nbytes);

        IMFSample* smp = nullptr;
        if (FAILED(MFCreateSample(&smp))) {
            buf->Release();
            pm::video::log_error("[mf-audio] MFCreateSample failed");
            return false;
        }
        smp->AddBuffer(buf);

        const LONGLONG pts = last_pts;
        const LONGLONG dur = static_cast<LONGLONG>(frames) * kHnsPerMuxSample;
        smp->SetSampleTime(pts);
        smp->SetSampleDuration(std::max<LONGLONG>(dur, 1));
        last_pts = pts + std::max<LONGLONG>(dur, 1);

        const HRESULT hr = sink->WriteSample(stream, smp);
        smp->Release();
        buf->Release();
        if (FAILED(hr)) {
            const uint64_t n =
                write_fails.fetch_add(1, std::memory_order_relaxed) + 1;
            if (!logged_write_fail.exchange(true, std::memory_order_relaxed) ||
                (n % 50) == 0) {
                pm::video::log_error("[mf-audio] WriteSample failed #" +
                              std::to_string(n) + " hr=" + hr_hex(hr) +
                              " stream=" + std::to_string(stream) +
                              " pts_ms=" + std::to_string(pts / 10000) +
                              " frames=" + std::to_string(frames) +
                              " source=" + source_name);
            }
            return false;
        }

        chunks_ok.fetch_add(1, std::memory_order_relaxed);
        frames_ok.fetch_add(frames, std::memory_order_relaxed);
        append_caption_dump(pcm, frames);
        return true;
    }

    /// Place @p pcm so the last sample lines up with @p end_hns on the video
    /// timeline. Leading samples before 0 are dropped.
    bool place_and_emit(const int16_t* pcm, size_t frames, LONGLONG end_hns)
    {
        if (!pcm || frames == 0) return true;
        const LONGLONG dur = static_cast<LONGLONG>(frames) * kHnsPerMuxSample;
        LONGLONG start = end_hns - dur;
        const int16_t* src = pcm;
        size_t n = frames;
        size_t skipped = 0;
        if (start < 0) {
            skipped = static_cast<size_t>((-start) / kHnsPerMuxSample);
            if (skipped > n) skipped = n;
            src += skipped * mux_channels;
            n -= skipped;
            start = 0;
            burst_drops.fetch_add(skipped, std::memory_order_relaxed);
        }
        if (!logged_first_chunk.exchange(true, std::memory_order_relaxed)) {
            pm::video::log_warn("[mf-audio] sync to first video frame source=" + source_name +
                         " dropped_ms=" + std::to_string(skipped * 1000 / kMuxSampleRate) +
                         " kept_ms=" + std::to_string(n * 1000 / kMuxSampleRate) +
                         " pts_ms=" + std::to_string(start / 10000));
        }
        if (n == 0) return true;
        if (!timeline_anchored) {
            timeline_anchored = true;
            last_pts = start;
            first_pts_hns = start;
        }
        return emit_pcm(src, n);
    }

    bool write_pcm_locked(const int16_t* pcm, size_t frames)
    {
        if (paused.load(std::memory_order_relaxed))
            return true;
        if (!sink || frames == 0 || !pcm) return true;
        if (!video_armed.load(std::memory_order_acquire)) {
            hold_until_video(pcm, frames);
            return true;
        }
        if (!timeline_anchored) {
            const LONGLONG end = std::max<LONGLONG>(
                0, to_hns(std::chrono::steady_clock::now() - video_origin));
            return place_and_emit(pcm, frames, end);
        }
        return emit_pcm(pcm, frames);
    }

    void arm_video_sync_locked(std::chrono::steady_clock::time_point origin)
    {
        if (video_armed.exchange(true, std::memory_order_acq_rel))
            return;
        video_origin = origin;
        if (!pending_have || pending_pcm.empty() || mux_channels == 0) {
            pending_pcm.clear();
            pending_have = false;
            return;
        }
        const size_t frames = pending_pcm.size() / mux_channels;
        LONGLONG end = to_hns(pending_end - origin);
        if (end < 0) end = 0;
        std::vector<int16_t> held;
        held.swap(pending_pcm);
        pending_have = false;
        (void)place_and_emit(held.data(), frames, end);
    }

    void append_caption_dump(const int16_t* pcm, size_t frames)
    {
        std::lock_guard<std::mutex> lk(dump_mu);
        if (dump_path.empty() || !pcm || frames == 0)
            return;
        const uint32_t step = kMuxSampleRate / 16000u;
        for (size_t f = 0; f < frames; ++f) {
            dump_accum += 1;
            if (step > 1 && (dump_accum % step) != 0)
                continue;
            int32_t acc = 0;
            for (UINT32 c = 0; c < mux_channels; ++c)
                acc += pcm[f * mux_channels + c];
            dump_pcm.push_back(static_cast<int16_t>(
                acc / static_cast<int32_t>(std::max<UINT32>(1, mux_channels))));
        }
    }

    bool write_capture_pcm(const int16_t* pcm, size_t frames)
    {
        if (!pcm || frames == 0) return true;

        if (capture_rate == kMuxSampleRate && capture_channels == mux_channels)
            return write_pcm_locked(pcm, frames);

        init_resampler();
        if (!resampler_open) {
            // Last resort: push raw (likely wrong rate) — surface loudly.
            pm::video::log_error("[mf-audio] resampler unavailable; writing capture PCM "
                          "without rate convert (likely corrupt AAC)");
            return write_pcm_locked(pcm, frames);
        }

        ma_uint64 in_frames  = frames;
        const ma_uint64 out_cap =
            static_cast<ma_uint64>(frames) * kMuxSampleRate / capture_rate + 64;
        resample_out.resize(static_cast<size_t>(out_cap * mux_channels));
        ma_uint64 out_frames = out_cap;
        const ma_result rr = ma_resampler_process_pcm_frames(
            &resampler, pcm, &in_frames, resample_out.data(), &out_frames);
        if (rr != MA_SUCCESS || out_frames == 0) {
            const uint64_t n =
                resample_fails.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n == 1 || (n % 50) == 0) {
                pm::video::log_error("[mf-audio] resample failed #" + std::to_string(n) +
                              " ma=" + std::to_string(static_cast<int>(rr)) +
                              " in_frames=" + std::to_string(frames) +
                              " out_frames=" + std::to_string(out_frames));
            }
            return false;
        }
        return write_pcm_locked(resample_out.data(), static_cast<size_t>(out_frames));
    }

#if defined(FEATURE_AUDIO_FILTER) && FEATURE_AUDIO_FILTER
    void enqueue_filter(const int16_t* pcm, size_t frames)
    {
        if (!pcm || frames == 0) return;
        FilterChunk chunk;
        chunk.frames = frames;
        chunk.pcm.assign(pcm, pcm + frames * capture_channels);
        {
            std::lock_guard<std::mutex> lk(q_mu);
            if (q_frames + frames > kMaxFilterQueueFrames) {
                if (!q.empty()) {
                    q_frames -= q.front().frames;
                    q.pop_front();
                }
                chunk.passthrough = true;
                if (!filter_overflow.exchange(true, std::memory_order_relaxed))
                    pm::video::log_warn("[mf-audio] DeepFilterNet worker behind; passing through");
            }
            q_frames += frames;
            q.push_back(std::move(chunk));
        }
        q_cv.notify_one();
    }

    void filter_loop()
    {
        for (;;) {
            FilterChunk chunk;
            {
                std::unique_lock<std::mutex> lk(q_mu);
                q_cv.wait(lk, [&] { return !q.empty() || !filter_run.load(); });
                if (q.empty() && !filter_run.load())
                    break;
                chunk = std::move(q.front());
                q.pop_front();
                q_frames -= chunk.frames;
            }
            if (chunk.passthrough || !dfn || !dfn->is_open()) {
                std::lock_guard<std::mutex> lk(write_mu);
                (void)write_capture_pcm(chunk.pcm.data(), chunk.frames);
                continue;
            }
            std::vector<int16_t> muxed;
            const int16_t* src = chunk.pcm.data();
            size_t src_frames = chunk.frames;
            if (capture_rate != kMuxSampleRate || capture_channels != mux_channels) {
                init_resampler();
                if (resampler_open) {
                    ma_uint64 in_frames = chunk.frames;
                    const ma_uint64 out_cap =
                        static_cast<ma_uint64>(chunk.frames) * kMuxSampleRate / capture_rate + 64;
                    resample_out.resize(static_cast<size_t>(out_cap * mux_channels));
                    ma_uint64 out_frames = out_cap;
                    if (ma_resampler_process_pcm_frames(
                            &resampler, chunk.pcm.data(), &in_frames,
                            resample_out.data(), &out_frames) == MA_SUCCESS &&
                        out_frames > 0) {
                        src = resample_out.data();
                        src_frames = static_cast<size_t>(out_frames);
                    }
                }
            }
            std::vector<int16_t> enhanced;
            dfn->process(src, src_frames, static_cast<uint16_t>(mux_channels), enhanced);
            if (enhanced.empty())
                continue;
            std::lock_guard<std::mutex> lk(write_mu);
            (void)write_pcm_locked(enhanced.data(), enhanced.size() / mux_channels);
        }
        if (dfn && dfn->is_open()) {
            std::vector<int16_t> tail;
            dfn->flush(tail);
            if (!tail.empty() && mux_channels > 0) {
                std::lock_guard<std::mutex> lk(write_mu);
                (void)write_pcm_locked(tail.data(), tail.size() / mux_channels);
            }
        }
    }

    void stop_filter_worker()
    {
        filter_run.store(false, std::memory_order_relaxed);
        q_cv.notify_all();
        if (filter_thread.joinable())
            filter_thread.join();
        q.clear();
        q_frames = 0;
        dfn.reset();
        filter_on = false;
    }
#endif
};

MfVideoAudioMux::MfVideoAudioMux() : m_(std::make_unique<Impl>()) {}
MfVideoAudioMux::~MfVideoAudioMux() { stop_capture(); }

bool MfVideoAudioMux::prepare_stream(IMFSinkWriter* sink,
                                     DWORD& out_stream_index,
                                     const VideoAudioMuxConfig& cfg,
                                     std::string& err)
{
    err.clear();
    if (!pm::audio::capture_source_active(cfg.capture)) {
        out_stream_index = 0;
        return true;
    }
    if (!sink) {
        err = "mf audio: null sink";
        pm::video::log_error("[mf-audio] prepare_stream: " + err);
        return false;
    }

    pcm_format_from_cfg(cfg.capture, m_->capture_rate, m_->capture_channels);
    // Resampler changes rate only; keep channel count identical to capture.
    m_->mux_channels = m_->capture_channels;
    m_->source_name = pm::audio::capture_source_name(cfg.capture.source);

    pm::video::log_warn("[mf-audio] prepare_stream source=" + m_->source_name +
                 " capture=" + std::to_string(m_->capture_rate) + "Hz/" +
                 std::to_string(m_->capture_channels) + "ch → AAC " +
                 std::to_string(kMuxSampleRate) + "Hz/" +
                 std::to_string(m_->mux_channels) + "ch @" +
                 std::to_string(kAacBytesPerSec) + " B/s");

    const UINT32 pcm_bytes_per_sec =
        kMuxSampleRate * m_->mux_channels * sizeof(int16_t);

    IMFMediaType* outT = nullptr;
    HRESULT hr = MFCreateMediaType(&outT);
    if (SUCCEEDED(hr)) {
        outT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        outT->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        outT->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, m_->mux_channels);
        outT->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kMuxSampleRate);
        outT->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        outT->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kAacBytesPerSec);
        outT->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        outT->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29); // AAC-LC
        hr = sink->AddStream(outT, &out_stream_index);
        outT->Release();
    }
    if (FAILED(hr)) {
        err = "mf audio: AddStream AAC failed";
        pm::video::log_error("[mf-audio] " + err + " hr=" + hr_hex(hr) +
                      " ch=" + std::to_string(m_->mux_channels));
        return false;
    }

    IMFMediaType* inT = nullptr;
    hr = MFCreateMediaType(&inT);
    if (SUCCEEDED(hr)) {
        inT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        inT->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        inT->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, m_->mux_channels);
        inT->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kMuxSampleRate);
        inT->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        inT->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,
                        m_->mux_channels * sizeof(int16_t));
        inT->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, pcm_bytes_per_sec);
        hr = sink->SetInputMediaType(out_stream_index, inT, nullptr);
        inT->Release();
    }
    if (FAILED(hr)) {
        err = "mf audio: SetInputMediaType PCM failed (HRESULT=" + hr_hex(hr) + ")";
        pm::video::log_error("[mf-audio] " + err +
                      " stream=" + std::to_string(out_stream_index) +
                      " ch=" + std::to_string(m_->mux_channels));
        return false;
    }

    pm::video::log_warn("[mf-audio] prepare_stream ok stream=" +
                 std::to_string(out_stream_index));
    return true;
}

bool MfVideoAudioMux::start_capture(IMFSinkWriter* sink,
                                    DWORD stream_index,
                                    const VideoAudioMuxConfig& cfg,
                                    std::chrono::steady_clock::time_point t0,
                                    std::string& err)
{
    stop_capture();
    err.clear();
    if (!pm::audio::capture_source_active(cfg.capture))
        return true;
    if (!sink) {
        err = "mf audio: null sink for capture";
        pm::video::log_error("[mf-audio] start_capture: " + err);
        return false;
    }

    m_->sink = sink;
    m_->stream = stream_index;
    m_->t0 = t0;
    m_->last_pts = 0;
    m_->timeline_anchored = false;
    m_->video_armed.store(false, std::memory_order_relaxed);
    m_->pending_pcm.clear();
    m_->pending_have = false;
    m_->paused.store(false, std::memory_order_relaxed);
    m_->uninit_resampler();
    m_->reset_stats();
    pcm_format_from_cfg(cfg.capture, m_->capture_rate, m_->capture_channels);
    m_->mux_channels = m_->capture_channels;
    m_->source_name = pm::audio::capture_source_name(cfg.capture.source);
    m_->active.store(true);

    pm::video::log_warn("[mf-audio] start_capture source=" + m_->source_name +
                 " stream=" + std::to_string(stream_index) +
                 " capture=" + std::to_string(m_->capture_rate) + "Hz/" +
                 std::to_string(m_->capture_channels) + "ch" +
                 " mic=\"" + cfg.capture.mic_device + "\"" +
                 " desktop=\"" + cfg.capture.desktop_device + "\"" +
                 " camera_audio=" +
                 std::string(cfg.capture.camera_audio ? "1" : "0") +
                 " camera=\"" + cfg.capture.camera_device + "\"");

    pm::audio::MixedCaptureConfig capture_cfg = cfg.capture;
    if (!capture_cfg.mic_device.empty() &&
        !pm::audio::capture_device_matches(capture_cfg.mic_device)) {
        pm::video::log_warn("[mf-audio] audio device \"" + capture_cfg.mic_device +
                     "\" was not found; using the default capture device");
        capture_cfg.mic_device.clear();
    }
    if (capture_cfg.camera_audio && !capture_cfg.camera_device.empty() &&
        !pm::audio::capture_device_matches(capture_cfg.camera_device)) {
        pm::video::log_warn("[mf-audio] camera audio device \"" + capture_cfg.camera_device +
                     "\" was not found; using the default capture device");
        capture_cfg.camera_device.clear();
    }

#if defined(FEATURE_AUDIO_FILTER) && FEATURE_AUDIO_FILTER
    m_->filter_on = false;
    pm::audio::filter::Options filter = cfg.filter;
    if (cfg.filter_enabled) {
        if (!pm::audio::capture_has_speech_mic(capture_cfg)) {
            err = "live audio filter requires --audio-source mic or mix (or camera audio)";
            m_->active.store(false);
            return false;
        }
        if (filter.kind == pm::audio::filter::Kind::Gtcrn) {
            pm::video::log_warn("[mf-audio] gtcrn is reserved; using DeepFilterNet");
            filter.kind = pm::audio::filter::Kind::DeepFilter;
        }
        m_->dfn = std::make_unique<pm::audio::filter::dfn::StreamSession>();
        if (!m_->dfn->open(filter, err)) {
            m_->dfn.reset();
            m_->active.store(false);
            return false;
        }
        m_->filter_on = true;
        m_->filter_run.store(true, std::memory_order_relaxed);
        m_->filter_overflow.store(false, std::memory_order_relaxed);
        m_->filter_thread = std::thread([this] { m_->filter_loop(); });
        pm::video::log_warn("[mf-audio] DeepFilterNet live on mux worker");
    }
#endif

    auto on_pcm = [this](const int16_t* pcm, size_t frames) {
        if (!m_->active.load()) return;
#if defined(FEATURE_AUDIO_FILTER) && FEATURE_AUDIO_FILTER
        if (m_->filter_on) {
            if (m_->paused.load(std::memory_order_relaxed))
                return;
            m_->enqueue_filter(pcm, frames);
            return;
        }
#endif
        std::lock_guard<std::mutex> lk(m_->write_mu);
        if (m_->paused.load(std::memory_order_relaxed)) return;
        (void)m_->write_capture_pcm(pcm, frames);
    };

    if (!m_->capture.start(capture_cfg, std::move(on_pcm), err)) {
        m_->active.store(false);
#if defined(FEATURE_AUDIO_FILTER) && FEATURE_AUDIO_FILTER
        m_->stop_filter_worker();
#endif
        pm::video::log_error("[mf-audio] MixedAudioCapture::start failed source=" +
                      m_->source_name + " err=" + err);
        return false;
    }

    pm::video::log_warn("[mf-audio] capture running mic=\"" +
                 m_->capture.opened_mic_device() + "\" desktop=\"" +
                 m_->capture.opened_desktop_device() + "\" camera=\"" +
                 m_->capture.opened_camera_device() + "\"");
    return true;
}

void MfVideoAudioMux::stop_capture() noexcept
{
    const bool was_active = m_->active.exchange(false, std::memory_order_relaxed);
    m_->capture.stop();
#if defined(FEATURE_AUDIO_FILTER) && FEATURE_AUDIO_FILTER
    m_->stop_filter_worker();
#endif

    std::lock_guard<std::mutex> lk(m_->write_mu);
    if (was_active || m_->chunks_ok.load(std::memory_order_relaxed) > 0 ||
        m_->write_fails.load(std::memory_order_relaxed) > 0) {
        const uint64_t chunks = m_->chunks_ok.load(std::memory_order_relaxed);
        const uint64_t frames = m_->frames_ok.load(std::memory_order_relaxed);
        const uint64_t bursts = m_->burst_drops.load(std::memory_order_relaxed);
        const uint64_t fails  = m_->write_fails.load(std::memory_order_relaxed);
        const uint64_t rsfails = m_->resample_fails.load(std::memory_order_relaxed);
        const LONGLONG end_pts = m_->last_pts;
        std::string summary =
            "[mf-audio] stop source=" + m_->source_name +
            " chunks=" + std::to_string(chunks) +
            " frames=" + std::to_string(frames) +
            " first_pts_ms=" + std::to_string(m_->first_pts_hns / 10000) +
            " end_pts_ms=" + std::to_string(end_pts / 10000) +
            " burst_drops=" + std::to_string(bursts) +
            " write_fails=" + std::to_string(fails) +
            " resample_fails=" + std::to_string(rsfails);
        if (chunks == 0) {
            pm::video::log_error(summary + " — no audio samples written (MP4 may be unplayable)");
        } else if (fails > 0 || rsfails > 0) {
            pm::video::log_error(summary);
        } else if (bursts > 0) {
            pm::video::log_warn(summary);
        } else {
            pm::video::log_warn(summary);
        }
    }

    m_->uninit_resampler();
    m_->sink = nullptr;
    m_->last_pts = 0;
    m_->timeline_anchored = false;
    m_->video_armed.store(false, std::memory_order_relaxed);
    m_->pending_pcm.clear();
    m_->pending_have = false;
    m_->t0 = {};
    m_->paused.store(false, std::memory_order_relaxed);
    finish_caption_dump();
}

bool MfVideoAudioMux::is_capturing() const noexcept
{
    return m_->active.load() && m_->capture.is_running();
}

void MfVideoAudioMux::set_paused(bool paused) noexcept
{
    const bool prev = m_->paused.exchange(paused, std::memory_order_relaxed);
    if (paused) {
#if defined(FEATURE_AUDIO_FILTER) && FEATURE_AUDIO_FILTER
        std::lock_guard<std::mutex> lk(m_->q_mu);
        m_->q.clear();
        m_->q_frames = 0;
#endif
    }
    if (prev != paused) {
        LONGLONG pts = 0;
        {
            std::lock_guard<std::mutex> lk(m_->write_mu);
            pts = m_->last_pts;
        }
        pm::video::log_warn(std::string("[mf-audio] ") + (paused ? "paused" : "resumed") +
                     " source=" + m_->source_name +
                     " pts_ms=" + std::to_string(pts / 10000));
    }
}

void MfVideoAudioMux::set_speech_meter(
    std::function<void(const int16_t*, size_t, uint32_t, uint32_t)> fn)
{
    m_->capture.set_speech_meter(std::move(fn));
}

void MfVideoAudioMux::enable_caption_dump(const std::filesystem::path& wav_16k) noexcept
{
    std::lock_guard<std::mutex> lk(m_->dump_mu);
    m_->dump_path = wav_16k;
    m_->dump_pcm.clear();
    m_->dump_accum = 0;
}

void MfVideoAudioMux::finish_caption_dump() noexcept
{
    std::filesystem::path path;
    std::vector<int16_t> pcm;
    {
        std::lock_guard<std::mutex> lk(m_->dump_mu);
        path = m_->dump_path;
        pcm.swap(m_->dump_pcm);
        m_->dump_path.clear();
        m_->dump_accum = 0;
    }
    if (path.empty() || pcm.empty()) {
        if (!path.empty() && pcm.empty())
            pm::video::log_warn("[captions] dump empty (no encoded PCM) — skip wav");
        return;
    }
#if defined(FEATURE_STT) && FEATURE_STT
    try {
        const bool ok = pm::stt::write_pcm_wav(
            media::path::path_to_utf8(path), pcm.data(), pcm.size(), 16000);
        if (!ok)
            pm::video::log_error("[captions] write_pcm_wav failed");
    } catch (const std::exception& ex) {
        pm::video::log_error(std::string("[captions] dump write exception: ") + ex.what());
    } catch (...) {
        pm::video::log_error("[captions] dump write failed");
    }
#else
    (void)path;
#endif
}

std::mutex& MfVideoAudioMux::sink_write_mutex() noexcept
{
    return m_->write_mu;
}

void MfVideoAudioMux::arm_video_sync(std::chrono::steady_clock::time_point video_origin) noexcept
{
    m_->arm_video_sync_locked(video_origin);
}

bool MfVideoAudioMux::video_sync_armed() const noexcept
{
    return m_->video_armed.load(std::memory_order_acquire);
}

} // namespace pm::video

#endif
