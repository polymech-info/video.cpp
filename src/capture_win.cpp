#if defined(FEATURE_VIDEO) && FEATURE_VIDEO

// ── Win32 Media Foundation + WIC video capture backend ────────────────────────
// Requires:  mf.lib  mfplat.lib  mfreadwrite.lib  mfuuid.lib
//            wincodec.lib  shlwapi.lib  ole32.lib

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "pm/video/capture.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

// ── Helpers ────────────────────────────────────────────────────────────────────

static std::string wide_to_utf8(const wchar_t* w) {
    if (!w) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    return s;
}

static std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    return w;
}

// Case-insensitive substring search on wide strings
static bool wide_icontains(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) return true;
    auto it = std::search(haystack.begin(), haystack.end(),
                          needle.begin(),  needle.end(),
                          [](wchar_t a, wchar_t b) {
                              return towupper(a) == towupper(b);
                          });
    return it != haystack.end();
}

static bool wide_iequals(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (towupper(a[i]) != towupper(b[i])) return false;
    return true;
}

static std::wstring mf_allocated_string(IMFActivate* act, const GUID& key) {
    if (!act) return {};
    WCHAR* p = nullptr;
    UINT32 n = 0;
    if (FAILED(act->GetAllocatedString(key, &p, &n)) || !p)
        return {};
    std::wstring w(p);
    CoTaskMemFree(p);
    return w;
}

struct MfDevicePick {
    int         index = -1;
    std::string name;
};

// Empty needle selects the first device. Otherwise an exact symbolic-link match
// wins, then the first friendly-name substring (same rule as before).
static MfDevicePick pick_mf_capture_device(IMFActivate** devices, UINT32 count,
                                           const std::wstring& needle) {
    MfDevicePick out;
    if (!devices || count == 0)
        return out;
    if (needle.empty()) {
        out.index = 0;
        const auto fn = mf_allocated_string(devices[0], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
        out.name = wide_to_utf8(fn.c_str());
        return out;
    }
    int name_hit = -1;
    std::string name_label;
    for (UINT32 i = 0; i < count; ++i) {
        const auto sym = mf_allocated_string(
            devices[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
        const auto fn = mf_allocated_string(devices[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
        if (!sym.empty() && wide_iequals(sym, needle)) {
            out.index = static_cast<int>(i);
            out.name = wide_to_utf8(fn.c_str());
            return out;
        }
        if (name_hit < 0 && wide_icontains(fn, needle)) {
            name_hit = static_cast<int>(i);
            name_label = wide_to_utf8(fn.c_str());
        }
    }
    out.index = name_hit;
    out.name = std::move(name_label);
    return out;
}

// Ensure MF is initialized in the calling thread context.
// Call once per thread that will use MF.
static HRESULT ensure_mf() {
    static std::once_flag mf_once;
    static HRESULT        mf_hr = S_OK;
    std::call_once(mf_once, [] {
        mf_hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    });
    return mf_hr;
}

// ── Internal helpers ──────────────────────────────────────────────────────────

// Convert a MF video subtype GUID to a human-readable format name.
static std::string subtype_name(const GUID& g) {
    if (IsEqualGUID(g, MFVideoFormat_MJPG))  return "MJPEG";
    if (IsEqualGUID(g, MFVideoFormat_NV12))  return "NV12";
    if (IsEqualGUID(g, MFVideoFormat_YUY2))  return "YUY2";
    if (IsEqualGUID(g, MFVideoFormat_UYVY))  return "UYVY";
    if (IsEqualGUID(g, MFVideoFormat_RGB24)) return "RGB24";
    if (IsEqualGUID(g, MFVideoFormat_RGB32)) return "RGB32";
    if (IsEqualGUID(g, MFVideoFormat_ARGB32))return "ARGB32";
    if (IsEqualGUID(g, MFVideoFormat_I420))  return "I420";
    if (IsEqualGUID(g, MFVideoFormat_YV12))  return "YV12";
    if (IsEqualGUID(g, MFVideoFormat_H264))  return "H264";
    if (IsEqualGUID(g, MFVideoFormat_HEVC))  return "HEVC";
    // Fallback: first 4 chars of GUID Data1 as FourCC.
    char buf[5];
    buf[0] = static_cast<char>( g.Data1        & 0xFF);
    buf[1] = static_cast<char>((g.Data1 >>  8) & 0xFF);
    buf[2] = static_cast<char>((g.Data1 >> 16) & 0xFF);
    buf[3] = static_cast<char>((g.Data1 >> 24) & 0xFF);
    buf[4] = '\0';
    for (int i = 0; i < 4; ++i) if (buf[i] < 0x20 || buf[i] > 0x7E) buf[i] = '?';
    return std::string(buf);
}

// Open a source reader for a specific IMFActivate (without starting capture).
// Returns null on failure.
static ComPtr<IMFSourceReader> open_reader_for_modes(IMFActivate* pActivate) {
    ComPtr<IMFMediaSource> pSource;
    if (FAILED(pActivate->ActivateObject(IID_PPV_ARGS(&pSource)))) return nullptr;

    ComPtr<IMFAttributes> pAttr;
    MFCreateAttributes(&pAttr, 1);
    // No ENABLE_VIDEO_PROCESSING here — we want native types only.

    ComPtr<IMFSourceReader> pReader;
    if (FAILED(MFCreateSourceReaderFromMediaSource(pSource.Get(), pAttr.Get(), &pReader)))
        return nullptr;
    return pReader;
}

// Enumerate all native DeviceMode entries from an already-open source reader.
static std::vector<pm::video::DeviceMode> read_modes(IMFSourceReader* pReader) {
    std::vector<pm::video::DeviceMode> modes;
    for (DWORD ti = 0; ; ++ti) {
        ComPtr<IMFMediaType> pType;
        HRESULT hr = pReader->GetNativeMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, ti, &pType);
        if (hr == MF_E_NO_MORE_TYPES || FAILED(hr)) break;

        GUID major{}, sub{};
        pType->GetGUID(MF_MT_MAJOR_TYPE, &major);
        if (!IsEqualGUID(major, MFMediaType_Video)) continue;
        pType->GetGUID(MF_MT_SUBTYPE, &sub);

        UINT32 w = 0, h = 0;
        MFGetAttributeSize(pType.Get(), MF_MT_FRAME_SIZE, &w, &h);
        if (w == 0 || h == 0) continue;

        UINT32 fpsN = 0, fpsD = 1;
        MFGetAttributeRatio(pType.Get(), MF_MT_FRAME_RATE, &fpsN, &fpsD);
        if (fpsD == 0) fpsD = 1;

        pm::video::DeviceMode m;
        m.width   = static_cast<int>(w);
        m.height  = static_cast<int>(h);
        m.fps_num = static_cast<int>(fpsN);
        m.fps_den = static_cast<int>(fpsD);
        m.format  = subtype_name(sub);
        modes.push_back(std::move(m));
    }
    return modes;
}

namespace pm::video {

// ── enumerate_capture_devices ─────────────────────────────────────────────────

std::vector<DeviceInfo> enumerate_capture_devices() {
    std::vector<DeviceInfo> result;

    if (FAILED(ensure_mf())) return result;

    ComPtr<IMFAttributes> pConfig;
    if (FAILED(MFCreateAttributes(&pConfig, 1))) return result;
    pConfig->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                     MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** ppDevices = nullptr;
    UINT32        count     = 0;
    if (FAILED(MFEnumDeviceSources(pConfig.Get(), &ppDevices, &count))) return result;

    for (UINT32 i = 0; i < count; ++i) {
        DeviceInfo di;
        di.is_default = (i == 0);

        WCHAR* pName = nullptr;
        UINT32  nameLen = 0;
        if (SUCCEEDED(ppDevices[i]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &pName, &nameLen))) {
            di.name = wide_to_utf8(pName);
            CoTaskMemFree(pName);
        }

        WCHAR* pSym = nullptr;
        UINT32  symLen = 0;
        if (SUCCEEDED(ppDevices[i]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                &pSym, &symLen))) {
            di.id = wide_to_utf8(pSym);
            CoTaskMemFree(pSym);
        }

        result.push_back(std::move(di));
        ppDevices[i]->Release();
    }
    CoTaskMemFree(ppDevices);
    return result;
}

// ── enumerate_device_modes ────────────────────────────────────────────────────

std::vector<DeviceMode> enumerate_device_modes(const std::string& device_name) {
    if (FAILED(ensure_mf())) return {};

    ComPtr<IMFAttributes> pConfig;
    MFCreateAttributes(&pConfig, 1);
    pConfig->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                     MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** ppDevices = nullptr;
    UINT32        count     = 0;
    if (FAILED(MFEnumDeviceSources(pConfig.Get(), &ppDevices, &count)) || count == 0)
        return {};

    const auto picked = pick_mf_capture_device(ppDevices, count, utf8_to_wide(device_name));
    const int chosen = picked.index;

    std::vector<DeviceMode> result;
    if (chosen >= 0) {
        auto pReader = open_reader_for_modes(ppDevices[chosen]);
        if (pReader) result = read_modes(pReader.Get());
    }

    for (UINT32 i = 0; i < count; ++i) ppDevices[i]->Release();
    CoTaskMemFree(ppDevices);
    return result;
}

// ── VideoInput::Impl ───────────────────────────────────────────────────────────

struct VideoInput::Impl {
    ComPtr<IMFSourceReader>             reader;
    std::thread                         capture_thread;
    std::atomic<bool>                   running{false};
    std::string                         device_name;
    std::atomic<int>                    actual_w{0};
    std::atomic<int>                    actual_h{0};
    std::function<void(const VideoFrame&)> on_frame;

    // Called from capture_thread after negotiation succeeds.
    void read_loop() {
        // Each thread needs COM initialized.
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        while (running.load(std::memory_order_relaxed)) {
            DWORD     streamIndex = 0, flags = 0;
            LONGLONG  timestamp   = 0;
            ComPtr<IMFSample> pSample;

            HRESULT hr = reader->ReadSample(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                0, &streamIndex, &flags, &timestamp, &pSample);

            if (FAILED(hr) || !running.load()) break;
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
            if (flags & MF_SOURCE_READERF_STREAMTICK)  continue;
            if (!pSample) continue;

            // Try 2D buffer path first (handles bottom-up stride correctly).
            ComPtr<IMFMediaBuffer> pBuffer;
            if (FAILED(pSample->ConvertToContiguousBuffer(&pBuffer))) continue;

            ComPtr<IMF2DBuffer> p2D;
            VideoFrame frame;

            int w = actual_w.load();
            int h = actual_h.load();

            if (SUCCEEDED(pBuffer.As(&p2D))) {
                BYTE*  pScan0 = nullptr;
                LONG   pitch  = 0;
                if (FAILED(p2D->Lock2D(&pScan0, &pitch))) continue;

                // BGR24: pitch can be negative (bottom-up).
                int abs_pitch = pitch < 0 ? -pitch : pitch;
                // Source is RGB32 (BGRX, 4 bytes/pixel, may be bottom-up).
                // Convert to packed BGR24 (3 bytes/pixel, top-down).
                frame.width        = w;
                frame.height       = h;
                frame.stride       = w * 3;
                frame.format       = PixelFormat::BGR24;
                frame.timestamp_ms = static_cast<int64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                frame.data.resize(static_cast<size_t>(w * h * 3));

                for (int row = 0; row < h; ++row) {
                    // Negative pitch → bottom-up: row 0 is at the highest address.
                    const BYTE* src_row = (pitch >= 0)
                        ? (pScan0 + static_cast<ptrdiff_t>(row) * pitch)
                        : (pScan0 + static_cast<ptrdiff_t>(h - 1 - row) * abs_pitch);
                    uint8_t* dst_row = frame.data.data() + row * frame.stride;
                    for (int x = 0; x < w; ++x) {
                        dst_row[x * 3 + 0] = src_row[x * 4 + 0]; // B
                        dst_row[x * 3 + 1] = src_row[x * 4 + 1]; // G
                        dst_row[x * 3 + 2] = src_row[x * 4 + 2]; // R
                    }
                }
                p2D->Unlock2D();
            } else {
                // Flat buffer fallback — convert BGRX→BGR24 inline.
                BYTE*  pData  = nullptr;
                DWORD  curLen = 0;
                if (FAILED(pBuffer->Lock(&pData, nullptr, &curLen))) continue;

                frame.width        = w;
                frame.height       = h;
                frame.stride       = w * 3;
                frame.format       = PixelFormat::BGR24;
                frame.timestamp_ms = static_cast<int64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                const int pixel_count = w * h;
                frame.data.resize(static_cast<size_t>(pixel_count * 3));
                for (int i = 0; i < pixel_count; ++i) {
                    frame.data[i * 3 + 0] = pData[i * 4 + 0]; // B
                    frame.data[i * 3 + 1] = pData[i * 4 + 1]; // G
                    frame.data[i * 3 + 2] = pData[i * 4 + 2]; // R
                }
                pBuffer->Unlock();
            }

            if (on_frame && !frame.data.empty()) {
                on_frame(frame);
            }
        }
        CoUninitialize();
    }
};

// ── VideoInput public API ─────────────────────────────────────────────────────

VideoInput::VideoInput() : m_(std::make_unique<Impl>()) {}
VideoInput::~VideoInput() { stop(); }

void VideoInput::start(const std::string&                      device_name,
                       int                                     width,
                       int                                     height,
                       int                                     fps,
                       std::function<void(const VideoFrame&)>  on_frame) {
    if (m_->running.load()) stop();

    if (FAILED(ensure_mf()))
        throw std::runtime_error("video: MFStartup failed");

    // ── Find device ──────────────────────────────────────────────────────────
    ComPtr<IMFAttributes> pConfig;
    MFCreateAttributes(&pConfig, 1);
    pConfig->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                     MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** ppDevices = nullptr;
    UINT32        count     = 0;
    if (FAILED(MFEnumDeviceSources(pConfig.Get(), &ppDevices, &count)) || count == 0)
        throw std::runtime_error("video: no capture devices found");

    const auto picked = pick_mf_capture_device(ppDevices, count, utf8_to_wide(device_name));
    const int chosen = picked.index;
    const std::string chosen_name = picked.name;
    if (chosen < 0) {
        for (UINT32 i = 0; i < count; ++i) ppDevices[i]->Release();
        CoTaskMemFree(ppDevices);
        throw std::runtime_error("video: device not found: " + device_name);
    }

    // ── Activate media source ────────────────────────────────────────────────
    ComPtr<IMFMediaSource> pSource;
    HRESULT hr = ppDevices[chosen]->ActivateObject(IID_PPV_ARGS(&pSource));
    for (UINT32 i = 0; i < count; ++i) ppDevices[i]->Release();
    CoTaskMemFree(ppDevices);

    if (FAILED(hr))
        throw std::runtime_error("video: ActivateObject failed (hr=" +
                                 std::to_string(hr) + ")");

    // ── Create source reader ─────────────────────────────────────────────────
    ComPtr<IMFAttributes> pReaderAttr;
    MFCreateAttributes(&pReaderAttr, 2);
    // Let MF insert the video processor MFT so we can request any pixel
    // format regardless of what the device natively delivers (e.g. MJPEG → RGB24).
    pReaderAttr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

    ComPtr<IMFSourceReader> pReader;
    hr = MFCreateSourceReaderFromMediaSource(pSource.Get(), pReaderAttr.Get(), &pReader);
    if (FAILED(hr))
        throw std::runtime_error("video: MFCreateSourceReaderFromMediaSource failed");

    // ── Pick best native media type for the requested resolution/fps ─────────
    // Collect all native types, score them, and pre-select the best one.
    // The video processor (ENABLE_VIDEO_PROCESSING) will then convert it to RGB32.
    {
        // Collect native types.
        struct NativeEntry {
            DWORD              index;
            int                width, height;
            int                fps_num, fps_den; // rational
            ComPtr<IMFMediaType> type;
        };
        std::vector<NativeEntry> natives;
        for (DWORD ti = 0; ; ++ti) {
            ComPtr<IMFMediaType> pNative;
            if (FAILED(pReader->GetNativeMediaType(
                    MF_SOURCE_READER_FIRST_VIDEO_STREAM, ti, &pNative)))
                break;
            GUID major{}; pNative->GetGUID(MF_MT_MAJOR_TYPE, &major);
            if (!IsEqualGUID(major, MFMediaType_Video)) continue;
            UINT32 nw = 0, nh = 0;
            MFGetAttributeSize(pNative.Get(), MF_MT_FRAME_SIZE, &nw, &nh);
            if (nw == 0 || nh == 0) continue;
            UINT32 fn = 0, fd = 1;
            MFGetAttributeRatio(pNative.Get(), MF_MT_FRAME_RATE, &fn, &fd);
            if (fd == 0) fd = 1;
            natives.push_back({ ti, static_cast<int>(nw), static_cast<int>(nh),
                                 static_cast<int>(fn), static_cast<int>(fd),
                                 pNative });
        }

        if (!natives.empty()) {
            // Score each mode: lower is better.
            //  - Resolution penalty: area distance to requested (0 if no preference).
            //  - FPS penalty: |actual_fps - requested_fps| (0 if fps == 0).
            //  - Tiebreak: higher resolution preferred over lower.
            auto score = [&](const NativeEntry& e) -> double {
                double res_score = 0.0;
                if (width > 0 && height > 0) {
                    double dw = static_cast<double>(e.width  - width);
                    double dh = static_cast<double>(e.height - height);
                    res_score = dw * dw + dh * dh;
                }
                double fps_score = 0.0;
                if (fps > 0 && e.fps_den > 0) {
                    double actual = static_cast<double>(e.fps_num) / e.fps_den;
                    double diff   = actual - static_cast<double>(fps);
                    fps_score = diff * diff * 0.01; // weight fps lower than resolution
                }
                // Tiebreak: prefer larger area (subtract it so larger → lower score).
                double area_bonus = -static_cast<double>(e.width * e.height) * 1e-10;
                return res_score + fps_score + area_bonus;
            };

            auto best = std::min_element(natives.begin(), natives.end(),
                [&](const NativeEntry& a, const NativeEntry& b) {
                    return score(a) < score(b);
                });

            // Pre-select the best native type (sets the source's capture format).
            pReader->SetCurrentMediaType(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, best->type.Get());
        }

        // Request RGB32 for output — the MF video processor converts any native
        // format (MJPEG, NV12, YUY2 …) to RGB32 when ENABLE_VIDEO_PROCESSING is set.
        // We strip the padding byte in the read loop to produce BGR24 frames.
        ComPtr<IMFMediaType> pOutputType;
        MFCreateMediaType(&pOutputType);
        pOutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        pOutputType->SetGUID(MF_MT_SUBTYPE,    MFVideoFormat_RGB32);
        hr = pReader->SetCurrentMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pOutputType.Get());
        if (FAILED(hr))
            throw std::runtime_error("video: cannot set RGB32 output type (hr=" +
                                     std::to_string(hr) + ")");
    }

    // ── Read back actual negotiated resolution ───────────────────────────────
    {
        ComPtr<IMFMediaType> pCurrent;
        pReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pCurrent);
        UINT32 aw = 0, ah = 0;
        MFGetAttributeSize(pCurrent.Get(), MF_MT_FRAME_SIZE, &aw, &ah);
        m_->actual_w.store(static_cast<int>(aw));
        m_->actual_h.store(static_cast<int>(ah));
    }

    m_->reader      = pReader;
    m_->device_name = chosen_name;
    m_->on_frame    = std::move(on_frame);
    m_->running.store(true);
    m_->capture_thread = std::thread([this] { m_->read_loop(); });
}

void VideoInput::stop() {
    if (!m_->running.exchange(false)) return;
    // Unblock ReadSample by flushing.
    if (m_->reader) m_->reader->Flush(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    if (m_->capture_thread.joinable()) m_->capture_thread.join();
    m_->reader.Reset();
}

bool VideoInput::is_running() const { return m_->running.load(); }

const std::string& VideoInput::opened_device_name() const { return m_->device_name; }
int VideoInput::actual_width()  const { return m_->actual_w.load(); }
int VideoInput::actual_height() const { return m_->actual_h.load(); }

// ── capture_still ─────────────────────────────────────────────────────────────

VideoFrame capture_still(const std::string& device_name,
                         int width, int height, int timeout_ms,
                         std::function<bool()> cancel_fn) {
    VideoFrame         result;
    std::mutex         mu;
    std::condition_variable cv;
    bool               got_frame = false;

    VideoInput vin;
    vin.start(device_name, width, height, 0,
              [&](const VideoFrame& f) {
                  std::lock_guard<std::mutex> lk(mu);
                  if (!got_frame) {
                      result    = f;
                      got_frame = true;
                      cv.notify_one();
                  }
              });

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);

    while (true) {
        {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait_for(lk, std::chrono::milliseconds(80),
                        [&] { return got_frame; });
            if (got_frame) break;
        }
        if (cancel_fn && cancel_fn()) {
            vin.stop();
            throw std::runtime_error("video: cancelled");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            vin.stop();
            throw std::runtime_error("video: timeout waiting for first frame");
        }
    }
    vin.stop();
    return result;
}

// ── WIC encode helpers ────────────────────────────────────────────────────────

// WIC (and the COM-based WIC factory) require COM to be initialized on the
// calling thread. The webcam path gets this for free via Media Foundation
// (MFStartup + the capture thread's CoInitializeEx), but the GDI screen-capture
// path never touches COM — so encode_jpeg()/save_frame() called from a
// ScreenSource thread (record) or the main thread (image) would fail with
// "WIC factory unavailable". Initialize once per thread (idempotent; one ref is
// reclaimed when the thread exits) before any WIC call.
//
// Per-thread COM for WIC. Never force MTA on a thread that already owns STA (the UI
// thread after OleInitialize) — that poisons Win32++ startup. Worker capture threads
// still call CoInitializeEx(MTA) in their own read_loop().
void ensure_com_initialized() {
    thread_local bool inited = false;
    if (inited)
        return;
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr == RPC_E_CHANGED_MODE || hr == S_FALSE) {
        inited = true;
        return;
    }
    if (SUCCEEDED(hr))
        inited = true;
}

static ComPtr<IWICImagingFactory> get_wic_factory() {
    ensure_com_initialized();
    static ComPtr<IWICImagingFactory> s_factory;
    static std::once_flag             s_once;
    std::call_once(s_once, [] {
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&s_factory));
    });
    return s_factory;
}

// Returns WIC GUID for the container format from a file extension.
static bool container_from_ext(const std::string& path,
                                GUID& container, GUID& pixel_fmt) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (ext == "jpg" || ext == "jpeg") {
        container = GUID_ContainerFormatJpeg;
        pixel_fmt = GUID_WICPixelFormat24bppBGR;
        return true;
    }
    if (ext == "png") {
        container = GUID_ContainerFormatPng;
        pixel_fmt = GUID_WICPixelFormat24bppBGR;
        return true;
    }
    if (ext == "bmp") {
        container = GUID_ContainerFormatBmp;
        pixel_fmt = GUID_WICPixelFormat24bppBGR;
        return true;
    }
    return false;
}

std::vector<uint8_t> encode_jpeg(const VideoFrame& frame, int quality) {
    if (frame.data.empty() || frame.width <= 0 || frame.height <= 0) return {};

    auto pWIC = get_wic_factory();
    if (!pWIC) return {};

    // Encode to a memory stream backed by HGLOBAL.
    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, 0);
    if (!hGlobal) return {};

    ComPtr<IStream> pStream;
    if (FAILED(CreateStreamOnHGlobal(hGlobal, TRUE, &pStream))) {
        GlobalFree(hGlobal);
        return {};
    }

    ComPtr<IWICBitmapEncoder> pEncoder;
    if (FAILED(pWIC->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &pEncoder))) return {};
    if (FAILED(pEncoder->Initialize(pStream.Get(), WICBitmapEncoderNoCache))) return {};

    ComPtr<IWICBitmapFrameEncode> pFrame;
    ComPtr<IPropertyBag2>         pBag;
    if (FAILED(pEncoder->CreateNewFrame(&pFrame, &pBag))) return {};

    // Set JPEG quality.
    if (pBag) {
        PROPBAG2 option{};
        option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT val{};
        val.vt   = VT_R4;
        val.fltVal = static_cast<float>(quality) / 100.0f;
        pBag->Write(1, &option, &val);
    }

    if (FAILED(pFrame->Initialize(pBag.Get()))) return {};

    UINT w = static_cast<UINT>(frame.width);
    UINT h = static_cast<UINT>(frame.height);
    if (FAILED(pFrame->SetSize(w, h))) return {};

    GUID wicFmt = GUID_WICPixelFormat24bppBGR;
    if (FAILED(pFrame->SetPixelFormat(&wicFmt))) return {};

    // Frames from VideoInput are BGR24 top-down.
    UINT stride = static_cast<UINT>(frame.stride > 0 ? frame.stride : frame.width * 3);
    UINT cbData = stride * h;
    if (FAILED(pFrame->WritePixels(h, stride, cbData,
                                   const_cast<BYTE*>(frame.data.data()))))
        return {};

    if (FAILED(pFrame->Commit())) return {};
    if (FAILED(pEncoder->Commit())) return {};

    // Copy HGLOBAL to vector.
    STATSTG stat{};
    pStream->Stat(&stat, STATFLAG_NONAME);
    size_t size = static_cast<size_t>(stat.cbSize.QuadPart);
    std::vector<uint8_t> out(size);

    LARGE_INTEGER li{}; li.QuadPart = 0;
    pStream->Seek(li, STREAM_SEEK_SET, nullptr);
    ULONG read = 0;
    pStream->Read(out.data(), static_cast<ULONG>(size), &read);
    out.resize(read);
    return out;
}

bool save_frame(const VideoFrame& frame, const std::string& path,
                std::string& err, int jpeg_quality) {
    if (frame.data.empty() || frame.width <= 0 || frame.height <= 0) {
        err = "empty frame"; return false;
    }

    GUID container, pixel_fmt;
    if (!container_from_ext(path, container, pixel_fmt)) {
        err = "unsupported extension (use .jpg, .png, .bmp)";
        return false;
    }

    auto pWIC = get_wic_factory();
    if (!pWIC) { err = "WIC factory unavailable"; return false; }

    std::wstring wpath = utf8_to_wide(path);

    ComPtr<IWICStream> pStream;
    if (FAILED(pWIC->CreateStream(&pStream)) ||
        FAILED(pStream->InitializeFromFilename(wpath.c_str(), GENERIC_WRITE))) {
        err = "cannot create output stream: " + path;
        return false;
    }

    ComPtr<IWICBitmapEncoder> pEncoder;
    if (FAILED(pWIC->CreateEncoder(container, nullptr, &pEncoder)) ||
        FAILED(pEncoder->Initialize(pStream.Get(), WICBitmapEncoderNoCache))) {
        err = "WIC encoder init failed";
        return false;
    }

    ComPtr<IWICBitmapFrameEncode> pFrame;
    ComPtr<IPropertyBag2>         pBag;
    if (FAILED(pEncoder->CreateNewFrame(&pFrame, &pBag))) {
        err = "WIC CreateNewFrame failed"; return false;
    }

    // Set JPEG quality for JPEG container.
    if (container == GUID_ContainerFormatJpeg && pBag) {
        PROPBAG2 option{};
        option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT val{}; val.vt = VT_R4;
        val.fltVal = static_cast<float>(jpeg_quality) / 100.0f;
        pBag->Write(1, &option, &val);
    }

    if (FAILED(pFrame->Initialize(pBag.Get()))) { err = "WIC frame init"; return false; }

    UINT w = static_cast<UINT>(frame.width);
    UINT h = static_cast<UINT>(frame.height);
    pFrame->SetSize(w, h);
    GUID fmt = pixel_fmt;
    pFrame->SetPixelFormat(&fmt);

    UINT stride = static_cast<UINT>(frame.stride > 0 ? frame.stride : frame.width * 3);
    UINT cbData = stride * h;
    if (FAILED(pFrame->WritePixels(h, stride, cbData,
                                   const_cast<BYTE*>(frame.data.data())))) {
        err = "WIC WritePixels failed"; return false;
    }

    if (FAILED(pFrame->Commit()) || FAILED(pEncoder->Commit())) {
        err = "WIC Commit failed"; return false;
    }
    return true;
}

bool load_frame(const std::string& path, VideoFrame& out, std::string& err) {
    auto pWIC = get_wic_factory();
    if (!pWIC) { err = "WIC factory unavailable"; return false; }

    std::wstring wpath = utf8_to_wide(path);

    ComPtr<IWICBitmapDecoder> pDecoder;
    if (FAILED(pWIC->CreateDecoderFromFilename(wpath.c_str(), nullptr, GENERIC_READ,
                                               WICDecodeMetadataCacheOnDemand, &pDecoder))) {
        err = "cannot open image: " + path;
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> pFrame;
    if (FAILED(pDecoder->GetFrame(0, &pFrame))) {
        err = "no frame in image: " + path;
        return false;
    }

    // Convert whatever the source pixel format is → packed 24bpp BGR.
    ComPtr<IWICFormatConverter> pConv;
    if (FAILED(pWIC->CreateFormatConverter(&pConv)) ||
        FAILED(pConv->Initialize(pFrame.Get(), GUID_WICPixelFormat24bppBGR,
                                 WICBitmapDitherTypeNone, nullptr, 0.0,
                                 WICBitmapPaletteTypeCustom))) {
        err = "WIC convert to BGR24 failed: " + path;
        return false;
    }

    UINT w = 0, h = 0;
    pConv->GetSize(&w, &h);
    if (w == 0 || h == 0) { err = "zero-size image: " + path; return false; }

    out.width        = static_cast<int>(w);
    out.height       = static_cast<int>(h);
    out.stride       = static_cast<int>(w) * 3;
    out.format       = PixelFormat::BGR24;
    out.timestamp_ms = 0;
    out.data.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);

    WICRect rc{0, 0, static_cast<INT>(w), static_cast<INT>(h)};
    if (FAILED(pConv->CopyPixels(&rc, static_cast<UINT>(out.stride),
                                 static_cast<UINT>(out.data.size()), out.data.data()))) {
        err = "WIC CopyPixels failed: " + path;
        return false;
    }
    return true;
}

} // namespace pm::video

#endif // FEATURE_VIDEO
