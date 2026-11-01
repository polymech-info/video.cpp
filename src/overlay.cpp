#include "pm/video/overlay.hpp"

#if defined(_WIN32)

#if defined(FEATURE_VIDEO) && FEATURE_VIDEO
#include "pm/video/capture.hpp"
#include "pm/video/hooks.hpp"
#if defined(FEATURE_VIDEO_BEAUTY) && FEATURE_VIDEO_BEAUTY
#include "video/filters/beauty_processor.hpp"
#endif
#endif

#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "Gdiplus.lib")

namespace pm::video {
namespace {

constexpr UINT kPaintMessage = WM_APP + 0x431;
constexpr UINT kAnchorMessage = WM_APP + 0x432;
constexpr UINT kStopMessage = WM_APP + 0x433;
constexpr wchar_t kClassName[] = L"PmCameraOverlayPip";

#if defined(FEATURE_VIDEO) && FEATURE_VIDEO
inline std::string lower_ascii_copy(std::string s)
{
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

inline double mode_fps(const DeviceMode& m) noexcept
{
    if (m.fps_den <= 0) return 0.0;
    return static_cast<double>(m.fps_num) / static_cast<double>(m.fps_den);
}

inline int mode_fps_rounded(const DeviceMode& m) noexcept
{
    if (m.fps_den <= 0) return 30;
    return std::max(1, (m.fps_num + m.fps_den / 2) / m.fps_den);
}

inline bool parse_resolution_token(
    std::string token, int& want_w, int& want_h, int& want_fps, bool& want_max)
{
    want_w = 0;
    want_h = 0;
    want_fps = 0;
    want_max = false;
    token = lower_ascii_copy(std::move(token));
    while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
        token.erase(token.begin());
    while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
        token.pop_back();
    if (token.empty() || token == "auto") return true;
    if (token == "max" || token == "highest" || token == "native-max") {
        want_max = true;
        return true;
    }
    if (token == "720p" || token == "hd") {
        want_w = 1280;
        want_h = 720;
        return true;
    }
    if (token == "1080p" || token == "fhd") {
        want_w = 1920;
        want_h = 1080;
        return true;
    }
    if (token == "1440p" || token == "qhd") {
        want_w = 2560;
        want_h = 1440;
        return true;
    }
    if (token == "4k" || token == "2160p" || token == "uhd") {
        want_w = 3840;
        want_h = 2160;
        return true;
    }

    // WxH or WxH@fps (also accepts *)
    for (char& c : token) {
        if (c == '*' || c == 'X') c = 'x';
    }
    int w = 0, h = 0, fps = 0;
    const char* p = token.c_str();
    char* end = nullptr;
    w = static_cast<int>(std::strtol(p, &end, 10));
    if (!end || *end != 'x') return false;
    h = static_cast<int>(std::strtol(end + 1, &end, 10));
    if (w <= 0 || h <= 0) return false;
    if (end && *end == '@') {
        fps = static_cast<int>(std::strtol(end + 1, &end, 10));
        if (fps <= 0) return false;
    }
    want_w = w;
    want_h = h;
    want_fps = fps;
    return true;
}

/// Pick a native device media type from @p resolution:
///   auto — enough pixels for ≈2× on-screen PiP near 30 fps
///   max  — highest-area native mode (fps near 30 preferred)
///   WxH / WxH@fps / 720p / 1080p / 4k — nearest matching native mode
inline void choose_pip_native_mode(
    const std::string& device,
    int pip_px,
    const std::string& resolution,
    int& out_w,
    int& out_h,
    int& out_fps)
{
    out_w = 1280;
    out_h = 720;
    out_fps = 30;

    const auto modes = enumerate_device_modes(device);
    if (modes.empty()) return;

    int want_w = 0, want_h = 0, want_fps = 0;
    bool want_max = false;
    if (!parse_resolution_token(resolution, want_w, want_h, want_fps, want_max)) {
        // Bad token → behave like auto.
        want_w = want_h = want_fps = 0;
        want_max = false;
    }

    const DeviceMode* best = nullptr;
    double best_score = 1e300;

    if (want_max) {
        for (const auto& m : modes) {
            if (m.width <= 0 || m.height <= 0 || m.fps_den <= 0) continue;
            const double fps = mode_fps(m);
            const double area = static_cast<double>(m.width) * m.height;
            const double fps_diff = fps - 30.0;
            // Prefer larger area; mild preference for ~30 fps when areas tie.
            const double score = -area + fps_diff * fps_diff * 10.0;
            if (score < best_score) {
                best_score = score;
                best = &m;
            }
        }
    } else if (want_w > 0 && want_h > 0) {
        for (const auto& m : modes) {
            if (m.width <= 0 || m.height <= 0 || m.fps_den <= 0) continue;
            const double fps = mode_fps(m);
            const double dw = static_cast<double>(m.width - want_w);
            const double dh = static_cast<double>(m.height - want_h);
            double score = dw * dw + dh * dh;
            if (want_fps > 0) {
                const double diff = fps - static_cast<double>(want_fps);
                score += diff * diff * 40.0;
            } else {
                const double diff = fps - 30.0;
                score += diff * diff * 0.05;
            }
            if (score < best_score) {
                best_score = score;
                best = &m;
            }
        }
    } else {
        // auto — PiP-sized pick
        const int want_short =
            std::clamp(pip_px > 0 ? pip_px * 2 : 720, 480, 1080);
        for (const auto& m : modes) {
            if (m.width <= 0 || m.height <= 0 || m.fps_den <= 0) continue;
            const int short_edge = std::min(m.width, m.height);
            const double fps = mode_fps(m);
            const double size_pen = short_edge >= want_short
                ? (short_edge - want_short) * 0.35
                : (want_short - short_edge) * 4.0;
            const double fps_diff = fps - 30.0;
            const double fps_pen = fps_diff * fps_diff * 0.05;
            const double area_pen =
                static_cast<double>(m.width) * static_cast<double>(m.height) *
                1e-9;
            const double score = size_pen + fps_pen + area_pen;
            if (score < best_score) {
                best_score = score;
                best = &m;
            }
        }
    }

    if (!best) return;
    out_w = best->width;
    out_h = best->height;
    out_fps = mode_fps_rounded(*best);
}
#endif

struct LayerBuf {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HBITMAP old_bmp = nullptr;
    void* bits = nullptr;
    int w = 0;
    int h = 0;

    bool ensure(int nw, int nh)
    {
        if (dc && bmp && nw == w && nh == h) return true;
        free();
        if (nw <= 0 || nh <= 0) return false;
        HDC screen = ::GetDC(nullptr);
        if (!screen) return false;
        dc = ::CreateCompatibleDC(screen);
        ::ReleaseDC(nullptr, screen);
        if (!dc) return false;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = nw;
        bi.bmiHeader.biHeight = -nh;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bmp = ::CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp || !bits) {
            free();
            return false;
        }
        old_bmp = static_cast<HBITMAP>(::SelectObject(dc, bmp));
        w = nw;
        h = nh;
        return true;
    }

    void free()
    {
        if (dc) {
            if (old_bmp) {
                ::SelectObject(dc, old_bmp);
                old_bmp = nullptr;
            }
            ::DeleteDC(dc);
            dc = nullptr;
        }
        if (bmp) {
            ::DeleteObject(bmp);
            bmp = nullptr;
        }
        bits = nullptr;
        w = h = 0;
    }
};

bool frame_has_visible_content(
    const std::uint8_t* bgr, int w, int h, int stride) noexcept
{
    if (!bgr || w <= 0 || h <= 0) return false;
    // Sample a sparse grid; skip pure-black / near-black warmup frames.
    int lit = 0;
    int samples = 0;
    for (int y = h / 8; y < h; y += std::max(1, h / 6)) {
        const auto* row = bgr + static_cast<std::size_t>(y) * stride;
        for (int x = w / 8; x < w; x += std::max(1, w / 6)) {
            const auto* p = row + x * 3;
            ++samples;
            if (p[0] > 18 || p[1] > 18 || p[2] > 18) ++lit;
        }
    }
    return samples > 0 && lit * 10 >= samples; // ≥10% lit samples
}

Gdiplus::Color argb_from_rgb(std::uint32_t rgb, BYTE a = 255) noexcept
{
    return Gdiplus::Color(
        a,
        static_cast<BYTE>((rgb >> 16) & 0xFF),
        static_cast<BYTE>((rgb >> 8) & 0xFF),
        static_cast<BYTE>(rgb & 0xFF));
}

} // namespace

struct WinCameraOverlayController::Impl {
    CameraOverlayConfig options;
    RECT anchor{};
    bool anchor_valid = false;

    std::mutex api_mutex;
    std::mutex frame_mutex;
    std::mutex init_mutex;
    std::condition_variable init_cv;
    std::thread ui_thread;

    std::atomic<bool> is_running{false};
    std::atomic<DWORD> ui_thread_id{0};
    bool init_done = false;
    bool init_ok = false;
    std::string init_error;

    HWND window = nullptr;
    LayerBuf layer;
    ULONG_PTR gdip_token = 0;

    int pip_x = 0;
    int pip_y = 0;
    int pip_size = 0;

    int frame_w = 0;
    int frame_h = 0;
    int frame_stride = 0;
    std::vector<std::uint8_t> frame_bgr;
    bool frame_ready = false;
    bool paint_pending = false;

#if defined(FEATURE_VIDEO) && FEATURE_VIDEO
    std::unique_ptr<VideoInput> camera;
#if defined(FEATURE_VIDEO_BEAUTY) && FEATURE_VIDEO_BEAUTY
    BeautyProcessor beauty;
    bool beauty_on = false;
    std::int64_t beauty_prev_ts = -1;
    bool beauty_fail_logged = false;
#endif
#endif

    static LRESULT CALLBACK window_proc(
        HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            ::SetWindowLongPtrW(
                hwnd, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* self = reinterpret_cast<Impl*>(
            ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (message) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        default:
            break;
        }
        return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }

    static bool register_class()
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &Impl::window_proc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.lpszClassName = kClassName;
        if (::RegisterClassExW(&wc)) return true;
        return ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    void compute_layout()
    {
        if (!anchor_valid) return;
        const int aw = anchor.right - anchor.left;
        const int ah = anchor.bottom - anchor.top;
        camera_overlay_layout_rect(
            anchor.left, anchor.top, aw, ah, options,
            pip_x, pip_y, pip_size);
    }

    bool create_window(std::string& error)
    {
        if (!register_class()) {
            error = "failed to register camera overlay window class";
            return false;
        }
        compute_layout();
        // Extra padding for soft shadow outside the PiP square.
        const int pad = options.shadow ? std::max(12, pip_size / 10) : 4;
        const int win_w = pip_size + pad * 2;
        const int win_h = pip_size + pad * 2;
        window = ::CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
                WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            kClassName, L"", WS_POPUP,
            pip_x - pad, pip_y - pad, win_w, win_h,
            nullptr, nullptr, ::GetModuleHandleW(nullptr), this);
        if (!window) {
            error = "CreateWindowExW(camera overlay) failed (" +
                    std::to_string(::GetLastError()) + ")";
            return false;
        }
        // Capture-visible — do not call SetWindowDisplayAffinity.
        return true;
    }

    void on_camera_frame_bgr(
        const std::uint8_t* data, int w, int h, int stride)
    {
        if (!data || w <= 0 || h <= 0) return;
        if (!frame_has_visible_content(data, w, h, stride)) return;
        {
            std::lock_guard<std::mutex> lock(frame_mutex);
            const std::size_t need =
                static_cast<std::size_t>(stride) * static_cast<std::size_t>(h);
            if (frame_bgr.size() != need) frame_bgr.resize(need);
            std::memcpy(frame_bgr.data(), data, need);
            frame_w = w;
            frame_h = h;
            frame_stride = stride;
            frame_ready = true;
            if (paint_pending) return;
            paint_pending = true;
        }
        const DWORD tid = ui_thread_id.load(std::memory_order_relaxed);
        if (tid) ::PostThreadMessageW(tid, kPaintMessage, 0, 0);
    }

    void paint()
    {
        std::vector<std::uint8_t> local;
        int fw = 0, fh = 0, fstride = 0;
        {
            std::lock_guard<std::mutex> lock(frame_mutex);
            paint_pending = false;
            if (!frame_ready || frame_bgr.empty()) return;
            local = frame_bgr;
            fw = frame_w;
            fh = frame_h;
            fstride = frame_stride;
        }
        if (!window || pip_size <= 0) return;

#if defined(FEATURE_VIDEO) && FEATURE_VIDEO && \
    defined(FEATURE_VIDEO_BEAUTY) && FEATURE_VIDEO_BEAUTY
        if (beauty_on && !local.empty() && fw > 0 && fh > 0) {
            VideoFrame vf;
            vf.width = fw;
            vf.height = fh;
            vf.stride = fstride > 0 ? fstride : fw * 3;
            vf.format = PixelFormat::BGR24;
            vf.data = std::move(local);
            if (!is_packed_bgr24(vf)) {
                VideoFrame packed;
                if (to_bgr24(vf, packed))
                    vf = std::move(packed);
            }
            if (is_packed_bgr24(vf)) {
                const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                std::int64_t ts = now_ms;
                if (ts <= beauty_prev_ts)
                    ts = beauty_prev_ts + 1;
                beauty_prev_ts = ts;
                std::string berr;
                if (beauty.process(vf, ts, berr)) {
                    local = std::move(vf.data);
                    fw = vf.width;
                    fh = vf.height;
                    fstride = vf.stride;
                } else if (!beauty_fail_logged) {
                    beauty_fail_logged = true;
                    pm::video::log_warn("[camera-overlay] beauty failed: " + berr);
                }
            }
        }
#endif

        const int pad = options.shadow ? std::max(12, pip_size / 10) : 4;
        const int win_w = pip_size + pad * 2;
        const int win_h = pip_size + pad * 2;
        if (!layer.ensure(win_w, win_h)) return;

        Gdiplus::Graphics g(layer.dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);

        const float ox = static_cast<float>(pad);
        const float oy = static_cast<float>(pad);
        const float sz = static_cast<float>(pip_size);

        if (options.shadow) {
            const float so = sz * 0.04f;
            Gdiplus::SolidBrush shadow(
                Gdiplus::Color(90, 0, 0, 0));
            if (options.shape == CameraOverlayShape::circle) {
                g.FillEllipse(&shadow, ox + so, oy + so, sz, sz);
            } else if (options.shape == CameraOverlayShape::rounded) {
                Gdiplus::GraphicsPath path;
                const float r = sz * 0.18f;
                path.AddArc(ox + so, oy + so, r * 2, r * 2, 180, 90);
                path.AddArc(ox + so + sz - r * 2, oy + so, r * 2, r * 2, 270, 90);
                path.AddArc(ox + so + sz - r * 2, oy + so + sz - r * 2, r * 2, r * 2, 0, 90);
                path.AddArc(ox + so, oy + so + sz - r * 2, r * 2, r * 2, 90, 90);
                path.CloseFigure();
                g.FillPath(&shadow, &path);
            } else {
                g.FillRectangle(&shadow, ox + so, oy + so, sz, sz);
            }
        }

        // Cover-fit source into the square.
        const float src_aspect =
            static_cast<float>(fw) / static_cast<float>(std::max(1, fh));
        float src_w = static_cast<float>(fw);
        float src_h = static_cast<float>(fh);
        float src_x = 0.f;
        float src_y = 0.f;
        if (src_aspect > 1.f) {
            src_w = static_cast<float>(fh);
            src_x = (static_cast<float>(fw) - src_w) * 0.5f;
        } else if (src_aspect < 1.f) {
            src_h = static_cast<float>(fw);
            src_y = (static_cast<float>(fh) - src_h) * 0.5f;
        }

        Gdiplus::Bitmap src(
            fw, fh, fstride, PixelFormat24bppRGB, local.data());
        {
            Gdiplus::GraphicsPath clip;
            if (options.shape == CameraOverlayShape::circle) {
                clip.AddEllipse(ox, oy, sz, sz);
            } else if (options.shape == CameraOverlayShape::rounded) {
                const float r = sz * 0.18f;
                clip.AddArc(ox, oy, r * 2, r * 2, 180, 90);
                clip.AddArc(ox + sz - r * 2, oy, r * 2, r * 2, 270, 90);
                clip.AddArc(ox + sz - r * 2, oy + sz - r * 2, r * 2, r * 2, 0, 90);
                clip.AddArc(ox, oy + sz - r * 2, r * 2, r * 2, 90, 90);
                clip.CloseFigure();
            } else {
                clip.AddRectangle(Gdiplus::RectF(ox, oy, sz, sz));
            }
            Gdiplus::Region region(&clip);
            g.SetClip(&region);
            g.DrawImage(
                &src,
                Gdiplus::RectF(ox, oy, sz, sz),
                src_x, src_y, src_w, src_h,
                Gdiplus::UnitPixel);
            g.ResetClip();
        }

        if (options.border) {
            Gdiplus::Pen pen(argb_from_rgb(options.border_color), 3.0f);
            pen.SetAlignment(Gdiplus::PenAlignmentInset);
            if (options.shape == CameraOverlayShape::circle) {
                g.DrawEllipse(&pen, ox + 1.5f, oy + 1.5f, sz - 3.f, sz - 3.f);
            } else if (options.shape == CameraOverlayShape::rounded) {
                const float r = sz * 0.18f;
                Gdiplus::GraphicsPath path;
                path.AddArc(ox + 1.5f, oy + 1.5f, r * 2, r * 2, 180, 90);
                path.AddArc(ox + sz - r * 2 - 1.5f, oy + 1.5f, r * 2, r * 2, 270, 90);
                path.AddArc(ox + sz - r * 2 - 1.5f, oy + sz - r * 2 - 1.5f, r * 2, r * 2, 0, 90);
                path.AddArc(ox + 1.5f, oy + sz - r * 2 - 1.5f, r * 2, r * 2, 90, 90);
                path.CloseFigure();
                g.DrawPath(&pen, &path);
            } else {
                g.DrawRectangle(&pen, ox + 1.5f, oy + 1.5f, sz - 3.f, sz - 3.f);
            }
        }

        POINT dst{pip_x - pad, pip_y - pad};
        SIZE sz_win{win_w, win_h};
        POINT src_pt{0, 0};
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;
        HDC screen = ::GetDC(nullptr);
        ::UpdateLayeredWindow(
            window, screen, &dst, &sz_win, layer.dc, &src_pt, 0, &blend,
            ULW_ALPHA);
        ::ReleaseDC(nullptr, screen);
        ::ShowWindow(window, SW_SHOWNOACTIVATE);
    }

    void apply_anchor(const RECT& next)
    {
        anchor = next;
        anchor_valid = (next.right > next.left && next.bottom > next.top);
        compute_layout();
        if (frame_ready) paint();
    }

    bool start_camera(std::string& error)
    {
#if defined(FEATURE_VIDEO) && FEATURE_VIDEO
        try {
            compute_layout();
            int cap_w = 1280;
            int cap_h = 720;
            int cap_fps = 30;
            choose_pip_native_mode(
                options.input, pip_size, options.resolution, cap_w, cap_h, cap_fps);

            camera = std::make_unique<VideoInput>();
            Impl* self = this;
            camera->start(
                options.input, cap_w, cap_h, cap_fps,
                [self](const VideoFrame& frame) {
                    if (frame.format != PixelFormat::BGR24 &&
                        frame.format != PixelFormat::RGB24)
                        return;
                    if (frame.data.empty() || frame.width <= 0 ||
                        frame.height <= 0)
                        return;
                    // VideoInput documents BGR24 as the usual Win32 path.
                    self->on_camera_frame_bgr(
                        frame.data.data(), frame.width, frame.height,
                        frame.stride > 0 ? frame.stride : frame.width * 3);
                });
            pm::video::log_warn(
                "[camera-overlay] capture native " +
                std::to_string(camera->actual_width()) + "x" +
                std::to_string(camera->actual_height()) +
                " (mode=" +
                (options.resolution.empty() ? "auto" : options.resolution) +
                " → " + std::to_string(cap_w) + "x" +
                std::to_string(cap_h) + "@" + std::to_string(cap_fps) +
                ", pip=" + std::to_string(pip_size) + "px)");
            return true;
        } catch (const std::exception& ex) {
            error = ex.what() ? ex.what() : "camera open failed";
            camera.reset();
            return false;
        }
#else
        error = "webcam capture unavailable in this build (FEATURE_VIDEO)";
        return false;
#endif
    }

    void stop_camera() noexcept
    {
#if defined(FEATURE_VIDEO) && FEATURE_VIDEO
        if (camera) {
            try {
                camera->stop();
            } catch (...) {
            }
            camera.reset();
        }
#endif
    }

    void destroy_ui() noexcept
    {
        if (window) {
            ::DestroyWindow(window);
            window = nullptr;
        }
        layer.free();
#if defined(FEATURE_VIDEO) && FEATURE_VIDEO && \
    defined(FEATURE_VIDEO_BEAUTY) && FEATURE_VIDEO_BEAUTY
        beauty.reset();
        beauty_on = false;
#endif
        if (gdip_token) {
            Gdiplus::GdiplusShutdown(gdip_token);
            gdip_token = 0;
        }
    }

    bool initialize(std::string& error)
    {
        Gdiplus::GdiplusStartupInput gdiin;
        if (Gdiplus::GdiplusStartup(&gdip_token, &gdiin, nullptr) !=
            Gdiplus::Ok) {
            error = "GdiplusStartup failed";
            return false;
        }
        if (!create_window(error)) return false;
        if (!start_camera(error)) return false;
#if defined(FEATURE_VIDEO) && FEATURE_VIDEO && \
    defined(FEATURE_VIDEO_BEAUTY) && FEATURE_VIDEO_BEAUTY
        beauty_on = false;
        beauty_prev_ts = -1;
        beauty_fail_logged = false;
        if (options.beauty.wants_beauty()) {
            BeautyConfig cfg = options.beauty;
            cfg.running_mode = "video";
            std::string berr;
            if (!probe_beauty_runtime(cfg, berr) || !beauty.create(cfg, berr)) {
                pm::video::log_warn("[camera-overlay] beauty unavailable: " +
                             (berr.empty() ? std::string("create failed") : berr));
            } else {
                beauty_on = true;
                pm::video::log_warn("[camera-overlay] beauty on PiP webcam");
            }
        }
#endif
        return true;
    }

    void thread_main()
    {
        ui_thread_id.store(::GetCurrentThreadId(), std::memory_order_release);
        MSG msg{};
        ::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

        std::string error;
        const bool ok = initialize(error);
        {
            std::lock_guard<std::mutex> lock(init_mutex);
            init_ok = ok;
            init_error = std::move(error);
            init_done = true;
        }
        init_cv.notify_all();

        if (!ok) {
            stop_camera();
            destroy_ui();
            ui_thread_id.store(0, std::memory_order_release);
            return;
        }
        is_running.store(true, std::memory_order_release);

        bool stopping = false;
        while (!stopping && ::GetMessageW(&msg, nullptr, 0, 0) > 0) {
            switch (msg.message) {
            case kPaintMessage:
                paint();
                break;
            case kAnchorMessage: {
                RECT next{};
                {
                    std::lock_guard<std::mutex> lock(api_mutex);
                    next = anchor;
                }
                apply_anchor(next);
                break;
            }
            case kStopMessage:
                stopping = true;
                break;
            default:
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
                break;
            }
        }

        is_running.store(false, std::memory_order_release);
        stop_camera();
        destroy_ui();
        ui_thread_id.store(0, std::memory_order_release);
    }
};

WinCameraOverlayController::WinCameraOverlayController()
    : impl_(std::make_unique<Impl>())
{
}

WinCameraOverlayController::~WinCameraOverlayController()
{
    stop();
}

bool WinCameraOverlayController::start(
    const CameraOverlayConfig& config,
    const RECT& anchor_screen,
    std::string* error)
{
    if (impl_->ui_thread_id.load(std::memory_order_acquire) ==
        ::GetCurrentThreadId()) {
        if (error) *error = "camera overlay cannot start on its UI thread";
        return false;
    }
    stop();

    {
        std::lock_guard<std::mutex> lock(impl_->api_mutex);
        impl_->options = config;
        impl_->anchor = anchor_screen;
        impl_->anchor_valid =
            anchor_screen.right > anchor_screen.left &&
            anchor_screen.bottom > anchor_screen.top;
        impl_->init_done = false;
        impl_->init_ok = false;
        impl_->init_error.clear();
    }

    impl_->ui_thread = std::thread([implementation = impl_.get()] {
        implementation->thread_main();
    });

    {
        std::unique_lock<std::mutex> lock(impl_->init_mutex);
        impl_->init_cv.wait(lock, [&] { return impl_->init_done; });
        if (!impl_->init_ok) {
            if (error) *error = impl_->init_error;
            if (impl_->ui_thread.joinable()) impl_->ui_thread.join();
            return false;
        }
    }
    return true;
}

void WinCameraOverlayController::update_anchor(const RECT& anchor_screen)
{
    {
        std::lock_guard<std::mutex> lock(impl_->api_mutex);
        impl_->anchor = anchor_screen;
        impl_->anchor_valid =
            anchor_screen.right > anchor_screen.left &&
            anchor_screen.bottom > anchor_screen.top;
    }
    const DWORD tid = impl_->ui_thread_id.load(std::memory_order_acquire);
    if (tid) ::PostThreadMessageW(tid, kAnchorMessage, 0, 0);
}

void WinCameraOverlayController::stop() noexcept
{
    const DWORD tid = impl_->ui_thread_id.load(std::memory_order_acquire);
    if (tid) ::PostThreadMessageW(tid, kStopMessage, 0, 0);
    if (impl_->ui_thread.joinable()) {
        if (impl_->ui_thread.get_id() == std::this_thread::get_id())
            return;
        impl_->ui_thread.join();
    }
    impl_->is_running.store(false, std::memory_order_release);
}

bool WinCameraOverlayController::running() const noexcept
{
    return impl_->is_running.load(std::memory_order_acquire);
}

} // namespace pm::video

#endif
