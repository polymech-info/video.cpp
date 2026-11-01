#if defined(FEATURE_VIDEO) && FEATURE_VIDEO && defined(_WIN32)

// ── Win32 frame pump backend ────────────────────────────────────────────────────
// Implements pm::video::make_frame_pump() for Windows:
//   • FileFramePump  — decode a video container with Media Foundation
//                      (MFCreateSourceReaderFromURL → RGB32 → packed BGR24).
//   • LiveFramePump  — adapt the push-based make_frame_source() (webcam / screen)
//                      to a pull interface via a bounded frame queue.
//   • ImageFramePump — one-shot decode of a still image via load_frame().
// No OpenCV. Links: mf mfplat mfreadwrite mfuuid (already on the video targets).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>

#include "pm/video/frame_pump.hpp"
#include "pm/video/frame_source.hpp"  // make_frame_source (webcam / screen)
#include "pm/video/screen.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace pm::video {
namespace {

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    return w;
}

HRESULT ensure_mf() {
    static std::once_flag once;
    static HRESULT hr = S_OK;
    std::call_once(once, [] { hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET); });
    return hr;
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline uint8_t clip8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : static_cast<uint8_t>(v)); }

// Convert NV12 (Y plane + interleaved UV, BT.601 limited range) → packed BGR24.
// `y_stride` is the Y-plane row stride in bytes (chroma plane shares it, half the
// rows, immediately after the Y plane). This is the decoder-native path: no
// Video Processor MFT, so decode is the cheap thing and we own the color math.
void nv12_to_bgr24(const BYTE* base, int y_stride, int w, int h, VideoFrame& f) {
    const BYTE* Y  = base;
    const BYTE* UV = base + static_cast<size_t>(y_stride) * h;
    f.width        = w;
    f.height       = h;
    f.stride       = w * 3;
    f.format       = PixelFormat::BGR24;
    f.timestamp_ms = now_ms();
    f.data.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    for (int y = 0; y < h; ++y) {
        const BYTE* yr  = Y  + static_cast<size_t>(y) * y_stride;
        const BYTE* uvr = UV + static_cast<size_t>(y / 2) * y_stride;
        uint8_t*    dst = f.data.data() + static_cast<size_t>(y) * f.stride;
        for (int x = 0; x < w; ++x) {
            const int C = static_cast<int>(yr[x]) - 16;
            const int uvx = x & ~1;
            const int U = static_cast<int>(uvr[uvx])     - 128;
            const int V = static_cast<int>(uvr[uvx + 1]) - 128;
            const int R = (298 * C + 409 * V + 128) >> 8;
            const int G = (298 * C - 100 * U - 208 * V + 128) >> 8;
            const int B = (298 * C + 516 * U + 128) >> 8;
            dst[x * 3 + 0] = clip8(B);
            dst[x * 3 + 1] = clip8(G);
            dst[x * 3 + 2] = clip8(R);
        }
    }
}

// Copy a locked RGB32 (BGRX, possibly bottom-up) scanline buffer → packed BGR24.
void rgb32_to_bgr24(const BYTE* scan0, LONG pitch, int w, int h, VideoFrame& f) {
    const int abs_pitch = pitch < 0 ? -pitch : pitch;
    f.width        = w;
    f.height       = h;
    f.stride       = w * 3;
    f.format       = PixelFormat::BGR24;
    f.timestamp_ms = now_ms();
    f.data.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    for (int row = 0; row < h; ++row) {
        const BYTE* src = (pitch >= 0)
            ? (scan0 + static_cast<ptrdiff_t>(row) * pitch)
            : (scan0 + static_cast<ptrdiff_t>(h - 1 - row) * abs_pitch);
        uint8_t* dst = f.data.data() + static_cast<size_t>(row) * f.stride;
        for (int x = 0; x < w; ++x) {
            dst[x * 3 + 0] = src[x * 4 + 0]; // B
            dst[x * 3 + 1] = src[x * 4 + 1]; // G
            dst[x * 3 + 2] = src[x * 4 + 2]; // R
        }
    }
}

// ── ImageFramePump: a single decoded still, then EOF ────────────────────────────
class ImageFramePump final : public IFramePump {
public:
    bool open(const std::string& path, std::string& err) {
        if (!load_frame(path, frame_, err)) return false;
        path_ = path;
        return true;
    }
    bool next(VideoFrame& out, std::string& err) override {
        (void)err;
        if (done_) return false;
        out  = frame_;
        done_ = true;
        ++count_;
        return true;
    }
    std::string describe() const override {
        return "image " + path_ + " " + std::to_string(frame_.width) + "x" +
               std::to_string(frame_.height);
    }
    long long frames_read() const override { return count_; }

private:
    VideoFrame  frame_;
    std::string path_;
    bool        done_  = false;
    long long   count_ = 0;
};

// ── FileFramePump: Media Foundation video-file decode → BGR24 ────────────────────
// Decodes on a background worker (ReadSample blocks while MF decodes) into a small
// bounded queue, so decode + our colour-convert overlap the consumer's filter +
// encode work. Output format is the decoder-native NV12 (no Video Processor MFT;
// we do the YUV→BGR math), with an RGB32 fallback for sources that won't give NV12.
class FileFramePump final : public IFramePump {
public:
    explicit FileFramePump(const FramePumpOptions& o) : opt_(o) {}
    ~FileFramePump() override {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            quit_ = true;
        }
        not_full_.notify_all();
        not_empty_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    bool open(const std::string& path, std::string& err) {
        // The IMFSourceReader is created AND driven on the worker thread (MTA), so
        // ReadSample never marshals across COM apartments back to the caller's
        // thread — that marshaling is what serialized decode against the consumer's
        // filter/encode work. open() just launches the worker and blocks until it
        // has negotiated the format (so describe()/source_fps()/dims are valid).
        path_     = path;
        start_ms_ = now_ms();
        worker_   = std::thread([this] { run(); });

        std::unique_lock<std::mutex> lk(mtx_);
        ready_cv_.wait(lk, [this] { return ready_ || !worker_err_.empty(); });
        if (!worker_err_.empty()) { err = worker_err_; return false; }
        return true;
    }

    // Create + configure the reader (worker thread). Returns false + sets err.
    bool open_reader(std::string& err) {
        if (FAILED(ensure_mf())) { err = "MFStartup failed"; return false; }

        ComPtr<IMFAttributes> attr;
        if (SUCCEEDED(MFCreateAttributes(&attr, 1)))
            attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

        const std::wstring wpath = to_wide(path_);
        if (FAILED(MFCreateSourceReaderFromURL(wpath.c_str(), attr.Get(), &reader_))) {
            err = "cannot open video: " + path_;
            return false;
        }
        reader_->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        reader_->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

        // Prefer NV12 (decoder-native → no colour-convert MFT); fall back to RGB32.
        if (set_subtype(MFVideoFormat_NV12))       nv12_ = true;
        else if (set_subtype(MFVideoFormat_RGB32)) nv12_ = false;
        else { err = "video has no decodable video stream: " + path_; return false; }

        return refresh_format(err);
    }

    bool next(VideoFrame& out, std::string& err) override {
        std::unique_lock<std::mutex> lk(mtx_);
        for (;;) {
            if (!queue_.empty()) {
                out = std::move(queue_.front());
                queue_.pop_front();
                lk.unlock();
                not_full_.notify_one();
                ++delivered_;
                return true;
            }
            if (!worker_err_.empty()) { err = worker_err_; return false; }
            if (done_) return false;          // EOF / stop and drained
            not_empty_.wait(lk);
        }
    }

    std::string describe() const override {
        return "video " + path_ + " " + std::to_string(w_) + "x" + std::to_string(h_) +
               (nv12_ ? " nv12" : " rgb32");
    }
    long long frames_read() const override { return delivered_; }
    double source_fps() const override { return src_fps_; }

private:
    bool set_subtype(const GUID& sub) {
        ComPtr<IMFMediaType> want;
        if (FAILED(MFCreateMediaType(&want))) return false;
        want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        want->SetGUID(MF_MT_SUBTYPE,    sub);
        return SUCCEEDED(reader_->SetCurrentMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, want.Get()));
    }

    bool refresh_format(std::string& err) {
        ComPtr<IMFMediaType> cur;
        if (FAILED(reader_->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur))) {
            err = "GetCurrentMediaType failed";
            return false;
        }
        UINT32 w = 0, h = 0;
        if (FAILED(MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h)) || w == 0 || h == 0) {
            err = "video frame size unknown";
            return false;
        }
        w_ = static_cast<int>(w);
        h_ = static_cast<int>(h);
        UINT32 stride = 0;
        if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) && static_cast<INT32>(stride) > 0)
            y_stride_ = static_cast<int>(stride);
        else
            y_stride_ = nv12_ ? w_ : w_ * 4;  // sane default if unspecified
        UINT32 num = 0, den = 0;
        if (SUCCEEDED(MFGetAttributeRatio(cur.Get(), MF_MT_FRAME_RATE, &num, &den)) && den > 0)
            src_fps_ = static_cast<double>(num) / static_cast<double>(den);
        return true;
    }

    bool stop_requested(long long produced) const {
        if (opt_.max_frames > 0 && produced >= opt_.max_frames) return true;
        if (opt_.duration_ms > 0 && now_ms() - start_ms_ >= opt_.duration_ms) return true;
        if (opt_.cancel && opt_.cancel()) return true;
        return false;
    }

    bool decode_sample(IMFSample* sample, VideoFrame& f) {
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf))) return false;
        bool ok = false;
        if (nv12_) {
            BYTE* p = nullptr; DWORD len = 0;
            if (SUCCEEDED(buf->Lock(&p, nullptr, &len))) {
                nv12_to_bgr24(p, y_stride_, w_, h_, f);
                buf->Unlock();
                ok = true;
            }
        } else {
            ComPtr<IMF2DBuffer> b2d;
            if (SUCCEEDED(buf.As(&b2d))) {
                BYTE* scan0 = nullptr; LONG pitch = 0;
                if (SUCCEEDED(b2d->Lock2D(&scan0, &pitch))) {
                    rgb32_to_bgr24(scan0, pitch, w_, h_, f);
                    b2d->Unlock2D();
                    ok = true;
                }
            }
            if (!ok) {
                BYTE* p = nullptr; DWORD len = 0;
                if (SUCCEEDED(buf->Lock(&p, nullptr, &len))) {
                    rgb32_to_bgr24(p, w_ * 4, w_, h_, f);  // assume top-down
                    buf->Unlock();
                    ok = true;
                }
            }
        }
        return ok;
    }

    // Worker: pull → decode → push, blocking when the queue is full (back-pressure)
    // and stopping on EOF / stop condition / pump destruction.
    void run() {
        ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        // Create + negotiate the reader on this (MTA) thread, then unblock open().
        std::string oerr;
        const bool reader_ok = open_reader(oerr);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!reader_ok && worker_err_.empty()) worker_err_ = oerr;
            ready_ = true;
        }
        ready_cv_.notify_all();
        if (!reader_ok) {
            { std::lock_guard<std::mutex> lk(mtx_); done_ = true; }
            not_empty_.notify_all();
            reader_.Reset();
            ::CoUninitialize();
            return;
        }

        long long produced = 0;
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (quit_) break;
            }
            if (stop_requested(produced)) break;

            DWORD     flags = 0;
            LONGLONG  ts    = 0;
            ComPtr<IMFSample> sample;
            HRESULT hr = reader_->ReadSample(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &sample);
            if (FAILED(hr)) { set_error("ReadSample failed"); break; }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;  // clean EOF
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                std::string serr;
                if (!refresh_format(serr)) { set_error(serr); break; }
            }
            if (flags & MF_SOURCE_READERF_STREAMTICK) continue;
            if (!sample) continue;

            VideoFrame f;
            if (!decode_sample(sample.Get(), f)) continue;
            f.timestamp_ms = ts / 10000;  // 100ns → ms

            std::unique_lock<std::mutex> lk(mtx_);
            not_full_.wait(lk, [this] { return quit_ || queue_.size() < kCap; });
            if (quit_) break;
            queue_.push_back(std::move(f));
            lk.unlock();
            not_empty_.notify_one();
            ++produced;
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            done_ = true;
        }
        not_empty_.notify_all();
        reader_.Reset();   // release the reader on the thread that created it
        ::CoUninitialize();
    }

    void set_error(std::string e) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (worker_err_.empty()) worker_err_ = std::move(e);
    }

    static constexpr size_t kCap = 8;  // decode-ahead depth

    FramePumpOptions        opt_;
    ComPtr<IMFSourceReader> reader_;
    std::string             path_;
    bool                    nv12_     = true;
    int                     w_ = 0, h_ = 0, y_stride_ = 0;
    double                  src_fps_  = 0.0;
    int64_t                 start_ms_ = 0;

    std::thread             worker_;
    std::mutex              mtx_;
    std::condition_variable not_empty_, not_full_, ready_cv_;
    std::deque<VideoFrame>  queue_;
    std::string             worker_err_;
    bool                    ready_     = false;  // format negotiated (open() gate)
    bool                    done_      = false;
    bool                    quit_      = false;
    long long               delivered_ = 0;
};

// ── LiveFramePump: push (make_frame_source) adapted to pull ──────────────────────
class LiveFramePump final : public IFramePump {
public:
    explicit LiveFramePump(const FramePumpOptions& o) : opt_(o) {}
    ~LiveFramePump() override { if (src_) src_->stop(); }

    bool open(const std::string& spec, std::string& err) {
        try {
            if (is_screen_input_spec(spec)) {
                ScreenSpec ss;
                if (!parse_screen_spec(spec, ss)) {
                    err = "bad screen input spec: " + spec;
                    return false;
                }
                ss.include_cursor = opt_.include_cursor;
                src_ = std::make_unique<ScreenSource>(std::move(ss));
            } else {
                src_ = make_frame_source(spec);
            }
            if (!src_) { err = "no frame source for: " + spec; return false; }
            src_->start(opt_.width, opt_.height, opt_.fps,
                        [this](const VideoFrame& f) { push(f); });
        } catch (const std::exception& ex) {
            err = std::string("open live source failed: ") + ex.what();
            return false;
        }
        spec_     = spec;
        start_ms_ = now_ms();
        return true;
    }

    bool next(VideoFrame& out, std::string& err) override {
        (void)err;
        std::unique_lock<std::mutex> lk(mtx_);
        for (;;) {
            if (opt_.max_frames > 0 && count_ >= opt_.max_frames) return false;
            if (opt_.duration_ms > 0 && now_ms() - start_ms_ >= opt_.duration_ms) return false;
            if (opt_.cancel && opt_.cancel()) return false;

            if (!queue_.empty()) {
                out = std::move(queue_.front());
                queue_.pop_front();
                ++count_;
                return true;
            }
            // Wait briefly so stop conditions (duration / cancel) stay responsive.
            cv_.wait_for(lk, std::chrono::milliseconds(50));
        }
    }

    std::string describe() const override {
        return src_ ? src_->description() : ("live " + spec_);
    }
    long long frames_read() const override { return count_; }
    double source_fps() const override { return opt_.fps > 0 ? static_cast<double>(opt_.fps) : 0.0; }

private:
    void push(const VideoFrame& f) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            queue_.push_back(f);
            while (queue_.size() > kQueueCap) queue_.pop_front();  // drop oldest
        }
        cv_.notify_one();
    }

    static constexpr size_t kQueueCap = 4;  // small: prefer latest frames (low latency)

    FramePumpOptions        opt_;
    FrameSourcePtr          src_;
    std::string             spec_;
    std::mutex              mtx_;
    std::condition_variable cv_;
    std::deque<VideoFrame>  queue_;
    int64_t                 start_ms_ = 0;
    long long               count_    = 0;
};

} // namespace

std::unique_ptr<IFramePump> make_frame_pump(const std::string& spec,
                                            const FramePumpOptions& opts,
                                            std::string& err) {
    // Route by spec: video file → MF decoder, image file → one-shot, else live.
    const bool is_screen = is_screen_input_spec(spec);
    if (!spec.empty() && !is_screen) {
        if (is_video_file_spec(spec)) {
            auto p = std::make_unique<FileFramePump>(opts);
            if (!p->open(spec, err)) return nullptr;
            return p;
        }
        std::error_code ec;
        if (std::filesystem::is_regular_file(spec, ec)) {
            auto p = std::make_unique<ImageFramePump>();
            if (!p->open(spec, err)) return nullptr;
            return p;
        }
    }
    auto p = std::make_unique<LiveFramePump>(opts);
    if (!p->open(spec, err)) return nullptr;
    return p;
}

} // namespace pm::video

#endif // FEATURE_VIDEO && _WIN32
