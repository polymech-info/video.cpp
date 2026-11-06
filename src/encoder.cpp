//
// video_encoder.cpp — see video_encoder.hpp.
//
#include "pm/video/encoder.hpp"

#if defined(FEATURE_VIDEO) && FEATURE_VIDEO

#include "pm/video/hooks.hpp"

#include <algorithm>
#include <cstdio>

#if defined(_WIN32) && PM_HAS_AUDIO_IO
#include "pm/video/mf_audio_mux.hpp"
#endif

// ── Platform headers at global scope (must NOT live inside a namespace) ──────
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stringapiset.h>   // MultiByteToWideChar (excluded by WIN32_LEAN_AND_MEAN)
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#endif // _WIN32

namespace pm::video {

// ── Win32: Media Foundation IMFSinkWriter (RGB32 → H.264 MP4) ────────────────
#if defined(_WIN32)

namespace {

// Process-wide MFStartup refcount so multiple encoders / the session recorder
// can coexist without one shutting MF down under another.
std::atomic<int> g_mf_users{0};
void mf_retain() noexcept {
    if (g_mf_users.fetch_add(1) + 1 == 1) (void)MFStartup(MF_VERSION, MFSTARTUP_FULL);
}
void mf_release() noexcept {
    if (g_mf_users.fetch_sub(1) - 1 == 0) (void)MFShutdown();
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring(s.begin(), s.end());
    std::wstring w((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

template <class T> void safe_release(T*& p) { if (p) { p->Release(); p = nullptr; } }

} // namespace

struct VideoEncoder::Impl {
    IMFSinkWriter* sink   = nullptr;
    DWORD          stream = 0;
    bool           mf     = false;   // holds an MFStartup ref
    int            w = 0, h = 0, fps = 30;
    LONGLONG       frame_idx = 0;
    LONGLONG       frame_dur = 0;     // nominal 1/fps in 100ns units
    // Timing model:
    //   wall-clock (default) — live capture / burn-in. Playback duration must
    //     match real elapsed time (loop interval, capture jitter), so we stamp by
    //     elapsed wall time and fall back to the nominal grid before t0.
    //   constant-rate         — file transcode. Frames are produced as fast as the
    //     CPU allows, so wall-clock would compress the timeline (plays too fast);
    //     instead PTS = frame_idx / fps, so output duration == frame_count / fps.
    std::atomic<bool> constant_rate{false};
    std::chrono::steady_clock::time_point t0{};
    bool           have_t0   = false;
    LONGLONG       last_ts   = 0;
    std::vector<uint8_t> rowbuf;      // scratch BGRA frame (worker-only)

#if PM_HAS_AUDIO_IO
    std::unique_ptr<MfVideoAudioMux> audio_mux;
    DWORD                            audio_stream = 0;
    bool                             audio_enabled = false;
    std::chrono::steady_clock::time_point audio_t0{};
#endif

    // Async encode: write() copies the frame into a bounded queue and returns; a
    // worker thread does the pack + WriteSample so the producer (decode/filter)
    // overlaps the encoder instead of blocking on it. Encode errors are deferred
    // and surfaced on the next write() / on finalize().
    static constexpr size_t kCap = 8;
    struct Queued { VideoFrame f; std::chrono::steady_clock::time_point cap; };
    std::thread             worker_;
    std::mutex              qmtx_;
    std::condition_variable not_empty_, not_full_;
    std::deque<Queued>      queue_;
    bool                    finish_ = false;   // drain then exit (graceful)
    bool                    stop_   = false;   // exit ASAP (abort)
    std::string             worker_err_;       // first encode error (guarded by qmtx_)

    ~Impl() {
        std::string ignore;
        finalize(ignore);
        if (mf) { mf_release(); mf = false; }
    }

    // Drain the queue, join the worker, then finalize the MP4 sink. Idempotent.
    bool finalize(std::string& err) {
        const LONGLONG frames_written = frame_idx;
        if (worker_.joinable()) {
            { std::lock_guard<std::mutex> lk(qmtx_); finish_ = true; }
            not_empty_.notify_all();
            not_full_.notify_all();
            worker_.join();
        }
        bool ok = true;
        {
            std::lock_guard<std::mutex> lk(qmtx_);
            if (!worker_err_.empty()) {
                err = worker_err_;
                ok = false;
                pm::video::log_error("[video-encoder] worker error before finalize: " + err);
            }
        }
#if PM_HAS_AUDIO_IO
        const bool had_audio = audio_enabled && audio_mux;
        if (audio_mux) audio_mux->stop_capture();
        audio_mux.reset();
        audio_enabled = false;
#else
        const bool had_audio = false;
#endif
        if (sink) {
            const HRESULT hr = sink->Finalize();
            safe_release(sink);
            if (FAILED(hr)) {
                char b[16];
                std::snprintf(b, sizeof(b), "0x%08lx", static_cast<unsigned long>(hr));
                err = std::string("IMFSinkWriter::Finalize failed hr=") + b;
                ok = false;
                pm::video::log_error("[video-encoder] " + err +
                              " frames=" + std::to_string(frames_written) +
                              " audio=" + (had_audio ? "1" : "0"));
            } else if (frames_written == 0) {
                ok = false;
                err = "encoder finalized with 0 video frames (empty/incomplete MP4)";
                pm::video::log_error("[video-encoder] " + err +
                              " audio=" + std::string(had_audio ? "1" : "0"));
            } else {
                pm::video::log_warn("[video-encoder] Finalize ok frames=" +
                             std::to_string(frames_written) +
                             " audio=" + (had_audio ? "1" : "0") +
                             " last_pts_ms=" + std::to_string(last_ts / 10000));
            }
        }
        return ok;
    }

    bool open(const std::string& path, int width, int height, int f, std::string& err
#if PM_HAS_AUDIO_IO
              , const VideoAudioOptions* audio
#endif
    ) {
        if (sink) { err = "encoder already open"; return false; }
        w   = width  & ~1;            // H.264 needs even dimensions
        h   = height & ~1;
        fps = std::clamp(f, 1, 240);
        if (w < 2 || h < 2) { err = "frame too small for H.264"; return false; }
        frame_idx = 0;
        frame_dur = 10'000'000LL / fps;   // 100ns units
        have_t0   = false;
        last_ts   = 0;

        if (!mf) { mf_retain(); mf = true; }

        pm::video::log_warn("[video-encoder] open " + path + " " + std::to_string(w) + "x" +
                     std::to_string(h) + "@" + std::to_string(fps) + "fps"
#if PM_HAS_AUDIO_IO
                     + " audio=" +
                     std::string(audio && pm::audio::capture_source_active(audio->capture)
                                     ? pm::audio::capture_source_name(audio->capture.source)
                                     : "none")
#endif
        );

        const std::wstring wpath = widen(path);
        HRESULT hr = MFCreateSinkWriterFromURL(wpath.c_str(), nullptr, nullptr, &sink);
        if (FAILED(hr)) {
            err = "MFCreateSinkWriterFromURL failed (check extension/path)";
            pm::video::log_error("[video-encoder] " + err);
            return false;
        }

        IMFMediaType* outT = nullptr;
        hr = MFCreateMediaType(&outT);
        if (SUCCEEDED(hr)) {
            outT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            outT->SetGUID(MF_MT_SUBTYPE,    MFVideoFormat_H264);
            MFSetAttributeSize (outT, MF_MT_FRAME_SIZE, (UINT32)w, (UINT32)h);
            MFSetAttributeRatio(outT, MF_MT_FRAME_RATE, (UINT32)fps, 1);
            outT->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            // ~0.15 bits/px/frame, clamped to a sane window.
            const UINT32 bitrate = (UINT32)std::clamp<long long>(
                (long long)w * h * fps / 8, 1'000'000LL, 24'000'000LL);
            outT->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
            hr = sink->AddStream(outT, &stream);
            outT->Release();
        }
        if (FAILED(hr)) {
            err = "configure H.264 output type failed";
            pm::video::log_error("[video-encoder] " + err);
            finalize_quiet();
            return false;
        }

        IMFMediaType* inT = nullptr;
        hr = MFCreateMediaType(&inT);
        if (SUCCEEDED(hr)) {
            inT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            inT->SetGUID(MF_MT_SUBTYPE,    MFVideoFormat_RGB32);
            MFSetAttributeSize (inT, MF_MT_FRAME_SIZE, (UINT32)w, (UINT32)h);
            MFSetAttributeRatio(inT, MF_MT_FRAME_RATE, (UINT32)fps, 1);
            inT->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            inT->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)(w * 4));   // positive = top-down
            hr = sink->SetInputMediaType(stream, inT, nullptr);
            inT->Release();
        }
        if (FAILED(hr)) {
            err = "configure RGB32 input type failed";
            pm::video::log_error("[video-encoder] " + err);
            finalize_quiet();
            return false;
        }

#if PM_HAS_AUDIO_IO
        audio_mux.reset();
        audio_enabled = false;
        audio_stream = 0;
        if (audio && pm::audio::capture_source_active(audio->capture)) {
            audio_mux = std::make_unique<MfVideoAudioMux>();
            VideoAudioMuxConfig mux_cfg{audio->capture};
#if defined(FEATURE_AUDIO_FILTER) && FEATURE_AUDIO_FILTER
            mux_cfg.filter = audio->filter;
            mux_cfg.filter_enabled = audio->filter_enabled;
#endif
            if (!audio_mux->prepare_stream(sink, audio_stream, mux_cfg, err)) {
                pm::video::log_error("[video-encoder] audio prepare_stream failed: " + err);
                finalize_quiet();
                return false;
            }
            audio_enabled = true;
        }
#endif

        hr = sink->BeginWriting();
        if (FAILED(hr)) {
            err = "IMFSinkWriter::BeginWriting failed";
            pm::video::log_error("[video-encoder] " + err);
            finalize_quiet();
            return false;
        }

#if PM_HAS_AUDIO_IO
        if (audio_enabled && audio_mux) {
            // Share one timeline origin with audio so the first video frame is not
            // re-anchored to PTS 0 after audio has already been writing.
            audio_t0 = std::chrono::steady_clock::now();
            t0 = audio_t0;
            have_t0 = true;
            VideoAudioMuxConfig mux_cfg{audio->capture};
#if defined(FEATURE_AUDIO_FILTER) && FEATURE_AUDIO_FILTER
            mux_cfg.filter = audio->filter;
            mux_cfg.filter_enabled = audio->filter_enabled;
#endif
            if (!audio_mux->start_capture(sink, audio_stream, mux_cfg, audio_t0, err)) {
                pm::video::log_error("[video-encoder] audio start_capture failed: " + err);
                finalize_quiet();
                return false;
            }
            pm::video::log_warn("[video-encoder] shared A/V timeline t0 set; audio stream=" +
                         std::to_string(audio_stream));
        }
#endif

        rowbuf.assign((size_t)w * h * 4, 0);
        finish_ = stop_ = false;
        worker_err_.clear();
        worker_ = std::thread([this] { run(); });
        return true;
    }

    void finalize_quiet() { std::string ignore; finalize(ignore); }

    // Pack a BGR24/RGB24 frame into the scratch BGRA buffer (MF RGB32 = BGRA).
    void pack_bgra(const VideoFrame& f) {
        const bool is_rgb = (f.format == PixelFormat::RGB24);
        const int  bpp    = 3;
        const int  src_stride = f.stride > 0 ? f.stride : f.width * bpp;
        for (int y = 0; y < h; ++y) {
            const uint8_t* s = f.data.data() + (size_t)y * src_stride;
            uint8_t*       d = rowbuf.data()  + (size_t)y * w * 4;
            for (int x = 0; x < w; ++x) {
                const uint8_t c0 = s[x * 3 + 0];
                const uint8_t c1 = s[x * 3 + 1];
                const uint8_t c2 = s[x * 3 + 2];
                if (is_rgb) { d[x*4+0] = c2; d[x*4+1] = c1; d[x*4+2] = c0; }  // RGB→BGRA
                else        { d[x*4+0] = c0; d[x*4+1] = c1; d[x*4+2] = c2; }  // BGR→BGRA
                d[x*4+3] = 255;
            }
        }
    }

    // Producer side: validate, then hand a copy to the encode worker. Blocks only
    // when the encoder is kCap frames behind (back-pressure). Returns a previously
    // deferred encode error if the worker has failed.
    bool write(const VideoFrame& f, std::string& err) {
        if (!sink) { err = "encoder not open"; return false; }
        if (f.data.empty() || f.width <= 0 || f.height <= 0) { err = "empty frame"; return false; }
        if (f.format != PixelFormat::BGR24 && f.format != PixelFormat::RGB24) {
            err = "encoder requires BGR24/RGB24 frames"; return false;
        }
        if (!rgb_frame_fits(f)) { err = "frame buffer does not cover the image"; return false; }
        if ((f.width & ~1) < w || (f.height & ~1) < h) {
            err = "frame smaller than encoder size"; return false;
        }

        std::unique_lock<std::mutex> lk(qmtx_);
        if (!worker_err_.empty()) { err = worker_err_; return false; }
        not_full_.wait(lk, [this] { return queue_.size() < kCap || stop_; });
        if (stop_) { err = worker_err_.empty() ? "encoder stopped" : worker_err_; return false; }
        // Stamp the capture instant here (producer side) so wall-clock PTS reflects
        // capture time, not when the worker gets around to encoding it.
        queue_.push_back(Queued{f, std::chrono::steady_clock::now()});  // copy: caller retains ownership
        lk.unlock();
        not_empty_.notify_one();
        return true;
    }

    // Worker loop: pop → encode in order → repeat; exit on drain(finish_)/abort.
    void run() {
        ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;) {
            Queued q;
            {
                std::unique_lock<std::mutex> lk(qmtx_);
                not_empty_.wait(lk, [this] { return !queue_.empty() || finish_ || stop_; });
                if (queue_.empty()) break;          // drained + finish_/stop_
                if (stop_) break;                    // abort: drop remaining
                q = std::move(queue_.front());
                queue_.pop_front();
            }
            not_full_.notify_one();
            std::string e;
            if (!encode_one(q.f, q.cap, e)) {
                std::lock_guard<std::mutex> lk(qmtx_);
                if (worker_err_.empty()) worker_err_ = e;
                stop_ = true;                        // stop accepting / encoding
                not_full_.notify_all();
                break;
            }
        }
        ::CoUninitialize();
    }

    bool encode_one(const VideoFrame& f, std::chrono::steady_clock::time_point cap,
                    std::string& err) {
        pack_bgra(f);

        const DWORD nbytes = (DWORD)w * h * 4;
        IMFMediaBuffer* buf = nullptr;
        if (FAILED(MFCreateMemoryBuffer(nbytes, &buf))) { err = "MFCreateMemoryBuffer"; return false; }
        BYTE* dst = nullptr; DWORD maxlen = 0, curlen = 0;
        if (FAILED(buf->Lock(&dst, &maxlen, &curlen))) { buf->Release(); err = "buffer Lock"; return false; }
        std::memcpy(dst, rowbuf.data(), nbytes);
        buf->Unlock();
        buf->SetCurrentLength(nbytes);

        IMFSample* smp = nullptr;
        if (FAILED(MFCreateSample(&smp))) { buf->Release(); err = "MFCreateSample"; return false; }
        smp->AddBuffer(buf);

        // Presentation timestamp (100ns units). Transcode = constant-rate grid
        // (frame_idx / fps); live = real elapsed wall time anchored at first frame.
        LONGLONG ts, dur;
        if (constant_rate) {
            ts  = frame_idx * frame_dur;
            dur = frame_dur;
        } else {
            const auto now = cap;   // capture instant (stamped on the producer side)
            if (!have_t0) { t0 = now; have_t0 = true; }
            ts = std::chrono::duration_cast<std::chrono::duration<LONGLONG, std::ratio<1, 10'000'000>>>(now - t0).count();
            if (ts <= last_ts) ts = last_ts + 1;   // MF requires strictly increasing PTS
            dur = (frame_idx == 0) ? frame_dur : std::max<LONGLONG>(ts - last_ts, 1);
        }
        smp->SetSampleTime(ts);
        smp->SetSampleDuration(dur);
        last_ts = ts;
#if PM_HAS_AUDIO_IO
        // IMFSinkWriter is not thread-safe across audio callback + encode worker.
        std::unique_lock<std::mutex> sink_lk;
        if (audio_mux) {
            sink_lk = std::unique_lock<std::mutex>(audio_mux->sink_write_mutex());
            audio_mux->arm_video_sync(cap);
        }
#endif
        const HRESULT hr = sink->WriteSample(stream, smp);
        smp->Release();
        buf->Release();
        if (FAILED(hr)) {
            char b[16];
            std::snprintf(b, sizeof(b), "0x%08lx", static_cast<unsigned long>(hr));
            err = std::string("IMFSinkWriter::WriteSample failed hr=") + b;
            pm::video::log_error("[video-encoder] " + err +
                          " frame=" + std::to_string(frame_idx) +
                          " pts_ms=" + std::to_string(ts / 10000));
            return false;
        }
        if (frame_idx == 0) {
            pm::video::log_warn("[video-encoder] first video sample pts_ms=" +
                         std::to_string(ts / 10000) +
                         " have_t0=" + (have_t0 ? "1" : "0"));
        }
        ++frame_idx;
        return true;
    }
};

VideoEncoder::VideoEncoder() : m_(std::make_unique<Impl>()) {}
VideoEncoder::~VideoEncoder() = default;

bool VideoEncoder::open(const std::string& path, int width, int height, int fps, std::string& err
#if PM_HAS_AUDIO_IO
                        , const VideoAudioOptions* audio
#endif
) {
#if PM_HAS_AUDIO_IO
    return m_->open(path, width, height, fps, err, audio);
#else
    return m_->open(path, width, height, fps, err);
#endif
}
bool VideoEncoder::is_open() const noexcept { return m_->sink != nullptr; }

void VideoEncoder::set_constant_rate(bool on) noexcept { m_->constant_rate = on; }

bool VideoEncoder::write(const VideoFrame& frame, std::string& err) {
    return m_->write(frame, err);
}

bool VideoEncoder::close(std::string& err) {
    const bool ok = m_->finalize(err);
    if (m_->mf) { mf_release(); m_->mf = false; }
    return ok;
}

#else  // ── Non-Windows stub ───────────────────────────────────────────────────

struct VideoEncoder::Impl {};
VideoEncoder::VideoEncoder() : m_(std::make_unique<Impl>()) {}
VideoEncoder::~VideoEncoder() = default;
bool VideoEncoder::open(const std::string&, int, int, int, std::string& err) {
    err = "video encoding not implemented on this platform";
    return false;
}
bool VideoEncoder::is_open() const noexcept { return false; }
void VideoEncoder::set_constant_rate(bool) noexcept {}
bool VideoEncoder::write(const VideoFrame&, std::string& err) {
    err = "video encoding not implemented on this platform";
    return false;
}
bool VideoEncoder::close(std::string&) { return true; }

#endif

} // namespace pm::video

#endif // FEATURE_VIDEO
