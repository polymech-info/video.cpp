// Windows.Graphics.Capture (main window) + Media Foundation H.264 MP4.
// Interop: vendored robmikh.common (see src/session/recorder/vendor).

#include "pm/video/session_recorder.hpp"
#include "pm/video/text.hpp"
#include "pm/video/nvenc_loader.hpp"
#include "pm/video/zoom.hpp"
#include "pm/video/overlay.hpp"
#include "pm/video/idle.hpp"
#include "pm/video/pause.hpp"
#include "pm/video/text.hpp"
#if defined(FEATURE_STT) && FEATURE_STT
#include "pm/video/captions.hpp"
#endif

#if defined(_WIN32) && defined(FEATURE_SESSION_VIDEO_RECORDER)

#  include "pm/video/hooks.hpp"
#  include "pm/video/win_dpi.hpp"
#  include "pm/video/display_affinity.hpp"
#  include "pm/video/window_bounds.hpp"

#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <d3d11_4.h>
#  include <dxgi1_2.h>
#  include <unknwn.h>
// C++/WinRT (Graphics.Capture) — one consistent order before interop shims; robmikh
// `direct3d11.interop.h` includes the D3D11 WinRT header again (include-guarded).
#  include <winrt/base.h>
#  include <winrt/Windows.Foundation.h>
#  include <winrt/Windows.Foundation.Collections.h>
#  include <winrt/Windows.Graphics.h>
#  include <winrt/Windows.Graphics.Capture.h>
#  include <winrt/Windows.Graphics.DirectX.h>
#  include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#  include <mfapi.h>
#  include <mfidl.h>      // before mfreadwrite (IMF* / IMFSinkWriter)
#  include <mfreadwrite.h>
#  include <mferror.h>

#  include <robmikh.common/capture.desktop.interop.h>
#  include <robmikh.common/direct3d11.interop.h> // CreateDirect3D11DeviceFromDXGIDevice, GetDXGIInterfaceFromObject

#  include <algorithm>
#  include <atomic>
#  include <chrono>
#  include <cctype>
#  include <cmath>
#  include <condition_variable>
#  include <cstdint>
#  include <cstring>
#  include <deque>
#  include <utility>
#  include <iomanip>
#  include <memory>
#  include <mutex>
#  include <sstream>
#  include <stdexcept>
#  include <string>
#  include <thread>
#  include <vector>

#  include "pm/video/screen.hpp"

#  if PM_HAS_AUDIO_IO
#    if defined(FEATURE_VIDEO) && FEATURE_VIDEO
#      include "pm/video/mf_audio_mux.hpp"
#    endif
#    if defined(FEATURE_AUDIO_FILTER) && FEATURE_AUDIO_FILTER
#      include "pm/audio/filter/audio_filter.hpp"
#    endif
#  endif

#  pragma comment(lib, "d3d11.lib")
#  pragma comment(lib, "dxgi.lib")
#  pragma comment(lib, "mfplat.lib")
#  pragma comment(lib, "mfreadwrite.lib")
#  pragma comment(lib, "mfuuid.lib")
#  pragma comment(lib, "strmiids.lib")
#  pragma comment(lib, "windowsapp.lib")
#  pragma comment(lib, "RuntimeObject.lib")

using WgcFramePool  = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using DxpPixelFormat  = winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using IWinRtD3d11Device     = winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;

namespace media::win::recorder
{
namespace detail
{
constexpr int    kH264MaxDim      = 4096;
constexpr int    kH264MaxPixels   = 3840 * 2160;
constexpr int    kEncodeQueueSize = 3;

static std::wstring wide_from_utf8_lossy(const std::string& s)
{
    if (s.empty())
        return {};
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0)
        return std::wstring(s.begin(), s.end());
    std::wstring out(static_cast<size_t>(need), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), need) <= 0)
        return std::wstring(s.begin(), s.end());
    return out;
}

// UI-thread session video start: STA + C++/WinRT. Do **not** switch this thread to MTA for capture — the
// shell (Win32++/Ribbon) expects a single-threaded apartment here; use frame-pool `Create` when
// `CreateFreeThreaded` misbehaves instead of init_apartment(multi_threaded) on the UI thread.
void ensure_com_st_and_winrt()
{
    static std::atomic<bool> done{ false };
    if (done.load(std::memory_order_acquire)) return;
    (void)CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
    } catch (const winrt::hresult_error& e) {
        if (e.code() == 0x80010106) { /* RPC_E_CHANGED_MODE: COM + WinRT already inited for this thread */ }
        else
            throw;
    }
    done.store(true, std::memory_order_release);
}

/// In-app Log panel (see `log_sink.cpp` / `LogPanel.cpp` AppendLine + cwd file) — any thread.
static void ui_log_session_video(std::string msg)
{
    if (msg.rfind("[session video]", 0) != 0) msg = std::string("[session video] ") + std::move(msg);
    pm::video::log_info(msg);
    pm::video::ui_log_line(msg);
}

static void ui_warn_session_video(std::string msg)
{
    if (msg.rfind("[session video]", 0) != 0) msg = std::string("[session video] ") + std::move(msg);
    pm::video::log_warn(msg);
    pm::video::ui_log_line(msg);
}

static void ui_error_session_video(std::string msg)
{
    if (msg.rfind("[session video]", 0) != 0) msg = std::string("[session video] ") + std::move(msg);
    pm::video::log_error(msg);
    pm::video::ui_log_line(msg);
}

static const char* hresult_hint_un(unsigned int c)
{
    switch (c) {
        case 0x80004002:
            return " (E_NOINTERFACE — WGC: D3D/WinRT device bridge, frame pool, or optional session API on this OS.)";
        case 0x80010106: return " (RPC_E_CHANGED_MODE — COM already initialized on this thread)";
        default:         return "";
    }
}

static std::string hresult_failure(const char* operation, HRESULT hr)
{
    const auto code = static_cast<unsigned int>(hr);
    std::ostringstream out;
    out << operation << " failed, hresult=0x"
        << std::hex << std::setfill('0') << std::setw(8) << code
        << std::dec << hresult_hint_un(code);
    return out.str();
}

static bool validate_h264_frame_size(int w, int h, std::string& why)
{
    if (w < 2 || h < 2) {
        why = "encoded frame is too small after even-size crop";
        return false;
    }
    if (w > kH264MaxDim || h > kH264MaxDim ||
        static_cast<std::int64_t>(w) * static_cast<std::int64_t>(h) > kH264MaxPixels) {
        std::ostringstream o;
        o << "capture frame " << w << "x" << h
          << " is above the guarded H.264 limit (" << kH264MaxDim << " px per side, "
          << kH264MaxPixels << " pixels). Reduce the window size or record on a lower-scale monitor.";
        why = o.str();
        return false;
    }
    return true;
}

/// Optional `GraphicsCaptureSession` properties; missing contract → E_NOINTERFACE — log and continue.
template<typename F>
static void wgc_session_opt(F&& op, const char* ok_line, const char* skip_line)
{
    try {
        std::forward<F>(op)();
        ui_log_session_video(ok_line);
    } catch (const winrt::hresult_error& e) {
        if (e.code() != 0x80004002) throw;
        ui_log_session_video(skip_line);
    }
}

std::atomic<int> s_mf_users{ 0 };
void            mf_retain() noexcept
{
    if (s_mf_users.fetch_add(1) + 1 == 1) (void)MFStartup(MF_VERSION, MFSTARTUP_FULL);
}
void mf_release() noexcept
{
    int users = s_mf_users.load(std::memory_order_acquire);
    while (users > 0 &&
           !s_mf_users.compare_exchange_weak(
               users, users - 1, std::memory_order_acq_rel)) {
    }
    if (users <= 0) return;
    const int n = users - 1;
    if (n == 0) MFShutdown();
}

/// D3D for WGC: BGRA + video, feature levels 11.1/11.0, hardware then WARP (same idea as robmikh).
/// When @p used_warp is non-null, sets it true if the WARP software adapter was used.
static winrt::com_ptr<ID3D11Device> create_d3d11_device_for_wgc(
    winrt::com_ptr<ID3D11DeviceContext>& out_ctx,
    bool* used_warp = nullptr)
{
    if (used_warp) *used_warp = false;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#  if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#  endif
    D3D_FEATURE_LEVEL       levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    constexpr UINT          nlevels  = sizeof(levels) / sizeof(levels[0]);
    winrt::com_ptr<ID3D11Device> dev;
    D3D_FEATURE_LEVEL            got{};
    HRESULT                      hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, nlevels, D3D11_SDK_VERSION, dev.put(), &got, out_ctx.put());
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, nlevels, D3D11_SDK_VERSION, dev.put(), &got, out_ctx.put());
        if (SUCCEEDED(hr) && used_warp) *used_warp = true;
    }
    winrt::check_hresult(hr);
    return dev;
}

class RecSession;

std::mutex        g_mux;
std::shared_ptr<RecSession> g_sess;
pm::video::RecordingStatus g_lastStatus{};

void set_global_start_failure(const std::string& error) noexcept
{
    std::lock_guard<std::mutex> lock(g_mux);
    g_lastStatus = {};
    g_lastStatus.state = pm::video::RecordingState::failed;
    g_lastStatus.capture_backend = "fast-wgc";
    g_lastStatus.error = error;
}

class RecSession : public std::enable_shared_from_this<RecSession>
{
public:
    static std::shared_ptr<RecSession> make() { return std::shared_ptr<RecSession>(new RecSession); }
    const char* start_stage() const noexcept { return m_startStage; }

    void start_capture(const pm::video::WinCaptureTarget& target,
                       const std::filesystem::path& out,
                       bool include_cursor,
                       const CaptureCrop* crop,
                       int fps,
                       int bitrate_kbps,
                       std::string encoder,
                       const VideoTransformConfig* transform,
#if PM_HAS_AUDIO_IO
                       const SessionVideoAudioConfig* audio,
#endif
                       std::string& /*err*/)
    {
        m_outPath  = out;
        m_stopping = false;
        m_paused   = false;
        m_pauseReasons.store(pm::video::PauseNone, std::memory_order_relaxed);
        m_idleDetector.reset();
        m_idleDetector.configure(transform ? transform->idle_pause : pm::video::IdlePauseConfig{});
        m_idleHooksLive.store(false, std::memory_order_relaxed);
        m_haveIdleProbe = false;
        m_idleProbeFailed = false;
        m_idleVpFrame = 0;
        m_captionDumpPath.clear();
        m_encW     = 0;
        m_encH     = 0;
        m_hasCrop  = false;
        // Picture time follows the capture clock, same origin as the first frame
        // the audio mux is armed to. A 60 fps step per delivered frame runs the
        // picture ahead whenever WGC delivers slower than the target fps.
        m_use_wall_pts = true;
        m_have_pts_t0 = false;
        m_last_vid_pts = 0;
        m_pauseStartedValid = false;
        m_pauseDebt = {};
        m_fps      = std::clamp(fps, 1, 240);
        m_frameDur = 10'000'000LL / static_cast<LONGLONG>(m_fps);
        m_bitrateKbps = std::clamp(bitrate_kbps, 0, 100000);
        encoder = pm::video::lower_ascii(std::move(encoder));
        m_encoderRequest = (encoder == "nvenc" || encoder == "mediafoundation")
            ? std::move(encoder) : std::string("auto");
        m_haveAcceptedFrame = false;
        m_droppedFrames.store(0, std::memory_order_relaxed);
        m_framesWritten.store(0, std::memory_order_relaxed);
        m_recordingStarted.store(false, std::memory_order_relaxed);
        m_effectiveEncoder.clear();
        m_effectiveBitrateKbps = 0;
        m_target = target;
        m_transform = transform ? *transform : VideoTransformConfig{};
        m_transform.size_mode = pm::video::lower_ascii(std::move(m_transform.size_mode));
        if (m_transform.size_mode != "pad" &&
            m_transform.size_mode != "fit" &&
            m_transform.size_mode != "fill") {
            m_transform.size_mode = "native";
        }
        m_transform.follow = pm::video::lower_ascii(std::move(m_transform.follow));
        if (m_transform.follow != "cursor" &&
            m_transform.follow != "anchor") {
            m_transform.follow = "none";
        }
        m_transform.hud_capture = pm::video::lower_ascii(std::move(m_transform.hud_capture));
        if (m_transform.hud_capture != "visible" &&
            m_transform.hud_capture != "exclude") {
            m_transform.hud_capture = "auto";
        }
        m_transform.zoom_min = std::clamp(m_transform.zoom_min, 1.0, 32.0);
        m_transform.zoom_max = std::clamp(
            m_transform.zoom_max, m_transform.zoom_min, 32.0);
        m_transform.zoom = std::clamp(
            m_transform.zoom, m_transform.zoom_min, m_transform.zoom_max);
        m_transform.follow_speed =
            std::clamp(m_transform.follow_speed, 0.01, 1.0);
        m_transform.follow_deadzone =
            std::clamp(m_transform.follow_deadzone, 0, 1000);
        m_transform.transition_ms =
            std::clamp(m_transform.transition_ms, 0, 10000);
        m_transformEnabled =
            (m_transform.width > 0 && m_transform.height > 0) ||
            m_transform.aspect_ratio > 0.0 ||
            m_transform.zoom > 1.0001 ||
            m_transform.follow == "cursor" ||
            m_transform.interactive_zoom;
        m_pauseModifiers = 0;
        m_pauseVirtualKey = 0;
        m_stopModifiers = 0;
        m_stopVirtualKey = 0;
        m_userStopRequested.store(false, std::memory_order_relaxed);
        if (!m_transform.pause_key.empty() &&
            !pm::video::parse_shortcut(
                m_transform.pause_key,
                m_pauseModifiers, m_pauseVirtualKey)) {
            throw std::runtime_error(
                "invalid pauseKey shortcut: " +
                m_transform.pause_key);
        }
        if (!m_transform.stop_key.empty() &&
            !pm::video::parse_shortcut(
                m_transform.stop_key,
                m_stopModifiers, m_stopVirtualKey)) {
            throw std::runtime_error(
                "invalid stopKey shortcut: " +
                m_transform.stop_key);
        }
        m_zoomStart = m_zoomTarget = m_transform.zoom;
        m_currentZoom = m_transform.zoom;
        m_zoomTransitionStart = std::chrono::steady_clock::now();
        m_centerInitialized = false;
        set_error({});
        set_soft_warning({});
#if PM_HAS_AUDIO_IO
        m_audio_cfg = {};
        m_audio_enabled = false;
        if (audio && pm::audio::capture_source_active(*audio)) {
            m_audio_cfg = *audio;
            m_audio_enabled = true;
        }
        m_audio_mux.reset();
        m_audio_stream = 0;
#endif
        m_cropL = m_cropT = m_cropW = m_cropH = 0;
        if (crop && crop->valid()) {
            m_hasCrop = true;
            m_cropL   = crop->left;
            m_cropT   = crop->top;
            m_cropW   = crop->width;
            m_cropH   = crop->height;
            {
                std::ostringstream o;
                o << "WGC crop: rrect @" << m_cropL << "," << m_cropT
                  << " size " << m_cropW << "x" << m_cropH
                  << " (full-window GPU capture + subregion encode)";
                ui_log_session_video(o.str());
            }
        }
        m_loggedFirstFrame   = false;
        m_loggedEvenCrop     = false;
        m_loggedFirstVidSample.store(false, std::memory_order_relaxed);
        m_loggedVidWriteFail.store(false, std::memory_order_relaxed);
        m_loggedSizeChange   = false;
        m_logFrameIndexMod30 = 0;
        ui_log_session_video(std::string("starting capture, output: ") + pm::video::path_to_utf8(out));
        ui_log_session_video("D3D11: D3D11CreateDevice (BGRA+VIDEO, FL 11.1/11.0, HW then WARP)…");
        bool used_warp = false;
        m_d3d = create_d3d11_device_for_wgc(m_ctx, &used_warp);
        {
            // WGC / free-threaded pool may use the D3D device from worker threads; device hosts ID3D11Multithread.
            winrt::com_ptr<ID3D11Multithread> mthread;
            if (SUCCEEDED(m_d3d->QueryInterface(IID_PPV_ARGS(mthread.put())))) {
                mthread->SetMultithreadProtected(TRUE);
                ui_log_session_video("D3D11: SetMultithreadProtected(TRUE) on device");
            } else if (SUCCEEDED(m_ctx->QueryInterface(IID_PPV_ARGS(mthread.put())))) {
                mthread->SetMultithreadProtected(TRUE);
                ui_log_session_video("D3D11: SetMultithreadProtected(TRUE) on immediate context (fallback QI)");
            } else
                ui_log_session_video("D3D11: no ID3D11Multithread (unexpected) — WGC may fail on some stacks");
        }
        if (used_warp) {
            const char* msg =
                "D3D11 hardware device unavailable; using WARP (software) — "
                "capture/encode may be slow";
            ui_log_session_video(msg);
            append_soft_warning(msg);
        } else {
            ui_log_session_video("D3D11: device + immediate context ready (hardware)");
        }
        {
            winrt::com_ptr<IDXGIDevice>      dxgi = m_d3d.as<IDXGIDevice>();
            winrt::com_ptr<::IInspectable>  insp;
            ui_log_session_video("WinRT: CreateDirect3D11DeviceFromDXGIDevice (explicit IInspectable*)…");
            winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put()));
            m_wrDevice = insp.as<IWinRtD3d11Device>();
        }
        if (target.window) {
            ui_log_session_video("WGC: CreateCaptureItemForWindow…");
            m_item = robmikh::common::desktop::CreateCaptureItemForWindow(
                target.window);
        } else {
            ui_log_session_video("WGC: CreateCaptureItemForMonitor…");
            m_item = robmikh::common::desktop::CreateCaptureItemForMonitor(
                target.monitor);
        }
        // BGRA8 — WinRT exposes this as B8G8R8A8UIntNormalized (there is no separate B8G8R8A8UInt in the projection here).
        m_fmt = DxpPixelFormat::B8G8R8A8UIntNormalized;
        const auto sz = m_item.Size();
        int        pw = (sz.Width > 0) ? sz.Width : 1;
        int        ph = (sz.Height > 0) ? sz.Height : 1;
        m_poolW     = pw;
        m_poolH     = ph;
        {
            std::ostringstream o;
            o << "WGC: item " << sz.Width << "x" << sz.Height << ", pool (initial) " << pw << "x" << ph
              << ", B8G8R8A8UIntNormalized, buffers=3";
            ui_log_session_video(o.str());
        }
        const auto initialSize = m_item.Size();

        // Content space = visible window (physical px). WGC item/frame textures
        // may still include invisible DWM borders; m_contentOrigin* maps content
        // → texture. Without PerMonitorV2, visible bounds are logical (≈½ at
        // 200% DPI) while item.Size() stays physical — encoder then undersizes
        // and the MP4 looks pixelated (especially noticeable with cursor on).
        int initialContentW = pw;
        int initialContentH = ph;
        m_contentOriginL = 0;
        m_contentOriginT = 0;
        if (target.window) {
            media::win::ThreadDpiPerMonitorScope dpi_scope;
            RECT outer{};
            RECT visibleBounds{};
            const bool haveOuter =
                ::GetWindowRect(target.window, &outer) != FALSE &&
                outer.right > outer.left && outer.bottom > outer.top;
            if (media::win::visible_window_bounds(
                    target.window, visibleBounds)) {
                const int visibleW =
                    visibleBounds.right - visibleBounds.left;
                const int visibleH =
                    visibleBounds.bottom - visibleBounds.top;
                if (visibleW > 0 && visibleH > 0) {
                    if (haveOuter) {
                        const int outerW = outer.right - outer.left;
                        const int outerH = outer.bottom - outer.top;
                        if (outerW > 0 && outerH > 0) {
                            const double sx =
                                static_cast<double>(pw) /
                                static_cast<double>(outerW);
                            const double sy =
                                static_cast<double>(ph) /
                                static_cast<double>(outerH);
                            int originL = static_cast<int>(std::lround(
                                (visibleBounds.left - outer.left) * sx));
                            int originT = static_cast<int>(std::lround(
                                (visibleBounds.top - outer.top) * sy));
                            int mappedW = static_cast<int>(
                                std::lround(visibleW * sx));
                            int mappedH = static_cast<int>(
                                std::lround(visibleH * sy));
                            originL = std::max(
                                0, std::min(originL, pw - 1));
                            originT = std::max(
                                0, std::min(originT, ph - 1));
                            mappedW = std::max(
                                2, std::min(mappedW, pw - originL));
                            mappedH = std::max(
                                2, std::min(mappedH, ph - originT));
                            if (mappedW != pw || mappedH != ph ||
                                originL != 0 || originT != 0) {
                                ui_log_session_video(
                                    "WGC: visible content " +
                                    std::to_string(mappedW) + "x" +
                                    std::to_string(mappedH) +
                                    " @ (" + std::to_string(originL) +
                                    "," + std::to_string(originT) +
                                    ") in item " + std::to_string(pw) +
                                    "x" + std::to_string(ph) +
                                    " (physical px)");
                                m_contentOriginL = originL;
                                m_contentOriginT = originT;
                                initialContentW = mappedW;
                                initialContentH = mappedH;
                            }
                        }
                    } else if (visibleW != pw || visibleH != ph) {
                        ui_log_session_video(
                            "WGC: using visible window bounds " +
                            std::to_string(visibleW) + "x" +
                            std::to_string(visibleH) +
                            " for encoder (capture item includes invisible borders)");
                        initialContentW = visibleW;
                        initialContentH = visibleH;
                    }
                }
            }
        }
        int initialCropL = m_hasCrop ? m_cropL : 0;
        int initialCropT = m_hasCrop ? m_cropT : 0;
        int initialCropW = m_hasCrop ? m_cropW : initialContentW;
        int initialCropH = m_hasCrop ? m_cropH : initialContentH;
        initialCropL = std::max(
            0, std::min(initialCropL, initialContentW - 1));
        initialCropT = std::max(
            0, std::min(initialCropT, initialContentH - 1));
        initialCropW = std::max(
            0, std::min(initialCropW, initialContentW - initialCropL));
        initialCropH = std::max(
            0, std::min(initialCropH, initialContentH - initialCropT));
        const int initialEncW = initialCropW & ~1;
        const int initialEncH = initialCropH & ~1;
        m_baseL = initialCropL;
        m_baseT = initialCropT;
        m_baseW = initialCropW;
        m_baseH = initialCropH;
        m_sourceContentW = initialContentW;
        m_sourceContentH = initialContentH;
        m_lastViewport = {
            initialCropL, initialCropT,
            initialCropL + initialCropW,
            initialCropT + initialCropH};
        const int outputW = (m_transform.width > 0 && m_transform.height > 0)
            ? (m_transform.width & ~1) : initialEncW;
        const int outputH = (m_transform.width > 0 && m_transform.height > 0)
            ? (m_transform.height & ~1) : initialEncH;
        ui_log_session_video(
            "initializing encoder before capture: " +
            std::to_string(outputW) + "x" + std::to_string(outputH) +
            (m_transformEnabled
                ? " (fixed-canvas GPU transform, source " +
                    std::to_string(initialCropW) + "x" +
                    std::to_string(initialCropH) + ")"
                : std::string{}));
        m_startStage = "encoder initialization";
        if (!init_encoder(outputW, outputH))
            throw std::runtime_error(last_error());
        m_startStage = "GPU transform initialization";
        if (m_transformEnabled && !init_transform_processor(pw, ph))
            throw std::runtime_error(last_error());

        // Console hosts have no dispatcher to deliver Create() callbacks, so they
        // use the free-threaded pool. GUI hosts retain the dispatcher-first path
        // and fall back for apartment/driver quirks. A ribbon xblox worker does
        // not pump the window thread, so it also uses the free-threaded pool.
        const bool cli_process = pm::video::record_in_cli_process();
        bool deliver_on_this_thread = !cli_process;
        if (!cli_process) {
            if (HWND host = pm::video::originating_capture_hwnd()) {
                const DWORD window_tid = ::GetWindowThreadProcessId(host, nullptr);
                if (window_tid != 0 && window_tid != ::GetCurrentThreadId())
                    deliver_on_this_thread = false;
            }
        }
        m_startStage = "WGC frame-pool creation";
        if (!deliver_on_this_thread) {
            ui_log_session_video(cli_process
                ? "WGC step: Direct3D11CaptureFramePool::CreateFreeThreaded (CLI)…"
                : "WGC step: Direct3D11CaptureFramePool::CreateFreeThreaded (worker)…");
            m_pool = WgcFramePool::CreateFreeThreaded(
                m_wrDevice, m_fmt, 3, initialSize);
            ui_log_session_video(cli_process
                ? "WGC: frame pool = CreateFreeThreaded (CLI ok)"
                : "WGC: frame pool = CreateFreeThreaded (worker ok)");
        } else {
            ui_log_session_video("WGC step: Direct3D11CaptureFramePool::Create (UI-thread delivery)…");
            try {
                m_pool = WgcFramePool::Create(m_wrDevice, m_fmt, 3, initialSize);
                ui_log_session_video("WGC: frame pool = Create (ok)");
            } catch (const winrt::hresult_error& e) {
                const auto c = static_cast<unsigned int>(e.code() & 0xFFFFFFFFU);
                std::ostringstream o;
                o << "WGC: Create failed, hresult=0x" << std::hex << std::setfill('0') << std::setw(8) << c << std::dec
                  << " — try CreateFreeThreaded…";
                ui_log_session_video(o.str());
                m_pool = WgcFramePool::CreateFreeThreaded(m_wrDevice, m_fmt, 3, initialSize);
                ui_log_session_video("WGC: frame pool = CreateFreeThreaded (ok)");
            }
        }

        m_startStage = "WGC capture-session creation";
        ui_log_session_video("WGC step: CreateCaptureSession(item)…");
        m_session = m_pool.CreateCaptureSession(m_item);
        ui_log_session_video("WGC: CreateCaptureSession ok — optional border/cursor session properties…");
        wgc_session_opt(
            [this] { m_session.IsBorderRequired(false); },
            "WGC: IsBorderRequired(false) ok",
            "WGC: IsBorderRequired skipped (E_NOINTERFACE)");
        wgc_session_opt(
            [this, include_cursor] { m_session.IsCursorCaptureEnabled(include_cursor); },
            include_cursor ? "WGC: IsCursorCaptureEnabled(true) ok" : "WGC: IsCursorCaptureEnabled(false) ok",
            "WGC: IsCursorCaptureEnabled skipped (E_NOINTERFACE — pointer may be missing on older Windows)");

        m_startStage = "WGC frame callback registration";
        ui_log_session_video("WGC step: FrameArrived handler…");
        const std::weak_ptr<RecSession> weak{ shared_from_this() };
        m_pool.FrameArrived(
            [weak](WgcFramePool const& pool, winrt::Windows::Foundation::IInspectable const&) {
                if (const auto s = weak.lock()) s->on_frame(pool);
            });

        m_startStage = "WGC StartCapture";
        ui_log_session_video("WGC step: StartCapture()…");
        m_pickerHudCapture = std::make_unique<media::win::PickerHudCaptureScope>(
            m_transform.hud_capture == "visible");
        m_session.StartCapture();
        m_recordingStarted.store(true, std::memory_order_release);
        m_startStage = "capture active";
        ui_log_session_video("WGC: StartCapture() returned; capture active");
        if constexpr (requires(winrt::Windows::Graphics::Capture::GraphicsCaptureSession s) {
                          s.DirtyRegionMode(
                              winrt::Windows::Graphics::Capture::GraphicsCaptureDirtyRegionMode::ReportOnly);
                      }) {
            try {
                m_session.DirtyRegionMode(
                    winrt::Windows::Graphics::Capture::GraphicsCaptureDirtyRegionMode::ReportOnly);
            } catch (...) {
            }
        }
        if (m_transform.interactive_zoom ||
            m_pauseVirtualKey != 0 ||
            m_stopVirtualKey != 0 ||
            m_transform.key_overlay.enabled ||
            m_transform.cursor_fx.any() ||
            m_transform.idle_pause.enabled())
            start_interactive_controls();
        if (m_transform.camera_overlay.enabled)
            start_camera_overlay();
    }

    void on_frame(WgcFramePool const& pool)
    {
        try {
        if (m_stopping.load(std::memory_order_relaxed)) return;
        if (m_userStopRequested.load(std::memory_order_acquire)) {
            user_stop();
            return;
        }
        const auto fr = pool.TryGetNextFrame();
        if (!fr) return;
        if (m_stopping.load(std::memory_order_relaxed)) return;
        if (m_userStopRequested.load(std::memory_order_acquire)) {
            user_stop();
            return;
        }
        std::string workerError;
        {
            std::lock_guard<std::mutex> lock(m_encodeMutex);
            workerError = m_workerError;
        }
        if (!workerError.empty()) {
            err_stop(workerError.c_str());
            return;
        }

        const auto capturedAt = std::chrono::steady_clock::now();

        const auto cs  = fr.ContentSize();
        const int  cx  = cs.Width;
        const int  cy  = cs.Height;
        if (cx <= 0 || cy <= 0) return;

        // Encode the visible content region (physical px), not the full WGC
        // item when it still includes invisible borders. User rrect crops are
        // relative to that content origin.
        const int contentL = m_contentOriginL;
        const int contentT = m_contentOriginT;
        const int contentW = std::max(
            2, std::min(m_sourceContentW > 0 ? m_sourceContentW : cx,
                        cx - contentL));
        const int contentH = std::max(
            2, std::min(m_sourceContentH > 0 ? m_sourceContentH : cy,
                        cy - contentT));
        int cropL = contentL + (m_hasCrop ? m_cropL : 0);
        int cropT = contentT + (m_hasCrop ? m_cropT : 0);
        int cropW = m_hasCrop ? m_cropW : contentW;
        int cropH = m_hasCrop ? m_cropH : contentH;
        cropL = std::max(0, std::min(cropL, cx - 1));
        cropT = std::max(0, std::min(cropT, cy - 1));
        cropW = std::max(0, std::min(cropW, cx - cropL));
        cropH = std::max(0, std::min(cropH, cy - cropT));
        if (cropW < 2 || cropH < 2) return;

        const int encCx = m_transformEnabled ? m_encW : (cropW & ~1);
        const int encCy = m_transformEnabled ? m_encH : (cropH & ~1);
        if (!m_transformEnabled &&
            (encCx != cropW || encCy != cropH) &&
            !m_loggedEvenCrop) {
            m_loggedEvenCrop = true;
            std::ostringstream o;
            o << "encode region " << cropW << "x" << cropH << " from WGC frame " << cx << "x" << cy
              << " → even H.264 size " << encCx << "x" << encCy;
            ui_log_session_video(o.str());
        }
        std::string sizeErr;
        if (!validate_h264_frame_size(encCx, encCy, sizeErr)) {
            err_stop(sizeErr.c_str());
            return;
        }

        if (!m_transformEnabled && m_encW > 0 &&
            (encCx != m_encW || encCy != m_encH)) {
            m_droppedFrames.fetch_add(1, std::memory_order_relaxed);
            if (!m_loggedSizeChange) {
                m_loggedSizeChange = true;
                std::ostringstream o;
                o << "size changed to " << cx << "x" << cy << " (encoded "
                  << encCx << "x" << encCy << ") (encoder " << m_encW << "x"
                  << m_encH << ") — dropping frames; stop and restart to re-encode";
                ui_log_session_video(o.str());
            }
            return;
        }

        if (!m_sink) {
            err_stop("encoder sink became unavailable after successful startup");
            return;
        }
        if (!m_loggedFirstFrame) {
            m_loggedFirstFrame = true;
            ui_warn_session_video(
                std::string("first accepted WGC frame → encode queue; output: ") +
                pm::video::path_to_utf8(m_outPath) +
                " audio=" + std::to_string(m_audio_enabled ? 1 : 0) +
                " have_pts_t0=" + std::to_string(m_have_pts_t0 ? 1 : 0));
        }

        winrt::com_ptr<ID3D11Texture2D> srcTex;
        try {
            srcTex = GetDXGIInterfaceFromObject<ID3D11Texture2D>(fr.Surface());
        } catch (const winrt::hresult_error&) {
            err_stop("GetDXGIInterfaceFromObject(texture)");
            return;
        }

        std::vector<pm::video::IdleDirtyRect> dirty;
        collect_dirty_rects(fr, cropL, cropT, cropW, cropH, dirty);
        probe_idle_frame(srcTex.get(), cropL, cropT, cropW, cropH, cx, cy,
                         dirty.empty() ? nullptr : dirty.data(),
                         static_cast<int>(dirty.size()), capturedAt);
        tick_idle(capturedAt);

        const auto reasons = m_pauseReasons.load(std::memory_order_relaxed);
        const bool rec_active = pm::video::pause_recording_active(reasons);
        const bool user_paused = pm::video::pause_has(reasons, pm::video::PauseUser);

        if (user_paused) {
            discard_preroll();
            srcTex = nullptr;
            fr.Close();
            return;
        }

        const bool encode_due = !(m_haveAcceptedFrame && capturedAt < m_nextAcceptedFrame);
        if (!rec_active) {
            if (encode_due)
                copy_to_preroll(srcTex.get(), contentW, contentH, cropL, cropT,
                                encCx, encCy, cx, cy, capturedAt);
            srcTex = nullptr;
            fr.Close();
            return;
        }

        flush_preroll();

        if (!encode_due) {
            srcTex = nullptr;
            fr.Close();
            return;
        }

        if (m_transformEnabled &&
            (cx > m_transformSourceW || cy > m_transformSourceH) &&
            !init_transform_processor(
                std::max(cx, m_transformSourceW),
                std::max(cy, m_transformSourceH))) {
            err_stop(last_error().c_str());
            return;
        }
        int stagingIndex = -1;
        {
            std::lock_guard<std::mutex> lock(m_encodeMutex);
            if (!m_freeStaging.empty()) {
                stagingIndex = m_freeStaging.front();
                m_freeStaging.pop_front();
            }
        }
        if (stagingIndex < 0) {
            const auto dropped = m_droppedFrames.fetch_add(
                1, std::memory_order_relaxed) + 1;
            if (dropped == 1 || (dropped % 30) == 0) {
                ui_log_session_video(
                    "encoder queue saturated — skipped " + std::to_string(dropped)
                    + " frame(s) without blocking WGC");
            }
            return;
        }
        if (m_transformEnabled) {
            // Transform math is in visible-content space; the GPU source rect
            // is offset into the full WGC texture below.
            if (!transform_frame(
                    srcTex.get(), stagingIndex, contentW, contentH,
                    capturedAt)) {
                {
                    std::lock_guard<std::mutex> lock(m_encodeMutex);
                    m_freeStaging.push_back(stagingIndex);
                }
                err_stop(last_error().c_str());
                return;
            }
        } else {
            const D3D11_BOX srcBox{
                static_cast<UINT>(cropL),
                static_cast<UINT>(cropT),
                0,
                static_cast<UINT>(cropL + encCx),
                static_cast<UINT>(cropT + encCy),
                1,
            };
            m_ctx->CopySubresourceRegion(
                m_staging[static_cast<size_t>(stagingIndex)].get(),
                0, 0, 0, 0, srcTex.get(), 0, &srcBox);
        }
        srcTex = nullptr;
        fr.Close();
        {
            std::lock_guard<std::mutex> lock(m_encodeMutex);
            m_encodeQueue.push_back({stagingIndex, capturedAt});
        }
        m_encodeReady.notify_one();

        // A capture item can initially include invisible borders, then deliver
        // smaller content frames. The existing larger pool is valid; shrinking
        // it from a CreateFreeThreaded callback can fail with RPC_E_WRONG_THREAD.
        // Growth is handled only when the current pool cannot contain the frame.
        if (cx > m_poolW || cy > m_poolH) {
            try {
                pool.Recreate(
                    m_wrDevice, m_fmt, 3,
                    winrt::Windows::Graphics::SizeInt32{cx, cy});
                m_poolW = cx;
                m_poolH = cy;
                ui_log_session_video(
                    "WGC: frame pool recreated at " + std::to_string(cx) +
                    "x" + std::to_string(cy));
            } catch (const winrt::hresult_error& e) {
                const auto code =
                    static_cast<unsigned int>(e.code() & 0xFFFFFFFFU);
                std::ostringstream error;
                error << "WGC frame pool Recreate failed, hresult=0x"
                      << std::hex << std::setfill('0') << std::setw(8) << code;
                err_stop(error.str().c_str());
                return;
            }
        }

        const auto period = std::chrono::nanoseconds(
            1'000'000'000LL / static_cast<long long>(m_fps));
        if (!m_haveAcceptedFrame) {
            m_nextAcceptedFrame = capturedAt + period;
            m_haveAcceptedFrame = true;
        } else {
            m_nextAcceptedFrame += period;
            if (m_nextAcceptedFrame + period < capturedAt)
                m_nextAcceptedFrame = capturedAt + period;
        }
        } catch (const winrt::hresult_error& e) {
            const auto c = static_cast<unsigned int>(e.code() & 0xFFFFFFFFU);
            std::ostringstream o;
            o << "FrameArrived exception hresult=0x" << std::hex << std::setfill('0') << std::setw(8) << c
              << std::dec << hresult_hint_un(c);
            err_stop(o.str().c_str());
        } catch (const std::exception& ex) {
            err_stop(ex.what() ? ex.what() : "FrameArrived std::exception");
        } catch (...) {
            err_stop("FrameArrived unknown exception");
        }
    }

    void stop_and_finalize() noexcept
    {
        m_stopping = true;
        m_pickerHudCapture.reset();
        auto controls = m_interactiveControls.exchange({});
        if (controls) controls->stop();
        auto camera = m_cameraOverlay.exchange({});
        if (camera) camera->stop();
        try {
            if (m_session) m_session.Close();
        } catch (...) { /* ignore */ }
        m_session = nullptr;
        try {
            if (m_pool) m_pool.Close();
        } catch (...) { /* ignore */ }
        m_pool = nullptr;
#if PM_HAS_AUDIO_IO
        // Stop the second sink-writer producer before draining video. Leaving
        // live audio writes active can throttle WriteSample and block the video
        // worker join for many seconds.
        const bool had_audio = m_audio_enabled && m_audio_mux;
        if (m_audio_mux) m_audio_mux->stop_capture();
        m_audio_mux.reset();
        m_audio_enabled = false;
#if defined(FEATURE_STT) && FEATURE_STT
        if (m_transform.captions.any() && !m_captionDumpPath.empty()) {
            // Whisper runs for the whole take. Doing it on the UI thread froze
            // the window, so the GUI detaches and the process stays up. The CLI
            // process exits when the block returns, which kills a detached
            // worker before the .srt exists — join there.
            const auto dump = m_captionDumpPath;
            const auto media = m_outPath;
            const auto cfg = m_transform.captions;
            const std::string hud_mode = m_transform.status_hud;
            try {
            std::thread caption_worker([dump, media, cfg, hud_mode] {
                const auto hud = pm::video::caption_status(hud_mode, "Transcribing...");
                (void)hud;
                std::error_code ec;
                const auto sz = std::filesystem::file_size(dump, ec);
                const auto audio_s = (!ec && sz > 44)
                    ? static_cast<long long>((sz - 44) / 32000)
                    : 0LL;
                ui_log_session_video(
                    "captions: STT started (heavy) audio_s=" + std::to_string(audio_s));
                try {
                    std::vector<std::filesystem::path> written;
                    std::string capErr;
                    if (!pm::video::write_caption_sidecars(dump, media, cfg, written, capErr)) {
                        ui_log_session_video(
                            "captions: STT finished - " +
                            (capErr.empty() ? std::string("failed") : capErr));
                        return;
                    }
                    if (written.empty()) {
                        ui_log_session_video("captions: STT finished - no cues (empty transcript)");
                        return;
                    }
                    std::string line = "captions: STT finished - wrote";
                    for (const auto& p : written)
                        line += " " + pm::video::path_to_utf8(p);
                    ui_log_session_video(std::move(line));
                } catch (const std::exception& ex) {
                    ui_log_session_video(
                        std::string("captions: STT finished - ") +
                        (ex.what() ? ex.what() : "transcription failed"));
                } catch (...) {
                    ui_log_session_video("captions: STT finished - transcription failed");
                }
            });
            if (pm::video::record_in_cli_process()) {
                if (caption_worker.joinable())
                    caption_worker.join();
            } else {
                caption_worker.detach();
            }
            } catch (const std::exception& ex) {
                ui_log_session_video(
                    std::string("captions: STT failed to start - ") +
                    (ex.what() ? ex.what() : "thread"));
            } catch (...) {
                ui_log_session_video("captions: STT failed to start");
            }
        } else if (m_transform.captions.any()) {
            ui_log_session_video("captions: skipped (no dump wav — audio mux never started)");
        }
#endif
#else
        const bool had_audio = false;
#endif
        const auto frames = m_framesWritten.load(std::memory_order_relaxed);
        stop_encode_worker();
        bool discard_output = (frames == 0);
        if (m_sink) {
            if (frames == 0) {
                // Finalize cannot produce a valid requested video stream and
                // reports MF_E_SINK_NO_SAMPLES_PROCESSED, masking the startup
                // error that prevented the first frame.
                set_error_if_empty(
                    "recording stopped before any video frame was written");
                ui_error_session_video(last_error());
                ui_warn_session_video(
                    "skipping Media Foundation Finalize: zero video samples "
                    "(audio_requested=" + std::to_string(had_audio ? 1 : 0) + ")");
            } else {
                const HRESULT finalizeHr = m_sink->Finalize();
                if (FAILED(finalizeHr)) {
                    discard_output = true;
                    std::ostringstream error;
                    error << "Media Foundation Finalize failed, hresult=0x"
                          << std::hex << std::setfill('0') << std::setw(8)
                          << static_cast<unsigned int>(finalizeHr)
                          << " frames=" << frames
                          << " audio_requested=" << (had_audio ? 1 : 0);
                    set_error_if_empty(error.str());
                    ui_error_session_video(last_error());
                } else {
                    ui_warn_session_video(
                        "Finalize ok frames=" + std::to_string(frames) +
                        " audio_requested=" + std::to_string(had_audio ? 1 : 0) +
                        " last_vid_pts_ms=" + std::to_string(m_last_vid_pts / 10000));
                }
            }
            m_sink = nullptr;
        } else if (frames == 0) {
            set_error_if_empty(
                "recording stopped before encoder wrote any video frame");
            ui_error_session_video(last_error());
        }
        // Incomplete MF MP4s (no moov) are tiny stubs — remove so callers don't
        // treat a 79-byte unreadable file as a successful artifact.
        if (discard_output && !m_outPath.empty()) {
            std::error_code remove_ec;
            const auto sz = std::filesystem::file_size(m_outPath, remove_ec);
            if (!remove_ec) {
                std::filesystem::remove(m_outPath, remove_ec);
                ui_warn_session_video(
                    "discarded incomplete MF output (" + std::to_string(sz) +
                    " bytes, frames=" + std::to_string(frames) + "): " +
                    pm::video::path_to_utf8(m_outPath));
            }
        }
        m_nvenc.reset();
        discard_preroll();
        reset_idle_probe_gpu();
        m_staging.clear();
        m_cpuStaging.clear();
        m_videoOutputViews.clear();
        m_videoProcessor = nullptr;
        m_videoEnumerator = nullptr;
        m_videoContext = nullptr;
        m_videoDevice = nullptr;
        m_d3d       = nullptr;
        m_ctx       = nullptr;
        m_item      = nullptr;
        m_wrDevice  = nullptr;
    }

    bool active() const noexcept
    {
        // After err_stop, session is gone even if the shared_ptr is momentarily not cleared.
        return !m_stopping.load() && (m_session != nullptr);
    }

    void toggle_pause() noexcept
    {
        const bool user = pm::video::pause_has(
            m_pauseReasons.load(std::memory_order_relaxed),
            pm::video::PauseUser);
        set_pause_reason(pm::video::PauseUser, !user);
    }

    void set_pause_reason(pm::video::PauseReason bit, bool on) noexcept
    {
        const auto now = std::chrono::steady_clock::now();
        std::uint32_t prev = 0;
        std::uint32_t next = 0;
        {
            std::lock_guard<std::mutex> lock(m_timingMutex);
            prev = m_pauseReasons.load(std::memory_order_relaxed);
            next = pm::video::pause_set(prev, bit, on);
            m_pauseReasons.store(next, std::memory_order_release);
            const bool was_active = pm::video::pause_recording_active(prev);
            const bool now_active = pm::video::pause_recording_active(next);
            m_paused.store(!now_active, std::memory_order_release);
            if (was_active && !now_active) {
                m_pauseStarted = now;
                m_pauseStartedValid = true;
            } else if (!was_active && now_active && m_pauseStartedValid) {
                m_pauseDebt += now - m_pauseStarted;
                m_pauseStartedValid = false;
            }
        }
#if PM_HAS_AUDIO_IO
        if (m_audio_mux)
            m_audio_mux->set_paused(!pm::video::pause_recording_active(next));
#endif
        if (prev == next)
            return;
        if (pm::video::pause_has(next, pm::video::PauseUser) &&
            !pm::video::pause_has(prev, pm::video::PauseUser))
            ui_log_session_video("paused (user)");
        else if (!pm::video::pause_has(next, pm::video::PauseUser) &&
                 pm::video::pause_has(prev, pm::video::PauseUser))
            ui_log_session_video("resumed (user)");
        else if (pm::video::pause_has(next, pm::video::PauseIdle) &&
                 !pm::video::pause_has(prev, pm::video::PauseIdle))
            ui_log_session_video("auto-paused (idle)");
        else if (!pm::video::pause_has(next, pm::video::PauseIdle) &&
                 pm::video::pause_has(prev, pm::video::PauseIdle))
            ui_log_session_video("auto-resumed");
    }

    bool paused() const noexcept
    {
        return !pm::video::pause_recording_active(
            m_pauseReasons.load(std::memory_order_relaxed));
    }

    void request_user_stop() noexcept
    {
        if (m_userStopRequested.exchange(true, std::memory_order_acq_rel))
            return;
        ui_log_session_video(
            "stop shortcut pressed: " +
            (m_transform.stop_key.empty() ? std::string("stop")
                                          : m_transform.stop_key));
        try {
            const std::weak_ptr<RecSession> weak{shared_from_this()};
            std::thread([weak] {
                if (const auto session = weak.lock())
                    session->user_stop();
            }).detach();
        } catch (...) {
            // FrameArrived still honors the flag if the worker cannot start.
        }
    }

    void user_stop() noexcept
    {
        bool expected = false;
        if (!m_stopping.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel))
            return;
        ui_log_session_video(
            "stop shortcut: closing WGC + finalizing MP4");
        stop_and_finalize();
        {
            std::lock_guard<std::mutex> l(g_mux);
            g_lastStatus = status();
            if (g_sess.get() == this) g_sess.reset();
        }
        mf_release();
    }

    bool update_transform(
        const VideoTransformUpdate& update, std::string& error) noexcept
    {
        error.clear();
        if (!m_transformEnabled) {
            error =
                "video transform update requires zoom/follow or a fixed output "
                "canvas when recording starts";
            return false;
        }
        std::unique_lock<std::mutex> lock(m_transformMutex);
        const auto now = std::chrono::steady_clock::now();
        if (update.transition_ms >= 0)
            m_transform.transition_ms =
                std::clamp(update.transition_ms, 0, 10000);
        if (update.zoom >= 0.0) {
            m_zoomStart = current_zoom_locked(now);
            m_zoomTarget = std::clamp(
                update.zoom, m_transform.zoom_min, m_transform.zoom_max);
            m_zoomTransitionStart = now;
            m_transform.zoom = m_zoomTarget;
        }
        if (!update.follow.empty()) {
            std::string follow = update.follow;
            follow = pm::video::lower_ascii(std::move(follow));
            if (follow != "none" && follow != "anchor" &&
                follow != "cursor") {
                error = "follow must be none, anchor, or cursor";
                return false;
            }
            m_transform.follow = std::move(follow);
        }
        if (update.follow_speed >= 0.0)
            m_transform.follow_speed =
                std::clamp(update.follow_speed, 0.01, 1.0);
        if (update.follow_deadzone >= 0)
            m_transform.follow_deadzone =
                std::clamp(update.follow_deadzone, 0, 1000);
        if (update.focus_x >= 0.0 && update.focus_y >= 0.0) {
            m_transform.focus_x = std::clamp(update.focus_x, 0.0, 1.0);
            m_transform.focus_y = std::clamp(update.focus_y, 0.0, 1.0);
        }
        const double displayedZoom = m_zoomTarget;
        const std::string follow = m_transform.follow;
        lock.unlock();
        ui_log_session_video(
            "viewport update: zoom=" + std::to_string(displayedZoom) +
            " follow=" + follow);
        if (const auto controls = m_interactiveControls.load()) {
            controls->update(interactive_state(displayedZoom));
        }
        refresh_camera_overlay_anchor();
        return true;
    }

    pm::video::RecordingStatus status() const noexcept
    {
        pm::video::RecordingStatus result;
        result.capture_backend = "fast-wgc";
        result.effective_encoder = m_effectiveEncoder;
        result.width = m_encW;
        result.height = m_encH;
        result.fps = m_fps;
        result.bitrate_kbps = m_effectiveBitrateKbps;
        {
            std::lock_guard<std::mutex> lock(m_transformMutex);
            result.zoom = m_currentZoom;
            result.follow = m_transform.follow;
            result.viewport_left = m_lastViewport.left;
            result.viewport_top = m_lastViewport.top;
            result.viewport_width =
                m_lastViewport.right - m_lastViewport.left;
            result.viewport_height =
                m_lastViewport.bottom - m_lastViewport.top;
        }
        result.frames_written = m_framesWritten.load(std::memory_order_relaxed);
        result.frames_dropped = m_droppedFrames.load(std::memory_order_relaxed);
        result.paused = paused();
        result.pause_reasons = m_pauseReasons.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(m_statusMutex);
            result.error = m_lastErr;
            result.warning = m_softWarning;
        }
        if (!result.error.empty())
            result.state = pm::video::RecordingState::failed;
        else if (m_recordingStarted.load(std::memory_order_acquire) &&
                 !m_stopping.load(std::memory_order_relaxed))
            result.state = pm::video::RecordingState::recording;
        else if (!m_stopping.load(std::memory_order_relaxed))
            result.state = pm::video::RecordingState::starting;
        else
            result.state = pm::video::RecordingState::stopped;
        return result;
    }

    ~RecSession()
    {
        if (m_session || m_pool || m_sink) stop_and_finalize();
    }

private:
    RecSession() = default;

    struct TransformRects {
        RECT source{};
        RECT destination{};
    };

    double current_zoom_locked(
        std::chrono::steady_clock::time_point now) const noexcept
    {
        if (m_transform.transition_ms <= 0 ||
            m_zoomStart == m_zoomTarget) {
            return m_zoomTarget;
        }
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(now - m_zoomTransitionStart).count();
        const double t = std::clamp(
            static_cast<double>(elapsed) /
                static_cast<double>(m_transform.transition_ms),
            0.0, 1.0);
        const double eased = t * t * (3.0 - 2.0 * t);
        return m_zoomStart + (m_zoomTarget - m_zoomStart) * eased;
    }

    bool source_screen_bounds(RECT& bounds) const noexcept
    {
        // Frame-pool callbacks may not inherit the process DPI context.
        media::win::ThreadDpiPerMonitorScope dpi_scope;
        if (m_target.window) {
            if (!media::win::visible_window_bounds(
                    m_target.window, bounds)) {
                return false;
            }
        } else if (m_target.monitor) {
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (!::GetMonitorInfoW(m_target.monitor, &mi)) return false;
            bounds = mi.rcMonitor;
        } else {
            return false;
        }
        return true;
    }

    bool screen_point_in_source(
        POINT point, double& x, double& y) const noexcept
    {
        RECT bounds{};
        if (!source_screen_bounds(bounds)) return false;
        // Content-space coords (visible window / monitor), matching m_base*.
        x = static_cast<double>(point.x - bounds.left);
        y = static_cast<double>(point.y - bounds.top);
        return true;
    }

    bool cursor_in_source(double& x, double& y) const noexcept
    {
        media::win::ThreadDpiPerMonitorScope dpi_scope;
        POINT cursor{};
        if (!::GetCursorPos(&cursor)) return false;
        return screen_point_in_source(cursor, x, y);
    }

    TransformRects transform_rects(
        int contentW, int contentH,
        std::chrono::steady_clock::time_point now) noexcept
    {
        std::lock_guard<std::mutex> lock(m_transformMutex);
        const int baseL = std::clamp(m_baseL, 0, std::max(contentW - 1, 0));
        const int baseT = std::clamp(m_baseT, 0, std::max(contentH - 1, 0));
        const int baseW = std::max(
            2, std::min(m_baseW, contentW - baseL));
        const int baseH = std::max(
            2, std::min(m_baseH, contentH - baseT));
        const double zoom = std::max(1.0, current_zoom_locked(now));

        double targetX = baseL + baseW * 0.5;
        double targetY = baseT + baseH * 0.5;
        if (m_transform.focus_x >= 0.0 &&
            m_transform.focus_y >= 0.0) {
            targetX = baseL + baseW * m_transform.focus_x;
            targetY = baseT + baseH * m_transform.focus_y;
        }
        if (m_transform.follow == "cursor") {
            double cursorX = 0.0, cursorY = 0.0;
            if (cursor_in_source(cursorX, cursorY)) {
                targetX = cursorX;
                targetY = cursorY;
            }
        }
        if (!m_centerInitialized) {
            m_centerX = targetX;
            m_centerY = targetY;
            m_centerInitialized = true;
        } else if (m_transform.follow == "cursor") {
            const double dx = targetX - m_centerX;
            const double dy = targetY - m_centerY;
            const double deadzone =
                static_cast<double>(m_transform.follow_deadzone);
            if (std::abs(dx) > deadzone)
                m_centerX += dx * m_transform.follow_speed;
            if (std::abs(dy) > deadzone)
                m_centerY += dy * m_transform.follow_speed;
        } else {
            m_centerX = targetX;
            m_centerY = targetY;
        }

        double viewW = std::max(2.0, baseW / zoom);
        double viewH = std::max(2.0, baseH / zoom);
        RECT destination{0, 0, m_encW, m_encH};
        const double canvasAspect =
            m_encH > 0
                ? static_cast<double>(m_encW) / static_cast<double>(m_encH)
                : 0.0;
        // Lock the zoom/follow crop to a ratio when the user chose aspectRatio,
        // or when sizeMode=fill (canvas-cover crop). Otherwise keep the source
        // region's native ratio and letterbox into the encode canvas.
        const double lockAspect =
            m_transform.aspect_ratio > 0.0
                ? m_transform.aspect_ratio
                : (m_transform.size_mode == "fill" ? canvasAspect : 0.0);
        if (lockAspect > 0.0) {
            const double viewAspect = viewW / viewH;
            if (viewAspect > lockAspect)
                viewW = viewH * lockAspect;
            else
                viewH = viewW / lockAspect;
        }
        if (m_transform.size_mode == "fill") {
            // Destination stays full-frame; crop already matches lockAspect.
        } else {
            double scale = std::min(
                static_cast<double>(m_encW) / viewW,
                static_cast<double>(m_encH) / viewH);
            if (m_transform.size_mode == "native" ||
                m_transform.size_mode == "pad") {
                scale = std::min(1.0, scale);
            }
            const int dw = std::max(
                1, static_cast<int>(std::lround(viewW * scale)));
            const int dh = std::max(
                1, static_cast<int>(std::lround(viewH * scale)));
            destination.left = (m_encW - dw) / 2;
            destination.top = (m_encH - dh) / 2;
            destination.right = destination.left + dw;
            destination.bottom = destination.top + dh;
        }

        const double halfW = viewW * 0.5;
        const double halfH = viewH * 0.5;
        m_centerX = std::clamp(
            m_centerX, baseL + halfW, baseL + baseW - halfW);
        m_centerY = std::clamp(
            m_centerY, baseT + halfH, baseT + baseH - halfH);
        const int left = std::clamp(
            static_cast<int>(std::lround(m_centerX - halfW)),
            baseL, baseL + baseW - 2);
        const int top = std::clamp(
            static_cast<int>(std::lround(m_centerY - halfH)),
            baseT, baseT + baseH - 2);
        const int width = std::max(
            2, std::min(
                static_cast<int>(std::lround(viewW)),
                baseL + baseW - left));
        const int height = std::max(
            2, std::min(
                static_cast<int>(std::lround(viewH)),
                baseT + baseH - top));
        m_currentZoom = zoom;
        m_lastViewport = {left, top, left + width, top + height};
        return {{left, top, left + width, top + height}, destination};
    }

    bool init_transform_processor(int sourceW, int sourceH) noexcept
    {
        m_transformSourceW = sourceW;
        m_transformSourceH = sourceH;
        m_videoOutputViews.clear();
        m_videoProcessor = nullptr;
        m_videoEnumerator = nullptr;
        m_videoContext = nullptr;
        m_videoDevice = nullptr;
        HRESULT hr = m_d3d->QueryInterface(
            IID_PPV_ARGS(m_videoDevice.put()));
        if (SUCCEEDED(hr)) {
            hr = m_ctx->QueryInterface(
                IID_PPV_ARGS(m_videoContext.put()));
        }
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
        desc.InputFrameFormat =
            D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        desc.InputFrameRate = {
            static_cast<UINT>(m_fps), 1};
        desc.InputWidth = static_cast<UINT>(sourceW);
        desc.InputHeight = static_cast<UINT>(sourceH);
        desc.OutputFrameRate = {
            static_cast<UINT>(m_fps), 1};
        desc.OutputWidth = static_cast<UINT>(m_encW);
        desc.OutputHeight = static_cast<UINT>(m_encH);
        desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        if (SUCCEEDED(hr)) {
            hr = m_videoDevice->CreateVideoProcessorEnumerator(
                &desc, m_videoEnumerator.put());
        }
        if (SUCCEEDED(hr)) {
            hr = m_videoDevice->CreateVideoProcessor(
                m_videoEnumerator.get(), 0, m_videoProcessor.put());
        }
        if (FAILED(hr)) {
            set_error(hresult_failure(
                "D3D11 video processor initialization", hr));
            return false;
        }
        D3D11_VIDEO_COLOR background{};
        background.RGBA.A = 1.0f;
        m_videoContext->VideoProcessorSetOutputBackgroundColor(
            m_videoProcessor.get(), FALSE, &background);
        m_videoContext->VideoProcessorSetStreamFrameFormat(
            m_videoProcessor.get(), 0,
            D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        m_videoOutputViews.reserve(m_staging.size());
        for (const auto& texture : m_staging) {
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
            outputDesc.ViewDimension =
                D3D11_VPOV_DIMENSION_TEXTURE2D;
            outputDesc.Texture2D.MipSlice = 0;
            winrt::com_ptr<ID3D11VideoProcessorOutputView> output;
            hr = m_videoDevice->CreateVideoProcessorOutputView(
                texture.get(), m_videoEnumerator.get(),
                &outputDesc, output.put());
            if (FAILED(hr)) {
                set_error(hresult_failure(
                    "D3D11 video processor output view", hr));
                return false;
            }
            m_videoOutputViews.push_back(std::move(output));
        }
        ui_log_session_video(
            "D3D11 video processor ready: " +
            std::to_string(sourceW) + "x" + std::to_string(sourceH) +
            " -> " + std::to_string(m_encW) + "x" +
            std::to_string(m_encH) + " mode=" +
            m_transform.size_mode);
        return true;
    }

    bool transform_frame(
        ID3D11Texture2D* source, int outputIndex,
        int contentW, int contentH,
        std::chrono::steady_clock::time_point capturedAt) noexcept
    {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
        inputDesc.FourCC = 0;
        inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inputDesc.Texture2D.MipSlice = 0;
        inputDesc.Texture2D.ArraySlice = 0;
        winrt::com_ptr<ID3D11VideoProcessorInputView> input;
        HRESULT hr = m_videoDevice->CreateVideoProcessorInputView(
            source, m_videoEnumerator.get(), &inputDesc, input.put());
        if (FAILED(hr)) {
            set_error(hresult_failure(
                "D3D11 video processor input view", hr));
            return false;
        }
        const auto rects = transform_rects(
            contentW, contentH, capturedAt);
        RECT sourceTex = rects.source;
        ::OffsetRect(
            &sourceTex, m_contentOriginL, m_contentOriginT);
        m_videoContext->VideoProcessorSetStreamSourceRect(
            m_videoProcessor.get(), 0, TRUE, &sourceTex);
        m_videoContext->VideoProcessorSetStreamDestRect(
            m_videoProcessor.get(), 0, TRUE, &rects.destination);
        const RECT outputRect{0, 0, m_encW, m_encH};
        m_videoContext->VideoProcessorSetOutputTargetRect(
            m_videoProcessor.get(), TRUE, &outputRect);
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.OutputIndex = 0;
        stream.InputFrameOrField =
            static_cast<UINT>(m_videoFrameIndex++);
        stream.PastFrames = 0;
        stream.FutureFrames = 0;
        stream.pInputSurface = input.get();
        hr = m_videoContext->VideoProcessorBlt(
            m_videoProcessor.get(),
            m_videoOutputViews[
                static_cast<size_t>(outputIndex)].get(),
            0, 1, &stream);
        if (FAILED(hr)) {
            set_error(hresult_failure(
                "D3D11 video processor blit", hr));
            return false;
        }
        if (const auto controls = m_interactiveControls.load()) {
            controls->update(interactive_state(), false);
        }
        refresh_camera_overlay_anchor();
        return true;
    }

    pm::video::InteractiveZoomState interactive_state(
        double displayedZoom = -1.0) const noexcept
    {
        pm::video::InteractiveZoomState state;
        RECT sourceBounds{};
        {
            std::lock_guard<std::mutex> lock(m_transformMutex);
            state.zoom = displayedZoom >= 0.0
                ? displayedZoom : m_currentZoom;
            state.output_width = m_encW;
            state.output_height = m_encH;
            state.viewport_screen = m_lastViewport;
            state.viewport_visible =
                m_transform.show_zoom_status &&
                m_lastViewport.right > m_lastViewport.left &&
                m_lastViewport.bottom > m_lastViewport.top;
            if (m_transform.follow == "cursor")
                state.follow_mode =
                    pm::video::InteractiveFollowMode::cursor;
            else if (m_transform.follow == "anchor")
                state.follow_mode =
                    pm::video::InteractiveFollowMode::anchor;
        }
        if (state.viewport_visible &&
            source_screen_bounds(sourceBounds)) {
            ::OffsetRect(
                &state.viewport_screen,
                sourceBounds.left, sourceBounds.top);
        } else {
            state.viewport_visible = false;
        }
        state.recording =
            !m_stopping.load(std::memory_order_relaxed);
        state.paused = paused();
        return state;
    }

    void interactive_zoom(int steps, POINT cursor) noexcept
    {
        if (steps == 0) return;
        double targetZoom = 1.0;
        {
            std::lock_guard<std::mutex> lock(m_transformMutex);
            const auto now = std::chrono::steady_clock::now();
            m_zoomStart = current_zoom_locked(now);
            m_zoomTarget = std::clamp(
                m_zoomTarget +
                    static_cast<double>(steps) * m_transform.zoom_step,
                m_transform.zoom_min, m_transform.zoom_max);
            m_transform.zoom = m_zoomTarget;
            m_zoomTransitionStart = now;
            double sourceX = 0.0, sourceY = 0.0;
            if (screen_point_in_source(
                    cursor, sourceX, sourceY)) {
                m_transform.focus_x = std::clamp(
                    (sourceX - m_baseL) /
                        static_cast<double>(std::max(m_baseW, 1)),
                    0.0, 1.0);
                m_transform.focus_y = std::clamp(
                    (sourceY - m_baseT) /
                        static_cast<double>(std::max(m_baseH, 1)),
                    0.0, 1.0);
                m_centerX = sourceX;
                m_centerY = sourceY;
                m_centerInitialized = true;
            }
            targetZoom = m_zoomTarget;
        }
        ui_log_session_video(
            "interactive zoom: " + std::to_string(targetZoom) + "x");
        if (const auto controls = m_interactiveControls.load()) {
            controls->update(interactive_state(targetZoom));
        }
    }

    void start_interactive_controls() noexcept
    {
        auto controls =
            std::make_shared<pm::video::WinInteractiveZoomController>();
        pm::video::InteractiveZoomOptions options;
        options.enable_wheel_input = m_transform.interactive_zoom;
        options.consume_input =
            m_transform.interactive_zoom &&
            m_transform.consume_zoom_input;
        options.show_hud = m_transform.show_zoom_status;
        options.show_viewport_border =
            m_transform.interactive_zoom &&
            m_transform.show_zoom_status;
        options.overlays_visible_to_capture =
            m_transform.hud_capture == "visible";
        options.pause_modifiers = m_pauseModifiers;
        options.pause_virtual_key = m_pauseVirtualKey;
        options.stop_modifiers = m_stopModifiers;
        options.stop_virtual_key = m_stopVirtualKey;
        options.key_overlay = m_transform.key_overlay;
        options.cursor_fx = m_transform.cursor_fx;
        options.track_input_activity = m_transform.idle_pause.enabled() &&
            (m_transform.idle_pause.wake_key || m_transform.idle_pause.wake_mouse);
        std::string error;
        const std::weak_ptr<RecSession> weak{shared_from_this()};
        pm::video::InteractiveZoomWheelCallback wheelCallback;
        if (m_transform.interactive_zoom) {
            wheelCallback = [weak](int steps, POINT cursor) {
                if (const auto session = weak.lock())
                    session->interactive_zoom(steps, cursor);
            };
        }
        if (!controls->start(
                options, interactive_state(m_zoomTarget),
                std::move(wheelCallback),
                [weak] {
                    if (const auto session = weak.lock()) {
                        session->toggle_pause();
                        if (const auto controls =
                                session->m_interactiveControls.load()) {
                            controls->update(
                                session->interactive_state());
                        }
                    }
                },
                [weak] {
                    if (const auto session = weak.lock())
                        session->request_user_stop();
                },
                &error)) {
            const std::string msg =
                (m_transform.interactive_zoom
                     ? "interactive zoom unavailable: "
                     : "recording pause/stop/HUD controls unavailable: ") +
                error;
            ui_log_session_video(msg);
            append_soft_warning(msg);
            return;
        }
        if (!error.empty()) {
            ui_log_session_video("interactive zoom: " + error);
            append_soft_warning(error);
        }
        m_interactiveControls.store(std::move(controls));
        if (const auto started = m_interactiveControls.load()) {
            started->set_input_activity_callback(
                [weak](pm::video::IdleInputKind kind, POINT screen) {
                    if (const auto session = weak.lock())
                        session->on_idle_input(kind, screen);
                });
        }
        if (m_transform.interactive_zoom)
            ui_log_session_video(
                "interactive zoom ready: hold Shift and use mouse wheel");
        if (m_transform.show_zoom_status &&
            m_transform.hud_capture == "visible")
            ui_log_session_video(
                "HUD capture mode: visible to OBS and other screen recorders");
        if (m_pauseVirtualKey != 0)
            ui_log_session_video(
                "pause shortcut ready: " + m_transform.pause_key);
        if (m_stopVirtualKey != 0)
            ui_log_session_video(
                "stop shortcut ready: " + m_transform.stop_key);
        if (m_transform.key_overlay.enabled) {
            ui_log_session_video(
                std::string("key overlay ready: align=") +
                pm::video::key_overlay_align_name(
                    m_transform.key_overlay.align) +
                " filter=" +
                pm::video::key_overlay_filter_name(
                    m_transform.key_overlay.filter) +
                " capture=" + m_transform.key_overlay.capture);
            if (m_transform.key_overlay.burn_in)
                ui_log_session_video(
                    "key overlay burn-in requested but not yet composited "
                    "into encoded frames; use keyOverlayCapture=visible "
                    "to include the on-screen overlay in the MP4");
        }
        if (m_transform.cursor_fx.any()) {
            std::string fx = "cursor fx ready:";
            if (m_transform.cursor_fx.highlight) fx += " highlight";
            if (m_transform.cursor_fx.clicks) fx += " clicks";
            if (m_transform.cursor_fx.trail) fx += " trail";
            if (m_transform.cursor_fx.hide_when_typing)
                fx += " hide-when-typing";
            ui_log_session_video(fx);
        }
    }

    /// Screen rect for PiP corners: active zoom/follow viewport when known,
    /// otherwise the full capture monitor/window (same mapping as the zoom HUD).
    bool camera_overlay_anchor_bounds(RECT& bounds) const noexcept
    {
        RECT source{};
        if (!source_screen_bounds(source)) return false;
        RECT viewport{};
        {
            std::lock_guard<std::mutex> lock(m_transformMutex);
            viewport = m_lastViewport;
        }
        if (viewport.right > viewport.left &&
            viewport.bottom > viewport.top) {
            bounds = viewport;
            ::OffsetRect(&bounds, source.left, source.top);
            return true;
        }
        bounds = source;
        return true;
    }

    void on_idle_input(pm::video::IdleInputKind kind, POINT screen) noexcept
    {
        switch (kind) {
        case pm::video::IdleInputKind::key:
            m_idleDetector.note_key();
            m_idleHooksLive.store(true, std::memory_order_relaxed);
            break;
        case pm::video::IdleInputKind::click:
            m_idleDetector.note_click_or_wheel();
            m_idleHooksLive.store(true, std::memory_order_relaxed);
            break;
        case pm::video::IdleInputKind::move:
            m_idleDetector.note_mouse_screen(screen.x, screen.y);
            m_idleHooksLive.store(true, std::memory_order_relaxed);
            break;
        }
    }

    void collect_dirty_rects(
        const winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame& fr,
        int cropL, int cropT, int cropW, int cropH,
        std::vector<pm::video::IdleDirtyRect>& out) noexcept
    {
        out.clear();
        if constexpr (requires(winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame f) {
                          f.DirtyRegions();
                      }) {
            try {
                const auto regions = fr.DirtyRegions();
                const uint32_t n = regions ? regions.Size() : 0;
                for (uint32_t i = 0; i < n; ++i) {
                    const auto r = regions.GetAt(i);
                    int x = static_cast<int>(r.X) - cropL;
                    int y = static_cast<int>(r.Y) - cropT;
                    int w = static_cast<int>(r.Width);
                    int h = static_cast<int>(r.Height);
                    if (x < 0) { w += x; x = 0; }
                    if (y < 0) { h += y; y = 0; }
                    if (x >= cropW || y >= cropH || w <= 0 || h <= 0)
                        continue;
                    w = std::min(w, cropW - x);
                    h = std::min(h, cropH - y);
                    const int pw = m_idleDetector.probe_w();
                    const int ph = m_idleDetector.probe_h();
                    pm::video::IdleDirtyRect d;
                    d.x = x * pw / std::max(1, cropW);
                    d.y = y * ph / std::max(1, cropH);
                    d.w = std::max(1, w * pw / std::max(1, cropW));
                    d.h = std::max(1, h * ph / std::max(1, cropH));
                    out.push_back(d);
                }
            } catch (...) {
            }
        }
    }

    void reset_idle_probe_gpu() noexcept
    {
        m_idleOutputView = nullptr;
        m_idleProcessor = nullptr;
        m_idleEnumerator = nullptr;
        m_idleVideoContext = nullptr;
        m_idleVideoDevice = nullptr;
        m_idleCpu = nullptr;
        m_idleGpu = nullptr;
        m_idleSourceW = 0;
        m_idleSourceH = 0;
    }

    bool ensure_idle_probe(int texW, int texH) noexcept
    {
        if (m_idleProbeFailed)
            return false;
        if (texW < 2 || texH < 2 || !m_d3d || !m_ctx)
            return false;
        const int pw = m_idleDetector.probe_w();
        const int ph = m_idleDetector.probe_h();
        if (m_idleCpu && m_idleProcessor && m_idleSourceW == texW &&
            m_idleSourceH == texH)
            return true;
        reset_idle_probe_gpu();
        HRESULT hr = m_d3d->QueryInterface(
            IID_PPV_ARGS(m_idleVideoDevice.put()));
        if (SUCCEEDED(hr))
            hr = m_ctx->QueryInterface(
                IID_PPV_ARGS(m_idleVideoContext.put()));
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(pw);
        td.Height = static_cast<UINT>(ph);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc = {1, 0};
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (SUCCEEDED(hr))
            hr = m_d3d->CreateTexture2D(&td, nullptr, m_idleGpu.put());
        if (SUCCEEDED(hr)) {
            D3D11_TEXTURE2D_DESC cpu = td;
            cpu.Usage = D3D11_USAGE_STAGING;
            cpu.BindFlags = 0;
            cpu.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            hr = m_d3d->CreateTexture2D(&cpu, nullptr, m_idleCpu.put());
        }
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
        desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        desc.InputFrameRate = {static_cast<UINT>(m_fps), 1};
        desc.InputWidth = static_cast<UINT>(texW);
        desc.InputHeight = static_cast<UINT>(texH);
        desc.OutputFrameRate = {static_cast<UINT>(m_fps), 1};
        desc.OutputWidth = static_cast<UINT>(pw);
        desc.OutputHeight = static_cast<UINT>(ph);
        desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        if (SUCCEEDED(hr))
            hr = m_idleVideoDevice->CreateVideoProcessorEnumerator(
                &desc, m_idleEnumerator.put());
        if (SUCCEEDED(hr))
            hr = m_idleVideoDevice->CreateVideoProcessor(
                m_idleEnumerator.get(), 0, m_idleProcessor.put());
        if (SUCCEEDED(hr)) {
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{};
            ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            ov.Texture2D.MipSlice = 0;
            hr = m_idleVideoDevice->CreateVideoProcessorOutputView(
                m_idleGpu.get(), m_idleEnumerator.get(), &ov,
                m_idleOutputView.put());
        }
        if (FAILED(hr)) {
            reset_idle_probe_gpu();
            if (!m_idleProbeFailed) {
                m_idleProbeFailed = true;
                ui_log_session_video(
                    "idle visual probe unavailable (video processor); "
                    "using input/mic activity only");
            }
            return false;
        }
        m_idleVideoContext->VideoProcessorSetStreamFrameFormat(
            m_idleProcessor.get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        m_idleSourceW = texW;
        m_idleSourceH = texH;
        return true;
    }

    void probe_idle_frame(ID3D11Texture2D* src, int cropL, int cropT,
                          int cropW, int cropH, int texW, int texH,
                          const pm::video::IdleDirtyRect* dirty, int dirty_n,
                          std::chrono::steady_clock::time_point now) noexcept
    {
        if (!m_transform.idle_pause.enabled() ||
            !m_transform.idle_pause.uses_visual() || !src)
            return;
        const auto interval = std::chrono::milliseconds(
            std::max(1, 1000 / std::max(1, m_transform.idle_pause.sample_fps)));
        if (m_haveIdleProbe && now < m_nextIdleProbe)
            return;
        m_haveIdleProbe = true;
        m_nextIdleProbe = now + interval;
        if (!ensure_idle_probe(texW, texH))
            return;
        const int pw = m_idleDetector.probe_w();
        const int ph = m_idleDetector.probe_h();
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
        inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        winrt::com_ptr<ID3D11VideoProcessorInputView> input;
        HRESULT hr = m_idleVideoDevice->CreateVideoProcessorInputView(
            src, m_idleEnumerator.get(), &inputDesc, input.put());
        if (FAILED(hr))
            return;
        RECT srcRect{cropL, cropT, cropL + cropW, cropT + cropH};
        RECT dstRect{0, 0, pw, ph};
        m_idleVideoContext->VideoProcessorSetStreamSourceRect(
            m_idleProcessor.get(), 0, TRUE, &srcRect);
        m_idleVideoContext->VideoProcessorSetStreamDestRect(
            m_idleProcessor.get(), 0, TRUE, &dstRect);
        m_idleVideoContext->VideoProcessorSetOutputTargetRect(
            m_idleProcessor.get(), TRUE, &dstRect);
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.OutputIndex = 0;
        stream.InputFrameOrField = static_cast<UINT>(m_idleVpFrame++);
        stream.pInputSurface = input.get();
        hr = m_idleVideoContext->VideoProcessorBlt(
            m_idleProcessor.get(), m_idleOutputView.get(), 0, 1, &stream);
        if (FAILED(hr))
            return;
        m_ctx->CopyResource(m_idleCpu.get(), m_idleGpu.get());
        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(m_ctx->Map(m_idleCpu.get(), 0, D3D11_MAP_READ, 0, &map)))
            return;
        m_idleDetector.feed_bgra(
            static_cast<const std::uint8_t*>(map.pData), pw, ph,
            static_cast<int>(map.RowPitch), dirty, dirty_n);
        m_ctx->Unmap(m_idleCpu.get(), 0);
    }

    void tick_idle(std::chrono::steady_clock::time_point now) noexcept
    {
        if (!m_transform.idle_pause.enabled())
            return;
        if (!m_idleHooksLive.load(std::memory_order_relaxed)) {
            LASTINPUTINFO li{};
            li.cbSize = sizeof(li);
            if (::GetLastInputInfo(&li)) {
                const DWORD dt = ::GetTickCount() - li.dwTime;
                if (dt < static_cast<DWORD>(
                        std::max(50, m_transform.idle_pause.key_hold_ms)))
                    m_idleDetector.note_last_input_tick();
            }
        }
        const auto t = m_idleDetector.tick(now);
        const bool user = pm::video::pause_has(
            m_pauseReasons.load(std::memory_order_relaxed),
            pm::video::PauseUser);
        if (user)
            return;
        set_pause_reason(pm::video::PauseIdle, t.want_idle_pause);
    }

    void discard_preroll() noexcept
    {
        std::lock_guard<std::mutex> lock(m_encodeMutex);
        while (!m_preroll.empty()) {
            m_freeStaging.push_back(m_preroll.front().staging_index);
            m_preroll.pop_front();
        }
    }

    void flush_preroll() noexcept
    {
        bool any = false;
        {
            std::lock_guard<std::mutex> lock(m_encodeMutex);
            while (!m_preroll.empty()) {
                m_encodeQueue.push_back(m_preroll.front());
                m_preroll.pop_front();
                any = true;
            }
        }
        if (any)
            m_encodeReady.notify_one();
    }

    void copy_to_preroll(ID3D11Texture2D* src, int contentW, int contentH,
                         int cropL, int cropT, int encCx, int encCy,
                         int texW, int texH,
                         std::chrono::steady_clock::time_point capturedAt) noexcept
    {
        const int maxn = m_transform.idle_pause.preroll_frames(m_fps);
        if (maxn <= 0 || !src)
            return;
        if (m_transformEnabled &&
            (texW > m_transformSourceW || texH > m_transformSourceH ||
             !m_videoProcessor) &&
            !init_transform_processor(
                std::max(texW, m_transformSourceW),
                std::max(texH, m_transformSourceH)))
            return;
        int stagingIndex = -1;
        {
            std::lock_guard<std::mutex> lock(m_encodeMutex);
            if (!m_freeStaging.empty()) {
                stagingIndex = m_freeStaging.front();
                m_freeStaging.pop_front();
            } else if (!m_preroll.empty()) {
                stagingIndex = m_preroll.front().staging_index;
                m_preroll.pop_front();
            }
        }
        if (stagingIndex < 0)
            return;
        bool ok = true;
        if (m_transformEnabled) {
            ok = transform_frame(src, stagingIndex, contentW, contentH, capturedAt);
        } else {
            const D3D11_BOX srcBox{
                static_cast<UINT>(cropL),
                static_cast<UINT>(cropT),
                0,
                static_cast<UINT>(cropL + encCx),
                static_cast<UINT>(cropT + encCy),
                1,
            };
            m_ctx->CopySubresourceRegion(
                m_staging[static_cast<size_t>(stagingIndex)].get(),
                0, 0, 0, 0, src, 0, &srcBox);
        }
        std::lock_guard<std::mutex> lock(m_encodeMutex);
        if (!ok) {
            m_freeStaging.push_back(stagingIndex);
            return;
        }
        m_preroll.push_back({stagingIndex, capturedAt});
        while (static_cast<int>(m_preroll.size()) > maxn) {
            m_freeStaging.push_back(m_preroll.front().staging_index);
            m_preroll.pop_front();
        }
    }

    void start_camera_overlay() noexcept
    {
        RECT bounds{};
        if (!camera_overlay_anchor_bounds(bounds)) {
            const std::string msg =
                "camera overlay unavailable: could not resolve capture bounds";
            ui_log_session_video(msg);
            append_soft_warning(msg);
            return;
        }
        auto overlay =
            std::make_shared<pm::video::WinCameraOverlayController>();
        std::string error;
        if (!overlay->start(m_transform.camera_overlay, bounds, &error)) {
            const std::string msg =
                "camera overlay unavailable: " +
                (error.empty() ? std::string("start failed") : error);
            ui_log_session_video(msg);
            append_soft_warning(msg);
            return;
        }
        m_cameraAnchor = bounds;
        m_cameraAnchorValid = true;
        m_cameraOverlay.store(std::move(overlay));
        ui_log_session_video(
            std::string("camera overlay ready: position=") +
            (m_transform.camera_overlay.position ==
                     pm::video::CameraOverlayPosition::top_left
                 ? "top-left"
                 : m_transform.camera_overlay.position ==
                           pm::video::CameraOverlayPosition::top_right
                       ? "top-right"
                       : m_transform.camera_overlay.position ==
                                 pm::video::CameraOverlayPosition::bottom_left
                           ? "bottom-left"
                           : "bottom-right") +
            " size=" +
            std::to_string(static_cast<int>(m_transform.camera_overlay.size)) +
            " (tracks zoom/follow viewport)" +
            (m_transform.camera_overlay.input.empty()
                 ? " (default camera)"
                 : " input=" + m_transform.camera_overlay.input));
    }

    void refresh_camera_overlay_anchor() noexcept
    {
        const auto overlay = m_cameraOverlay.load();
        if (!overlay) return;
        RECT bounds{};
        if (!camera_overlay_anchor_bounds(bounds)) return;
        if (m_cameraAnchorValid &&
            bounds.left == m_cameraAnchor.left &&
            bounds.top == m_cameraAnchor.top &&
            bounds.right == m_cameraAnchor.right &&
            bounds.bottom == m_cameraAnchor.bottom)
            return;
        m_cameraAnchor = bounds;
        m_cameraAnchorValid = true;
        overlay->update_anchor(bounds);
    }

    void set_soft_warning(std::string warning) noexcept
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_softWarning = std::move(warning);
    }

    void append_soft_warning(std::string warning) noexcept
    {
        if (warning.empty()) return;
        std::lock_guard<std::mutex> lock(m_statusMutex);
        if (m_softWarning.empty())
            m_softWarning = std::move(warning);
        else
            m_softWarning += "; " + warning;
    }

    void set_error(std::string error) noexcept
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_lastErr = std::move(error);
    }

    void set_error_if_empty(std::string error) noexcept
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        if (m_lastErr.empty()) m_lastErr = std::move(error);
    }

    std::string last_error() const noexcept
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        return m_lastErr;
    }

    void err_stop(const char* w) noexcept
    {
        bool expected = false;
        if (!m_stopping.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel))
            return;
        const std::string error = w && *w ? w : "recording stopped unexpectedly";
        set_error(error);
        ui_log_session_video("error, stopping: " + error);
        stop_and_finalize();
        {
            std::lock_guard<std::mutex> l(g_mux);
            g_lastStatus = status();
            if (g_sess.get() == this) g_sess.reset();
        }
        mf_release();
    }

    bool init_encoder(int w, int h)
    {
        m_encW  = w;
        m_encH  = h;
        m_frameIdx = 0;
        std::string sizeErr;
        if (!validate_h264_frame_size(w, h, sizeErr)) {
            set_error(std::move(sizeErr));
            return false;
        }

        const UINT32 bitrate = static_cast<UINT32>(
            m_bitrateKbps > 0
                ? static_cast<long long>(m_bitrateKbps) * 1000LL
                : std::clamp<long long>(
                    static_cast<long long>(w) * h * m_fps / 8,
                    1'000'000LL, 24'000'000LL));
        m_effectiveBitrateKbps = static_cast<int>(bitrate / 1000U);

        m_nvenc.reset();
        if (m_encoderRequest != "mediafoundation") {
            auto nvenc = std::make_unique<NvencPlugin>();
            std::string nvencError;
            if (nvenc->create(m_d3d.get(), w, h, m_fps,
                              static_cast<int>(bitrate / 1000U), nvencError)) {
                m_nvenc = std::move(nvenc);
                m_effectiveEncoder = "nvenc";
                ui_log_session_video("encoder: NVIDIA NVENC plugin");
            } else {
                m_effectiveEncoder = "mediaFoundation";
                const std::string msg =
                    (m_encoderRequest == "nvenc"
                        ? "requested NVENC unavailable ("
                        : "NVENC unavailable (") +
                    nvencError + "); using Media Foundation fallback";
                ui_warn_session_video("encoder: " + msg);
                // Keep fallback visible to callers without failing recording.
                append_soft_warning(msg);
            }
        } else {
            m_effectiveEncoder = "mediaFoundation";
            ui_log_session_video("encoder: Media Foundation (requested)");
        }

        winrt::com_ptr<IMFAttributes> sinkAttributes;
        HRESULT hr = MFCreateAttributes(sinkAttributes.put(), 1);
        if (SUCCEEDED(hr)) {
            // Both video and audio are already paced by live capture. MF's
            // default writer throttling otherwise blocks one producer behind
            // the other, starving the bounded video queue.
            hr = sinkAttributes->SetUINT32(
                MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        }
        if (FAILED(hr)) {
            set_error(hresult_failure("MF sink writer attributes", hr));
            return false;
        }
        winrt::com_ptr<IMFSinkWriter> sw;
        hr = MFCreateSinkWriterFromURL(
            m_outPath.c_str(), nullptr, sinkAttributes.get(), sw.put());
        if (FAILED(hr)) {
            set_error(hresult_failure("MFCreateSinkWriterFromURL", hr));
            return false;
        }
        winrt::com_ptr<IMFMediaType> outT;
        hr = MFCreateMediaType(outT.put());
        if (SUCCEEDED(hr)) hr = outT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(hr)) hr = outT->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        if (SUCCEEDED(hr)) hr = MFSetAttributeSize(
            outT.get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(w), static_cast<UINT32>(h));
        if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(
            outT.get(), MF_MT_FRAME_RATE, static_cast<UINT32>(m_fps), 1);
        if (SUCCEEDED(hr)) hr = outT->SetUINT32(
            MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (SUCCEEDED(hr)) hr = outT->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
        if (FAILED(hr)) {
            set_error(hresult_failure("H.264 output media type configuration", hr));
            return false;
        }
        if (m_nvenc) {
            hr = outT->SetBlob(
                MF_MT_MPEG_SEQUENCE_HEADER,
                m_nvenc->sequence_header().data(),
                static_cast<UINT32>(m_nvenc->sequence_header().size()));
            if (FAILED(hr)) {
                set_error(hresult_failure("H.264 output sequence header", hr));
                return false;
            }
        }
        hr = sw->AddStream(outT.get(), &m_outStreamIndex);
        if (FAILED(hr)) {
            set_error(hresult_failure("Media Foundation AddStream", hr));
            return false;
        }
        winrt::com_ptr<IMFMediaType> inT;
        HRESULT inputHr = MFCreateMediaType(inT.put());
        if (SUCCEEDED(inputHr)) inputHr = inT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(inputHr)) inputHr = inT->SetGUID(
            MF_MT_SUBTYPE, m_nvenc ? MFVideoFormat_H264_ES : MFVideoFormat_RGB32);
        if (SUCCEEDED(inputHr)) inputHr = MFSetAttributeSize(
            inT.get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(w), static_cast<UINT32>(h));
        if (SUCCEEDED(inputHr)) inputHr = MFSetAttributeRatio(
            inT.get(), MF_MT_FRAME_RATE, static_cast<UINT32>(m_fps), 1);
        if (SUCCEEDED(inputHr) && m_nvenc) {
            inputHr = inT->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
            if (SUCCEEDED(inputHr)) inputHr = inT->SetUINT32(MF_MT_COMPRESSED, TRUE);
            if (SUCCEEDED(inputHr)) inputHr = inT->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, FALSE);
            if (SUCCEEDED(inputHr)) inputHr = inT->SetBlob(
                MF_MT_MPEG_SEQUENCE_HEADER,
                m_nvenc->sequence_header().data(),
                static_cast<UINT32>(m_nvenc->sequence_header().size()));
        } else if (SUCCEEDED(inputHr)) {
            inputHr = inT->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(w * 4));
        }
        if (FAILED(inputHr)) {
            set_error(hresult_failure(
                m_nvenc ? "H.264 passthrough input type"
                        : "RGB32 input type",
                inputHr));
            return false;
        }
        hr = sw->SetInputMediaType(m_outStreamIndex, inT.get(), nullptr);
        if (FAILED(hr)) {
            set_error(hresult_failure(
                m_nvenc ? "SetInputMediaType (H.264 passthrough)"
                        : "SetInputMediaType (RGB32 to H.264)",
                hr));
            return false;
        }
#if PM_HAS_AUDIO_IO
        if (m_audio_enabled) {
            m_startStage = "audio stream preparation";
            m_audio_mux = std::make_unique<pm::video::MfVideoAudioMux>();
            pm::video::VideoAudioMuxConfig mux_cfg{m_audio_cfg};
#if defined(FEATURE_AUDIO_FILTER) && FEATURE_AUDIO_FILTER
            if (m_transform.audio_polish.filter_on()) {
                mux_cfg.filter_enabled = true;
                mux_cfg.filter = pm::audio::filter::make_options(
                    m_transform.audio_polish.filter,
                    m_transform.audio_polish.filter_model,
                    m_transform.audio_polish.atten_lim_db,
                    m_transform.audio_polish.post_filter,
                    m_transform.audio_polish.compensate_delay);
            }
#endif
            std::string audioError;
            if (!m_audio_mux->prepare_stream(sw.get(), m_audio_stream, mux_cfg, audioError)) {
                set_error("audio stream setup failed: " + audioError);
                ui_error_session_video(last_error());
                return false;
            }
            ui_warn_session_video(
                "audio stream prepared index=" + std::to_string(m_audio_stream) +
                " source=" + pm::audio::capture_source_name(m_audio_cfg.source));
        }
#endif
        m_startStage = "Media Foundation BeginWriting";
        hr = sw->BeginWriting();
        if (FAILED(hr)) {
            set_error(hresult_failure("Media Foundation BeginWriting", hr));
            ui_error_session_video(last_error());
            return false;
        }
        m_sink = std::move(sw);
#if PM_HAS_AUDIO_IO
        if (m_audio_enabled && m_audio_mux) {
            m_startStage = "audio capture startup";
            pm::video::VideoAudioMuxConfig mux_cfg{m_audio_cfg};
#if defined(FEATURE_AUDIO_FILTER) && FEATURE_AUDIO_FILTER
            if (m_transform.audio_polish.filter_on()) {
                mux_cfg.filter_enabled = true;
                mux_cfg.filter = pm::audio::filter::make_options(
                    m_transform.audio_polish.filter,
                    m_transform.audio_polish.filter_model,
                    m_transform.audio_polish.atten_lim_db,
                    m_transform.audio_polish.post_filter,
                    m_transform.audio_polish.compensate_delay);
            }
#endif
            std::string audioError;
            if (!m_audio_mux->start_capture(
                    m_sink.get(), m_audio_stream, mux_cfg, std::chrono::steady_clock::now(), audioError)) {
                set_error("audio capture start failed: " + audioError);
                ui_error_session_video(last_error());
                m_sink = nullptr;
                return false;
            }
            if (m_transform.idle_pause.enabled() &&
                m_transform.idle_pause.uses_audio()) {
                m_audio_mux->set_speech_meter(
                    [this](const int16_t* pcm, size_t frames, uint32_t ch,
                           uint32_t rate) {
                        m_idleDetector.note_speech_pcm(
                            pcm, frames, static_cast<int>(ch),
                            static_cast<int>(rate));
                    });
            }
#if defined(FEATURE_STT) && FEATURE_STT
            if (m_transform.captions.any()) {
                m_captionDumpPath = m_outPath;
                m_captionDumpPath.replace_extension(".captions.wav");
                m_audio_mux->enable_caption_dump(m_captionDumpPath);
                ui_log_session_video(
                    "captions: sidecar " + m_transform.captions.format);
            }
#endif
            {
                std::ostringstream o;
                o << "MF audio: " << pm::audio::capture_source_name(m_audio_cfg.source)
                  << " (AAC " << 48000 << " Hz stereo); held until the first video frame";
                ui_warn_session_video(o.str());
            }
        } else if (m_transform.captions.any()) {
            ui_log_session_video(
                "captions: sidecar on but audio mux not started — no 16 kHz dump");
        }
#endif
        {
            std::ostringstream o;
            o << "MF sink ready: H.264, " << w << "x" << h << ", " << m_fps
              << " fps, " << (bitrate / 1000U) << " Kbps, "
              << (m_nvenc ? "NVENC Annex-B in" : "RGB32 in");
            ui_log_session_video(o.str());
        }
        {
            m_startStage = "encoder texture-pool allocation";
            D3D11_TEXTURE2D_DESC td = {};
            td.Width          = (UINT)w;
            td.Height         = (UINT)h;
            td.MipLevels      = 1;
            td.ArraySize      = 1;
            td.Format         = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc     = { 1, 0 };
            td.Usage          = (m_nvenc || m_transformEnabled)
                ? D3D11_USAGE_DEFAULT : D3D11_USAGE_STAGING;
            td.BindFlags      = m_transformEnabled
                ? D3D11_BIND_RENDER_TARGET : 0;
            td.CPUAccessFlags = (m_nvenc || m_transformEnabled)
                ? 0 : D3D11_CPU_ACCESS_READ;
            m_staging.clear();
            m_cpuStaging.clear();
            const int pool = kEncodeQueueSize +
                m_transform.idle_pause.preroll_frames(m_fps);
            m_staging.reserve(static_cast<size_t>(pool));
            if (m_transformEnabled && !m_nvenc)
                m_cpuStaging.reserve(static_cast<size_t>(pool));
            for (int i = 0; i < pool; ++i) {
                winrt::com_ptr<ID3D11Texture2D> staging;
                hr = m_d3d->CreateTexture2D(&td, nullptr, staging.put());
                if (FAILED(hr)) {
                    set_error(hresult_failure(
                        "D3D11 encoder staging texture creation", hr));
                    m_sink  = nullptr;
                    return false;
                }
                m_staging.push_back(std::move(staging));
                if (m_transformEnabled && !m_nvenc) {
                    D3D11_TEXTURE2D_DESC cpuTd = td;
                    cpuTd.Usage = D3D11_USAGE_STAGING;
                    cpuTd.BindFlags = 0;
                    cpuTd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    winrt::com_ptr<ID3D11Texture2D> cpuStaging;
                    hr = m_d3d->CreateTexture2D(
                        &cpuTd, nullptr, cpuStaging.put());
                    if (FAILED(hr)) {
                        set_error(hresult_failure(
                            "D3D11 CPU staging texture creation", hr));
                        m_sink = nullptr;
                        return false;
                    }
                    m_cpuStaging.push_back(std::move(cpuStaging));
                }
            }
        }
        m_startStage = "encode-worker startup";
        {
            std::lock_guard<std::mutex> lock(m_encodeMutex);
            m_freeStaging.clear();
            m_encodeQueue.clear();
            m_preroll.clear();
            const int pool = static_cast<int>(m_staging.size());
            for (int i = 0; i < pool; ++i)
                m_freeStaging.push_back(i);
            m_workerFinish = false;
            m_workerError.clear();
        }
        m_encodeWorker = std::thread([this] { encode_worker(); });
        m_startStage = "encoder ready";
        return true;
    }

    void next_sample_timing(std::chrono::steady_clock::time_point captured_at,
                            LONGLONG& timestamp,
                            LONGLONG& duration) noexcept
    {
        std::lock_guard<std::mutex> lock(m_timingMutex);
        timestamp = 0;
        duration = m_frameDur;
        if (m_use_wall_pts) {
            const auto now = captured_at;
            if (!m_have_pts_t0) {
                m_pts_t0 = now;
                m_have_pts_t0 = true;
            }
            auto debt = m_pauseDebt;
            if (m_pauseStartedValid && now > m_pauseStarted)
                debt += now - m_pauseStarted;
            auto span = now - m_pts_t0;
            if (debt < span)
                span -= debt;
            else
                span = {};
            timestamp = std::chrono::duration_cast<std::chrono::duration<LONGLONG, std::ratio<1, 10'000'000>>>(
                span).count();
            if (timestamp < 0) timestamp = 0;
            if (timestamp <= m_last_vid_pts) timestamp = m_last_vid_pts + 1;
            duration = (m_frameIdx == 0) ? m_frameDur
                                         : std::max<LONGLONG>(timestamp - m_last_vid_pts, 1);
            m_last_vid_pts = timestamp;
        } else {
            if (m_frameIdx == 0)
                timestamp = 0;
            else
                timestamp = m_last_vid_pts + m_frameDur;
            m_last_vid_pts = timestamp;
        }
        ++m_frameIdx;
    }

    bool push_rgb32_frame(const D3D11_MAPPED_SUBRESOURCE& map, int w, int h,
                          std::chrono::steady_clock::time_point captured_at) noexcept
    {
        LONGLONG timestamp = 0;
        LONGLONG duration = m_frameDur;
        next_sample_timing(captured_at, timestamp, duration);
        winrt::com_ptr<IMFMediaBuffer> buf;
        const DWORD   nbytes = (DWORD)w * (DWORD)h * 4U;
        if (FAILED(MFCreateMemoryBuffer(nbytes, buf.put()))) return false;
        winrt::com_ptr<IMFSample> s;
        if (FAILED(MFCreateSample(s.put())) || FAILED(s->AddBuffer(buf.get()))) return false;
        s->SetSampleTime(timestamp);
        s->SetSampleDuration(duration);
        BYTE* dst0 = nullptr;
        DWORD mlen = 0, clen = 0;
        if (FAILED(buf->Lock(&dst0, &mlen, &clen))) return false;
        for (int y = 0; y < h; ++y) {
            const auto* srow   = (const std::uint8_t*)map.pData + (size_t)y * (size_t)map.RowPitch;
            auto*       drow   = (std::uint8_t*)dst0 + (size_t)y * (size_t)w * 4U;
            memcpy(drow, srow, (size_t)w * 4U);
        }
        (void)buf->Unlock();
        (void)buf->SetCurrentLength(nbytes);
#if PM_HAS_AUDIO_IO
        std::unique_lock<std::mutex> sinkLock;
        if (m_audio_mux)
            sinkLock = std::unique_lock<std::mutex>(m_audio_mux->sink_write_mutex());
#endif
        const HRESULT writeHr = m_sink->WriteSample(m_outStreamIndex, s.get());
        if (FAILED(writeHr)) {
            if (!m_loggedVidWriteFail.exchange(true)) {
                std::ostringstream o;
                o << "video WriteSample (RGB32) failed hr=0x" << std::hex
                  << std::setfill('0') << std::setw(8)
                  << static_cast<unsigned int>(writeHr)
                  << std::dec << " pts_ms=" << (timestamp / 10000)
                  << " audio=" << (m_audio_enabled ? 1 : 0);
                ui_error_session_video(o.str());
            }
            return false;
        }
        if (!m_loggedFirstVidSample.exchange(true)) {
            ui_warn_session_video(
                "first video WriteSample ok pts_ms=" +
                std::to_string(timestamp / 10000) +
                " audio=" + std::to_string(m_audio_enabled ? 1 : 0));
        }
        return true;
    }

    bool write_h264_packet(const NvencEncodedPacket& packet) noexcept
    {
        if (packet.bytes.empty()) return true;
        winrt::com_ptr<IMFMediaBuffer> buffer;
        if (FAILED(MFCreateMemoryBuffer(
                static_cast<DWORD>(packet.bytes.size()), buffer.put()))) {
            return false;
        }
        BYTE* destination = nullptr;
        DWORD maximum = 0, current = 0;
        if (FAILED(buffer->Lock(&destination, &maximum, &current)))
            return false;
        std::memcpy(destination, packet.bytes.data(), packet.bytes.size());
        (void)buffer->Unlock();
        if (FAILED(buffer->SetCurrentLength(static_cast<DWORD>(packet.bytes.size()))))
            return false;

        winrt::com_ptr<IMFSample> sample;
        if (FAILED(MFCreateSample(sample.put())) ||
            FAILED(sample->AddBuffer(buffer.get()))) {
            return false;
        }
        sample->SetSampleTime(packet.timestamp_hns);
        sample->SetSampleDuration(m_frameDur);
        if (packet.keyframe)
            sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
#if PM_HAS_AUDIO_IO
        std::unique_lock<std::mutex> sinkLock;
        if (m_audio_mux)
            sinkLock = std::unique_lock<std::mutex>(m_audio_mux->sink_write_mutex());
#endif
        const HRESULT writeHr = m_sink->WriteSample(m_outStreamIndex, sample.get());
        if (FAILED(writeHr)) {
            if (!m_loggedVidWriteFail.exchange(true)) {
                std::ostringstream o;
                o << "video WriteSample (NVENC) failed hr=0x" << std::hex
                  << std::setfill('0') << std::setw(8)
                  << static_cast<unsigned int>(writeHr)
                  << std::dec << " pts_ms=" << (packet.timestamp_hns / 10000)
                  << " audio=" << (m_audio_enabled ? 1 : 0);
                ui_error_session_video(o.str());
            }
            return false;
        }
        if (!m_loggedFirstVidSample.exchange(true)) {
            ui_warn_session_video(
                "first video WriteSample (NVENC) ok pts_ms=" +
                std::to_string(packet.timestamp_hns / 10000) +
                " audio=" + std::to_string(m_audio_enabled ? 1 : 0));
        }
        return true;
    }

    void encode_worker() noexcept
    {
        (void)::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;) {
            QueuedFrame queued;
            {
                std::unique_lock<std::mutex> lock(m_encodeMutex);
                m_encodeReady.wait(lock, [this] {
                    return !m_encodeQueue.empty() || m_workerFinish;
                });
                if (m_encodeQueue.empty())
                    break;
                queued = m_encodeQueue.front();
                m_encodeQueue.pop_front();
            }

            auto& staging = m_staging[static_cast<size_t>(queued.staging_index)];
            bool ok = false;
#if PM_HAS_AUDIO_IO
            if (m_audio_mux && !m_audio_mux->video_sync_armed()) {
                std::lock_guard<std::mutex> audioLock(m_audio_mux->sink_write_mutex());
                m_audio_mux->arm_video_sync(queued.captured_at);
            }
#endif
            if (m_nvenc) {
                LONGLONG timestamp = 0;
                LONGLONG duration = m_frameDur;
                next_sample_timing(queued.captured_at, timestamp, duration);
                std::vector<NvencEncodedPacket> packets;
                std::string encodeError;
                ok = m_nvenc->encode(
                    staging.get(), 0, 0, timestamp, packets, encodeError);
                if (ok) {
                    for (const auto& packet : packets) {
                        if (!write_h264_packet(packet)) {
                            ok = false;
                            break;
                        }
                    }
                } else {
                    std::lock_guard<std::mutex> lock(m_encodeMutex);
                    if (m_workerError.empty()) m_workerError = std::move(encodeError);
                }
            } else {
                ID3D11Texture2D* readable = staging.get();
                if (m_transformEnabled) {
                    auto& cpu = m_cpuStaging[
                        static_cast<size_t>(queued.staging_index)];
                    m_ctx->CopyResource(cpu.get(), staging.get());
                    readable = cpu.get();
                }
                D3D11_MAPPED_SUBRESOURCE map{};
                if (SUCCEEDED(m_ctx->Map(
                        readable, 0, D3D11_MAP_READ, 0, &map))) {
                    ok = push_rgb32_frame(
                        map, m_encW, m_encH, queued.captured_at);
                    m_ctx->Unmap(readable, 0);
                }
            }

            {
                std::lock_guard<std::mutex> lock(m_encodeMutex);
                m_freeStaging.push_back(queued.staging_index);
                if (!ok && m_workerError.empty()) {
                    m_workerError = "asynchronous Map/WriteSample failed";
                    m_workerFinish = true;
                }
            }
            if (ok)
                m_framesWritten.fetch_add(1, std::memory_order_relaxed);
            else {
                std::string workerError;
                {
                    std::lock_guard<std::mutex> lock(m_encodeMutex);
                    workerError = m_workerError;
                }
                const std::string msg = workerError.empty()
                    ? "asynchronous encoder failure" : std::move(workerError);
                set_error(msg);
                ui_error_session_video(msg);
            }
            if (!ok)
                break;
        }
        ::CoUninitialize();
    }

    void stop_encode_worker() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(m_encodeMutex);
            m_workerFinish = true;
        }
        m_encodeReady.notify_all();
        if (m_encodeWorker.joinable())
            m_encodeWorker.join();
        if (m_nvenc) {
            std::vector<NvencEncodedPacket> packets;
            std::string flushError;
            if (m_nvenc->flush(packets, flushError)) {
                for (const auto& packet : packets) {
                    if (!write_h264_packet(packet)) {
                        flushError = "writing flushed NVENC packet failed";
                        break;
                    }
                }
            }
            if (!flushError.empty())
                ui_log_session_video("NVENC flush: " + flushError);
            if (!flushError.empty())
                set_error_if_empty("NVENC flush failed: " + flushError);
        }
        std::string workerError;
        {
            std::lock_guard<std::mutex> lock(m_encodeMutex);
            workerError = m_workerError;
        }
        if (!workerError.empty())
            ui_log_session_video("encoder worker: " + workerError);
        if (!workerError.empty())
            set_error_if_empty("encoder worker failed: " + workerError);
    }

    std::atomic_bool          m_stopping{ false };
    std::atomic_bool          m_paused{ false };
    std::atomic<std::uint32_t> m_pauseReasons{ pm::video::PauseNone };
    pm::video::CaptureIdleDetector m_idleDetector;
    std::atomic_bool          m_idleHooksLive{ false };
    std::chrono::steady_clock::time_point m_nextIdleProbe{};
    bool                      m_haveIdleProbe{ false };
    bool                      m_idleProbeFailed{ false };
    winrt::com_ptr<ID3D11Texture2D> m_idleGpu;
    winrt::com_ptr<ID3D11Texture2D> m_idleCpu;
    winrt::com_ptr<ID3D11VideoDevice> m_idleVideoDevice;
    winrt::com_ptr<ID3D11VideoContext> m_idleVideoContext;
    winrt::com_ptr<ID3D11VideoProcessorEnumerator> m_idleEnumerator;
    winrt::com_ptr<ID3D11VideoProcessor> m_idleProcessor;
    winrt::com_ptr<ID3D11VideoProcessorOutputView> m_idleOutputView;
    int                       m_idleSourceW{ 0 };
    int                       m_idleSourceH{ 0 };
    std::uint64_t             m_idleVpFrame{ 0 };
    std::filesystem::path     m_captionDumpPath;
    std::atomic_bool          m_recordingStarted{ false };
    std::atomic<std::uint64_t> m_framesWritten{ 0 };
    int                       m_encW{ 0 };
    int                       m_encH{ 0 };
    int                       m_poolW{ 0 };
    int                       m_poolH{ 0 };
    int                       m_fps{ 30 };
    int                       m_bitrateKbps{ 0 };
    int                       m_effectiveBitrateKbps{ 0 };
    std::string               m_encoderRequest{"auto"};
    LONGLONG                  m_frameIdx{ 0 };
    LONGLONG                  m_frameDur{ 10'000'000LL / 30LL };
    bool                      m_loggedFirstFrame{ false };
    bool                      m_loggedEvenCrop{ false };
    std::atomic_bool          m_loggedFirstVidSample{ false };
    std::atomic_bool          m_loggedVidWriteFail{ false };
    bool                      m_loggedSizeChange{ false };
    int                       m_logFrameIndexMod30{ 0 };
    bool                      m_hasCrop{ false };
    int                       m_cropL{ 0 };
    int                       m_cropT{ 0 };
    int                       m_cropW{ 0 };
    int                       m_cropH{ 0 };
    bool                      m_haveAcceptedFrame{ false };
    std::chrono::steady_clock::time_point m_nextAcceptedFrame{};
    std::atomic<std::uint64_t> m_droppedFrames{ 0 };
    bool                      m_use_wall_pts{ false };
    std::mutex                m_timingMutex;
    std::chrono::steady_clock::time_point m_pts_t0{};
    bool                      m_have_pts_t0{ false };
    LONGLONG                  m_last_vid_pts{ 0 };
    std::chrono::steady_clock::time_point m_pauseStarted{};
    bool                      m_pauseStartedValid{ false };
    std::chrono::steady_clock::duration m_pauseDebt{};
#if PM_HAS_AUDIO_IO
    bool                      m_audio_enabled{ false };
    SessionVideoAudioConfig   m_audio_cfg{};
    std::unique_ptr<pm::video::MfVideoAudioMux> m_audio_mux;
    DWORD                     m_audio_stream{ 0 };
#endif
    mutable std::mutex        m_statusMutex;
    std::string               m_lastErr;
    std::string               m_softWarning;
    std::string               m_effectiveEncoder;
    std::filesystem::path     m_outPath;
    pm::video::WinCaptureTarget m_target{};
    VideoTransformConfig      m_transform{};
    bool                      m_transformEnabled{false};
    int                       m_baseL{0};
    int                       m_baseT{0};
    int                       m_baseW{0};
    int                       m_baseH{0};
    int                       m_sourceContentW{0};
    int                       m_sourceContentH{0};
    int                       m_contentOriginL{0}; // visible content origin in WGC texture
    int                       m_contentOriginT{0};
    mutable std::mutex        m_transformMutex;
    double                    m_zoomStart{1.0};
    double                    m_zoomTarget{1.0};
    double                    m_currentZoom{1.0};
    std::chrono::steady_clock::time_point m_zoomTransitionStart{};
    bool                      m_centerInitialized{false};
    double                    m_centerX{0.0};
    double                    m_centerY{0.0};
    RECT                      m_lastViewport{};
    unsigned int              m_pauseModifiers{0};
    unsigned int              m_pauseVirtualKey{0};
    unsigned int              m_stopModifiers{0};
    unsigned int              m_stopVirtualKey{0};
    std::atomic_bool          m_userStopRequested{false};
    std::atomic<std::shared_ptr<pm::video::WinInteractiveZoomController>>
        m_interactiveControls;
    std::unique_ptr<media::win::PickerHudCaptureScope> m_pickerHudCapture;
    std::atomic<std::shared_ptr<pm::video::WinCameraOverlayController>>
        m_cameraOverlay;
    RECT m_cameraAnchor{};
    bool m_cameraAnchorValid = false;

    winrt::com_ptr<ID3D11Device>  m_d3d;
    winrt::com_ptr<ID3D11DeviceContext> m_ctx;
    winrt::com_ptr<ID3D11VideoDevice> m_videoDevice;
    winrt::com_ptr<ID3D11VideoContext> m_videoContext;
    winrt::com_ptr<ID3D11VideoProcessorEnumerator> m_videoEnumerator;
    winrt::com_ptr<ID3D11VideoProcessor> m_videoProcessor;
    std::vector<winrt::com_ptr<ID3D11VideoProcessorOutputView>>
        m_videoOutputViews;
    std::uint64_t               m_videoFrameIndex{0};
    int                         m_transformSourceW{0};
    int                         m_transformSourceH{0};
    std::unique_ptr<NvencPlugin>  m_nvenc;
    struct QueuedFrame {
        int staging_index = -1;
        std::chrono::steady_clock::time_point captured_at{};
    };
    std::vector<winrt::com_ptr<ID3D11Texture2D>> m_staging;
    std::vector<winrt::com_ptr<ID3D11Texture2D>> m_cpuStaging;
    std::thread                  m_encodeWorker;
    std::mutex                   m_encodeMutex;
    std::condition_variable      m_encodeReady;
    std::deque<int>              m_freeStaging;
    std::deque<QueuedFrame>      m_encodeQueue;
    std::deque<QueuedFrame>      m_preroll;
    bool                         m_workerFinish{ false };
    std::string                  m_workerError;
    IWinRtD3d11Device  m_wrDevice{ nullptr };
    DxpPixelFormat  m_fmt{};
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem     m_item{ nullptr };
    WgcFramePool  m_pool{ nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession  m_session{ nullptr };
    winrt::com_ptr<IMFSinkWriter> m_sink;
    DWORD m_outStreamIndex{ 0 };
    const char* m_startStage{ "session setup" };
};

void finalize_failed_start(const std::shared_ptr<RecSession>& session,
                           const std::string& error) noexcept
{
    {
        std::lock_guard<std::mutex> lock(g_mux);
        if (g_sess == session) g_sess.reset();
    }
    session->stop_and_finalize();
    auto failed = session->status();
    failed.state = pm::video::RecordingState::failed;
    if (failed.error.empty())
        failed.error = error;
    else if (failed.error != error)
        failed.error = error + " (cleanup also reported: " + failed.error + ")";
    {
        std::lock_guard<std::mutex> lock(g_mux);
        g_lastStatus = std::move(failed);
    }
    mf_release();
}

bool start_impl(const pm::video::WinCaptureTarget& target,
                const std::filesystem::path& out_mp4,
                std::string& err_utf8,
                bool include_cursor,
                const CaptureCrop* crop,
                int fps,
                int bitrate_kbps,
                const std::string& encoder,
                const VideoTransformConfig* transform
#if PM_HAS_AUDIO_IO
                , const SessionVideoAudioConfig* audio
#endif
                )
{
    if (!target.valid()) {
        err_utf8 = "session video: no window or monitor capture target";
        ui_log_session_video(err_utf8);
        set_global_start_failure(err_utf8);
        return false;
    }
    err_utf8.clear();
    try {
        ensure_com_st_and_winrt();
    } catch (const winrt::hresult_error& e) {
        const auto c = static_cast<unsigned int>(e.code() & 0xFFFFFFFFU);
        std::ostringstream o;
        o << "session video: winrt::init_apartment failed, hresult=0x" << std::hex << std::setfill('0') << std::setw(8) << c
          << std::dec << hresult_hint_un(c);
        err_utf8 = o.str();
        ui_log_session_video(err_utf8);
        set_global_start_failure(err_utf8);
        return false;
    }
    {
        std::lock_guard<std::mutex> l(g_mux);
        if (g_sess) {
            err_utf8 = "session video: already recording";
            ui_log_session_video("start rejected: already recording");
            return false;
        }
        g_lastStatus = {};
        g_lastStatus.state = pm::video::RecordingState::starting;
        g_lastStatus.capture_backend = "fast-wgc";
    }
    ui_log_session_video("Media Foundation: MFStartup (ref)");
    mf_retain();

    const std::shared_ptr<RecSession> sess = RecSession::make();
    {
        std::lock_guard<std::mutex> l(g_mux);
        g_sess = sess;
    }
    try {
        sess->start_capture(target, out_mp4, include_cursor, crop, fps,
                            bitrate_kbps, encoder, transform,
#if PM_HAS_AUDIO_IO
                            audio,
#endif
                            err_utf8);
    } catch (const winrt::hresult_error& e) {
        const auto    c  = static_cast<unsigned int>(e.code() & 0xFFFFFFFFU);
        std::ostringstream o;
        o << "session video startup failed at " << sess->start_stage()
          << ": hresult=0x" << std::hex << std::setfill('0')
          << std::setw(8) << c << std::dec << hresult_hint_un(c);
        err_utf8 = o.str();
        ui_log_session_video(err_utf8);
        finalize_failed_start(sess, err_utf8);
        return false;
    } catch (const std::exception& ex) {
        err_utf8 = "session video startup failed at ";
        err_utf8 += sess->start_stage();
        err_utf8 += ": ";
        err_utf8 += ex.what() ? ex.what() : "std::exception";
        ui_log_session_video(std::string("exception: ") + err_utf8);
        finalize_failed_start(sess, err_utf8);
        return false;
    } catch (...) {
        err_utf8   = "session video: unknown exception";
        ui_log_session_video(err_utf8);
        finalize_failed_start(sess, err_utf8);
        return false;
    }
    const auto startedStatus = sess->status();
    ui_log_session_video(
        "session video start complete (WGC + " +
        (startedStatus.effective_encoder.empty()
            ? std::string("unknown encoder")
            : startedStatus.effective_encoder) +
        "; stop with app command or hotkey)");
    return true;
}

void stop_impl() noexcept
{
    ui_log_session_video("stop: closing WGC + finalizing MP4, MF release");
    std::shared_ptr<RecSession> s;
    { std::lock_guard<std::mutex> l(g_mux); s = std::move(g_sess); }
    if (s) {
        s->stop_and_finalize();
        const auto stopped = s->status();
        ui_log_session_video(
            "stop summary: state=" +
            std::string(pm::video::recording_state_name(stopped.state)) +
            " encoder=" + stopped.effective_encoder +
            " size=" + std::to_string(stopped.width) + "x" +
            std::to_string(stopped.height) +
            " bitrate_kbps=" + std::to_string(stopped.bitrate_kbps) +
            " frames=" + std::to_string(stopped.frames_written) +
            " dropped=" + std::to_string(stopped.frames_dropped) +
            (stopped.error.empty()
                ? std::string{} : " error='" + stopped.error + "'"));
        std::lock_guard<std::mutex> l(g_mux);
        g_lastStatus = stopped;
        mf_release();
    }
    ui_log_session_video("stop: done (MF ref released)");
}

bool is_recording_impl() noexcept
{
    std::lock_guard<std::mutex> l(g_mux);
    return (bool)g_sess && g_sess->active();
}

pm::video::RecordingStatus status_impl() noexcept
{
    std::shared_ptr<RecSession> s;
    {
        std::lock_guard<std::mutex> l(g_mux);
        s = g_sess;
        if (!s) return g_lastStatus;
    }
    return s->status();
}

bool update_impl(
    const VideoTransformUpdate& update, std::string& error) noexcept
{
    std::shared_ptr<RecSession> session;
    {
        std::lock_guard<std::mutex> lock(g_mux);
        session = g_sess;
    }
    if (!session || !session->active()) {
        error = "session video: not recording";
        return false;
    }
    return session->update_transform(update, error);
}

void toggle_pause_impl() noexcept
{
    std::shared_ptr<RecSession> s;
    { std::lock_guard<std::mutex> l(g_mux); s = g_sess; }
    if (!s || !s->active()) {
        ui_log_session_video("pause toggle ignored: not recording (Ctrl+Alt+R to start)");
        return;
    }
    s->toggle_pause();
}

bool is_paused_impl() noexcept
{
    std::lock_guard<std::mutex> l(g_mux);
    return (bool)g_sess && g_sess->active() && g_sess->paused();
}
} // namespace detail

bool start(const pm::video::WinCaptureTarget& target,
           const std::filesystem::path& p, std::string& e, bool include_cursor,
           const CaptureCrop* crop, int fps, int bitrate_kbps,
           const std::string& encoder,
           const VideoTransformConfig* transform
#if PM_HAS_AUDIO_IO
           , const SessionVideoAudioConfig* audio
#endif
           )
{
#if PM_HAS_AUDIO_IO
    return detail::start_impl(
        target, p, e, include_cursor, crop, fps, bitrate_kbps, encoder,
        transform, audio);
#else
    return detail::start_impl(
        target, p, e, include_cursor, crop, fps, bitrate_kbps, encoder,
        transform);
#endif
}
bool start(HWND h, const std::filesystem::path& p, std::string& e, bool include_cursor,
           const CaptureCrop* crop, int fps, int bitrate_kbps,
           const std::string& encoder,
           const VideoTransformConfig* transform
#if PM_HAS_AUDIO_IO
           , const SessionVideoAudioConfig* audio
#endif
           )
{
    pm::video::WinCaptureTarget target;
    target.window = h;
#if PM_HAS_AUDIO_IO
    return start(target, p, e, include_cursor, crop, fps, bitrate_kbps,
                 encoder, transform, audio);
#else
    return start(target, p, e, include_cursor, crop, fps, bitrate_kbps,
                 encoder, transform);
#endif
}
void   stop() noexcept { detail::stop_impl(); }
bool   is_recording() noexcept { return detail::is_recording_impl(); }
pm::video::RecordingStatus status() noexcept { return detail::status_impl(); }
bool update(
    const VideoTransformUpdate& value, std::string& error) noexcept
{
    return detail::update_impl(value, error);
}
void   toggle_pause() noexcept { detail::toggle_pause_impl(); }
bool   is_paused() noexcept { return detail::is_paused_impl(); }
} // namespace media::win::recorder

#endif
