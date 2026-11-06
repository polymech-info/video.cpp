#if defined(_WIN32) && defined(FEATURE_VIDEO) && FEATURE_VIDEO

#include "pm/video/screen.hpp"
#include "pm/video/frame_source.hpp"
#include "pm/video/win_dpi.hpp"
#include "pm/video/window_bounds.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <tlhelp32.h>
#include "pm/video/text.hpp"

namespace pm::video {

// Screen geometry / BitBlt must run under PerMonitorV2 (see win/dpi_thread.hpp).
using ThreadDpiPerMonitorScope = media::win::ThreadDpiPerMonitorScope;

// ── Glob matching (case-insensitive, * = any, ? = one) ───────────────────────

static bool glob_match(const std::wstring& text, const std::wstring& pattern) noexcept
{
    // Classic DP-free iterative glob (no heap alloc for short strings).
    const wchar_t* t = text.c_str();
    const wchar_t* p = pattern.c_str();
    const wchar_t* star_p = nullptr;
    const wchar_t* star_t = nullptr;
    while (*t) {
        const wchar_t pl = towlower(*p), tl = towlower(*t);
        if (pl == L'*') { star_p = ++p; star_t = t; }
        else if (pl == L'?' || pl == tl) { ++p; ++t; }
        else if (star_p) { p = star_p; t = ++star_t; }
        else return false;
    }
    while (*p == L'*') ++p;
    return *p == L'\0';
}

static bool glob_match_utf8(const std::string& text, const std::string& pattern) noexcept
{
    // Convert to wchar for case-insensitive compare.
    const int tw = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    const int pw = MultiByteToWideChar(CP_UTF8, 0, pattern.c_str(), -1, nullptr, 0);
    if (tw <= 0 || pw <= 0) return false;
    std::wstring wt(tw, L'\0'), wp(pw, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wt.data(), tw);
    MultiByteToWideChar(CP_UTF8, 0, pattern.c_str(), -1, wp.data(), pw);
    return glob_match(wt, wp);
}

// ── Window enumeration ────────────────────────────────────────────────────────

// (public) Same matcher used by resolve_hwnd — see header.
bool wintitle_glob_match(const std::string& title, const std::string& glob) noexcept
{
    return glob_match_utf8(title, glob);
}

struct EnumCtx {
    std::vector<ScreenSource::WindowInfo> list;
};

static std::string wstr_to_utf8(const std::wstring& ws)
{
    if (ws.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

static BOOL CALLBACK enum_windows_cb(HWND hwnd, LPARAM lp)
{
    if (!::IsWindowVisible(hwnd)) return TRUE;
    RECT r{};
    if (!::GetWindowRect(hwnd, &r)) return TRUE;
    if (r.right <= r.left || r.bottom <= r.top) return TRUE;

    wchar_t buf[512] = {};
    ::GetWindowTextW(hwnd, buf, 512);
    if (buf[0] == L'\0') return TRUE;   // skip untitled windows

    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);

    ScreenSource::WindowInfo wi;
    wi.hwnd  = hwnd;
    wi.title = wstr_to_utf8(buf);
    wi.pid   = static_cast<int>(pid);
    wi.x = r.left; wi.y = r.top;
    wi.w = r.right - r.left;
    wi.h = r.bottom - r.top;

    auto* ctx = reinterpret_cast<EnumCtx*>(lp);
    ctx->list.push_back(std::move(wi));
    return TRUE;
}

std::vector<ScreenSource::WindowInfo> ScreenSource::enumerate_windows()
{
    ThreadDpiPerMonitorScope dpi_scope;  // native window rects on HiDPI
    EnumCtx ctx;
    ::EnumWindows(enum_windows_cb, reinterpret_cast<LPARAM>(&ctx));
    return std::move(ctx.list);
}

// ── Monitor enumeration ───────────────────────────────────────────────────────

struct MonitorCtx {
    std::vector<ScreenSource::MonitorInfo> list;
    int idx = 0;
};

static BOOL CALLBACK enum_monitors_cb(HMONITOR hmon, HDC, LPRECT, LPARAM lp)
{
    MONITORINFOEXA mi{};
    mi.cbSize = sizeof(mi);
    if (!::GetMonitorInfoA(hmon, &mi)) return TRUE;

    ScreenSource::MonitorInfo info;
    info.index      = reinterpret_cast<MonitorCtx*>(lp)->idx++;
    info.name       = mi.szDevice;
    info.x          = mi.rcMonitor.left;
    info.y          = mi.rcMonitor.top;
    info.w          = mi.rcMonitor.right  - mi.rcMonitor.left;
    info.h          = mi.rcMonitor.bottom - mi.rcMonitor.top;
    info.is_primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    reinterpret_cast<MonitorCtx*>(lp)->list.push_back(std::move(info));
    return TRUE;
}

std::vector<ScreenSource::MonitorInfo> ScreenSource::enumerate_monitors()
{
    ThreadDpiPerMonitorScope dpi_scope;  // native monitor bounds on HiDPI
    MonitorCtx ctx;
    ::EnumDisplayMonitors(nullptr, nullptr, enum_monitors_cb, reinterpret_cast<LPARAM>(&ctx));
    return std::move(ctx.list);
}

// ── Originating HWND for screen:current ───────────────────────────────────────

static std::atomic<uintptr_t> g_originating_capture_hwnd{0};

void set_originating_capture_hwnd(HWND hwnd) noexcept
{
    g_originating_capture_hwnd.store(
        reinterpret_cast<uintptr_t>(hwnd && ::IsWindow(hwnd) ? hwnd : nullptr),
        std::memory_order_release);
}

static DWORD parent_process_id() noexcept
{
    const DWORD self = ::GetCurrentProcessId();
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD parent = 0;
    if (::Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == self) {
                parent = pe.th32ParentProcessID;
                break;
            }
        } while (::Process32NextW(snap, &pe));
    }
    ::CloseHandle(snap);
    return parent;
}

static HWND largest_top_level_window_for_pid(DWORD pid) noexcept
{
    if (!pid) return nullptr;
    struct Ctx {
        DWORD pid = 0;
        HWND  best = nullptr;
        long  best_area = 0;
    } ctx;
    ctx.pid = pid;
    ::EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            DWORD wpid = 0;
            ::GetWindowThreadProcessId(hwnd, &wpid);
            if (wpid != c->pid) return TRUE;
            if (!::IsWindowVisible(hwnd) || ::GetWindow(hwnd, GW_OWNER)) return TRUE;
            RECT r{};
            if (!::GetWindowRect(hwnd, &r)) return TRUE;
            const long area = (r.right - r.left) * (r.bottom - r.top);
            if (area > c->best_area) {
                c->best_area = area;
                c->best = hwnd;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

HWND originating_capture_hwnd() noexcept
{
    HWND pinned = reinterpret_cast<HWND>(
        g_originating_capture_hwnd.load(std::memory_order_acquire));
    if (pinned && ::IsWindow(pinned))
        return ::GetAncestor(pinned, GA_ROOT);

    if (const char* env = std::getenv("PM_IMAGE_HOST_HWND")) {
        try {
            const auto raw = static_cast<uintptr_t>(std::stoull(env));
            HWND hwnd = reinterpret_cast<HWND>(raw);
            if (hwnd && ::IsWindow(hwnd))
                return ::GetAncestor(hwnd, GA_ROOT);
        } catch (...) {}
    }

    if (HWND parent = largest_top_level_window_for_pid(parent_process_id()))
        return parent;
    return nullptr;
}

static int monitor_index_for_hwnd(HWND hwnd) noexcept
{
    if (!hwnd || !::IsWindow(hwnd)) return 0;
    ThreadDpiPerMonitorScope dpi_scope;
    HMONITOR target = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!target) return 0;

    struct Ctx {
        HMONITOR target = nullptr;
        int index = 0;
        int found = -1;
    } ctx;
    ctx.target = target;
    ::EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            if (monitor == c->target) {
                c->found = c->index;
                return FALSE;
            }
            ++c->index;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.found >= 0 ? ctx.found : 0;
}

int resolve_effective_monitor_index(const ScreenSpec& spec) noexcept
{
    if (!spec.use_current)
        return spec.monitor_idx < 0 ? 0 : spec.monitor_idx;
    return monitor_index_for_hwnd(originating_capture_hwnd());
}

// ── Spec parser ───────────────────────────────────────────────────────────────

bool parse_screen_spec(const std::string& spec, ScreenSpec& out) noexcept
{
    out = ScreenSpec{};  // reset to defaults

    // Bare "screen" / "screen:" → monitor of the originating host HWND.
    if (spec == "screen" || spec == "screen:") {
        out.use_current = true;
        return true;
    }
    if (spec.size() < 7 || spec.substr(0, 7) != "screen:") return false;

    const std::string rest = spec.substr(7);  // "0", "current", "0:wintitle=pm*"
    std::istringstream ss(rest);
    std::string part;

    std::vector<std::string> parts;
    while (std::getline(ss, part, ':')) parts.push_back(part);

    if (!parts.empty()) {
        std::string token = parts[0];
        token = pm::video::lower_ascii(std::move(token));
        if (token == "current") {
            out.use_current = true;
            out.monitor_idx = 0;
        } else {
            try { out.monitor_idx = std::stoi(parts[0]); } catch (...) {}
        }
    }
    auto is_known_key = [](const std::string& key) noexcept {
        return key == "wintitle" || key == "title" || key == "hwnd" ||
               key == "rect" || key == "rrect" || key == "layer" || key == "include";
    };
    auto apply_tag = [&out](const std::string& key, const std::string& val) {
        auto lower = [](std::string s) {
            s = pm::video::lower_ascii(std::move(s));
            return s;
        };
        if (key == "wintitle" || key == "title") {
            out.wintitle_glob = val;
        } else if (key == "layer" || key == "include") {
            const std::string v = lower(val);
            if (v == "desktop" || v == "wallpaper")
                out.layer = ScreenSpec::CaptureLayer::Desktop;
            else if (v == "monitor" || v == "screen" || v == "apps" || v == "windows")
                out.layer = ScreenSpec::CaptureLayer::Monitor;
        } else if (key == "hwnd") {
            try { out.hwnd = std::stoull(val); } catch (...) {}
        } else if (key == "rect") {
            // rect=l,t,w,h (comma-separated; commas survive the ':' split)
            int v[4] = {0, 0, 0, 0};
            int n = 0;
            std::istringstream rs(val);
            std::string num;
            while (n < 4 && std::getline(rs, num, ',')) {
                try { v[n] = std::stoi(num); } catch (...) { v[n] = 0; }
                ++n;
            }
            if (n == 4 && v[2] > 0 && v[3] > 0) {
                out.has_rect = true;
                out.rect_l = v[0]; out.rect_t = v[1]; out.rect_w = v[2]; out.rect_h = v[3];
            }
        } else if (key == "rrect") {
            // rrect=l,t,w,h — offset relative to the resolved window's top-left.
            int v[4] = {0, 0, 0, 0};
            int n = 0;
            std::istringstream rs(val);
            std::string num;
            while (n < 4 && std::getline(rs, num, ',')) {
                try { v[n] = std::stoi(num); } catch (...) { v[n] = 0; }
                ++n;
            }
            if (n == 4 && v[2] > 0 && v[3] > 0) {
                out.has_rrect = true;
                out.rr_l = v[0]; out.rr_t = v[1]; out.rr_w = v[2]; out.rr_h = v[3];
            }
        }
    };

    // Remaining parts are key=value tags (or a bare wintitle glob). Keep ':' inside
    // wintitle values by appending unknown/bare chunks until the next known tag.
    std::string pending_key;
    std::string pending_val;
    auto flush_pending = [&] {
        if (!pending_key.empty()) {
            apply_tag(pending_key, pending_val);
            pending_key.clear();
            pending_val.clear();
        }
    };
    for (size_t i = 1; i < parts.size(); ++i) {
        const std::string& tag = parts[i];
        const auto eq = tag.find('=');
        if (eq != std::string::npos) {
            const std::string key = tag.substr(0, eq);
            const std::string val = tag.substr(eq + 1);
            if (is_known_key(key)) {
                flush_pending();
                pending_key = key;
                pending_val = val;
                continue;
            }
        }
        if (pending_key == "wintitle" || pending_key == "title") {
            pending_val += ":" + tag;
            continue;
        }
        flush_pending();
        if (eq == std::string::npos)
            out.wintitle_glob = tag;  // bare glob shorthand
    }
    flush_pending();
    if (out.has_rrect && out.hwnd == 0 && out.wintitle_glob.empty()) {
        out.has_rect = true;
        out.rect_l = out.rr_l; out.rect_t = out.rr_t; out.rect_w = out.rr_w; out.rect_h = out.rr_h;
        out.has_rrect = false;
    }
    return true;
}

// ── GDI capture helpers ───────────────────────────────────────────────────────

static void draw_cursor_if_inside(HDC dc, const RECT& rect)
{
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (!::GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor)
        return;
    ICONINFO ii{};
    if (!::GetIconInfo(ci.hCursor, &ii))
        return;

    int hot_x = static_cast<int>(ii.xHotspot);
    int hot_y = static_cast<int>(ii.yHotspot);
    int bmp_w = 0;
    int bmp_h = 0;
    const bool had_color = ii.hbmColor != nullptr;
    if (ii.hbmColor) {
        BITMAP bm{};
        if (::GetObject(ii.hbmColor, sizeof(bm), &bm)) {
            bmp_w = bm.bmWidth;
            bmp_h = bm.bmHeight;
        }
        ::DeleteObject(ii.hbmColor);
    }
    if (ii.hbmMask) {
        if (bmp_w <= 0 || bmp_h <= 0) {
            BITMAP bm{};
            if (::GetObject(ii.hbmMask, sizeof(bm), &bm)) {
                bmp_w = bm.bmWidth;
                // Mono mask stores AND+XOR stacked → half height is the image.
                bmp_h = (had_color || bm.bmHeight <= 0)
                    ? bm.bmHeight
                    : bm.bmHeight / 2;
            }
        }
        ::DeleteObject(ii.hbmMask);
    }

    // Capture rect is physical px; draw the cursor at the monitor's effective
    // cursor metrics so 200% DPI does not composite a tiny/blocky 1× glyph.
    const UINT dpi = media::win::monitor_dpi_for_rect(rect);
    int draw_w = media::win::system_metric_for_dpi(SM_CXCURSOR, dpi);
    int draw_h = media::win::system_metric_for_dpi(SM_CYCURSOR, dpi);
    if (draw_w <= 0) draw_w = 32;
    if (draw_h <= 0) draw_h = 32;
    if (bmp_w > 0 && bmp_h > 0) {
        // Prefer the larger of system metric vs actual glyph so HiDPI cursor
        // resources (already 2×) are not downscaled into a soft mess.
        draw_w = std::max(draw_w, bmp_w);
        draw_h = std::max(draw_h, bmp_h);
        hot_x = MulDiv(hot_x, draw_w, bmp_w);
        hot_y = MulDiv(hot_y, draw_h, bmp_h);
    }

    const int x = ci.ptScreenPos.x - rect.left - hot_x;
    const int y = ci.ptScreenPos.y - rect.top - hot_y;
    if (x > rect.right - rect.left || y > rect.bottom - rect.top)
        return;
    ::DrawIconEx(
        dc, x, y, ci.hCursor, draw_w, draw_h, 0, nullptr, DI_NORMAL);
}

// Capture a rectangular region from the screen into a BGR24 VideoFrame.
// rect is in screen coordinates (SetThreadDpiAwarenessContext recommended).
//
// include_layered: CAPTUREBLT pulls in layered/transparent topmost windows that
// overlap the rect. We WANT that for a faithful screenshot, but NOT when a
// concurrent live overlay (DetectOverlay/HighlightOverlay) sits over the same
// region — otherwise its boxes/skeletons get captured and then drawn again
// (double paint). WDA_EXCLUDEFROMCAPTURE only handles this on Win10 2004+, so
// callers that run alongside an overlay pass include_layered=false instead.
static bool gdi_capture_rect(const RECT& rect, VideoFrame& out, bool include_layered = true, bool include_cursor = true)
{
    const int W = rect.right  - rect.left;
    const int H = rect.bottom - rect.top;
    if (W <= 0 || H <= 0) return false;

    HDC screen_dc = ::GetDC(nullptr);
    if (!screen_dc) return false;

    HDC mem_dc = ::CreateCompatibleDC(screen_dc);
    if (!mem_dc) { ::ReleaseDC(nullptr, screen_dc); return false; }

    BITMAPINFOHEADER bih{};
    bih.biSize        = sizeof(bih);
    bih.biWidth       = W;
    bih.biHeight      = -H;  // top-down
    bih.biPlanes      = 1;
    bih.biBitCount    = 32;  // BGRA
    bih.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = ::CreateDIBSection(mem_dc, reinterpret_cast<BITMAPINFO*>(&bih),
                                     DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) {
        ::DeleteDC(mem_dc);
        ::ReleaseDC(nullptr, screen_dc);
        return false;
    }

    HGDIOBJ old = ::SelectObject(mem_dc, bmp);
    const DWORD rop = include_layered ? (SRCCOPY | CAPTUREBLT) : SRCCOPY;
    const BOOL ok = ::BitBlt(mem_dc, 0, 0, W, H,
                             screen_dc, rect.left, rect.top,
                             rop);
    if (ok && include_cursor)
        draw_cursor_if_inside(mem_dc, rect);

    BOOL copied = ok;
    if (copied && (W <= 0 || H <= 0 || W > (std::numeric_limits<int>::max() / 4)))
        copied = FALSE;
    if (copied) {
        // BGRA → BGR24 (drop alpha channel)
        const int stride32 = W * 4;
        const int stride24 = W * 3;
        out.width       = W;
        out.height      = H;
        out.stride      = stride24;
        out.format      = PixelFormat::BGR24;
        out.timestamp_ms = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        out.data.resize(static_cast<size_t>(stride24 * H));
        const auto* src = reinterpret_cast<const uint8_t*>(bits);
        uint8_t* dst    = out.data.data();
        for (int row = 0; row < H; ++row) {
            for (int col = 0; col < W; ++col) {
                const uint8_t* s = src + row * stride32 + col * 4;
                uint8_t*       d = dst + row * stride24 + col * 3;
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2];  // BGR
            }
        }
    }

    ::SelectObject(mem_dc, old);
    ::DeleteObject(bmp);
    ::DeleteDC(mem_dc);
    ::ReleaseDC(nullptr, screen_dc);
    return copied;
}

// Read a sub-region from an HBITMAP into a packed BGR24 VideoFrame.
static bool hbitmap_region_to_frame(HBITMAP bmp, int src_x, int src_y,
                                    int src_w, int src_h, VideoFrame& out)
{
    BITMAP bm{};
    if (!::GetObject(bmp, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0)
        return false;
    if (bm.bmWidth > (std::numeric_limits<int>::max() - 3) / 3)
        return false;

    src_x = std::max(0, src_x);
    src_y = std::max(0, src_y);
    src_w = std::min(src_w, static_cast<int>(bm.bmWidth)  - src_x);
    src_h = std::min(src_h, static_cast<int>(bm.bmHeight) - src_y);
    if (src_w <= 0 || src_h <= 0) return false;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = bm.bmWidth;
    bmi.bmiHeader.biHeight      = -bm.bmHeight;  // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 24;
    bmi.bmiHeader.biCompression = BI_RGB;

    const int full_stride = (static_cast<int>(bm.bmWidth) * 3 + 3) & ~3;
    if (full_stride <= 0 || bm.bmHeight > (std::numeric_limits<int>::max() / full_stride))
        return false;
    std::vector<uint8_t> full(static_cast<size_t>(full_stride) * bm.bmHeight);

    HDC tmp = ::CreateCompatibleDC(nullptr);
    if (!tmp) return false;
    const bool got = ::GetDIBits(tmp, bmp, 0, static_cast<UINT>(bm.bmHeight),
                                 full.data(), &bmi, DIB_RGB_COLORS) != 0;
    ::DeleteDC(tmp);
    if (!got) return false;

    const int dst_stride = src_w * 3;
    out.width  = src_w;
    out.height = src_h;
    out.stride = dst_stride;
    out.format = PixelFormat::BGR24;
    out.timestamp_ms = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    out.data.resize(static_cast<size_t>(dst_stride) * src_h);
    for (int row = 0; row < src_h; ++row) {
        std::memcpy(out.data.data() + static_cast<size_t>(row) * dst_stride,
                    full.data() + static_cast<size_t>(src_y + row) * full_stride
                                + static_cast<size_t>(src_x) * 3,
                    static_cast<size_t>(dst_stride));
    }
    return true;
}

// PrintWindow can include DWM's one-pixel black visible frame. Trim only edges
// whose complete perimeter is black so ordinary window content is untouched.
static void trim_black_window_frame(VideoFrame& frame)
{
    if (frame.format != PixelFormat::BGR24 ||
        frame.width <= 2 || frame.height <= 2 ||
        !rgb_frame_fits(frame))
        return;

    const auto black = [](const uint8_t* pixel) {
        return pixel[0] <= 4 && pixel[1] <= 4 && pixel[2] <= 4;
    };
    const auto row_black = [&](int y) {
        const uint8_t* row =
            frame.data.data() + static_cast<size_t>(y) * frame.stride;
        for (int x = 0; x < frame.width; ++x) {
            if (!black(row + static_cast<size_t>(x) * 3))
                return false;
        }
        return true;
    };
    const auto column_black = [&](int x) {
        for (int y = 0; y < frame.height; ++y) {
            const uint8_t* pixel =
                frame.data.data() + static_cast<size_t>(y) * frame.stride +
                static_cast<size_t>(x) * 3;
            if (!black(pixel))
                return false;
        }
        return true;
    };

    const int trim_top = row_black(0) ? 1 : 0;
    const int trim_bottom = row_black(frame.height - 1) ? 1 : 0;
    const int trim_left = column_black(0) ? 1 : 0;
    const int trim_right = column_black(frame.width - 1) ? 1 : 0;
    if (!(trim_top || trim_bottom || trim_left || trim_right))
        return;

    const int width = frame.width - trim_left - trim_right;
    const int height = frame.height - trim_top - trim_bottom;
    if (width <= 0 || height <= 0)
        return;

    const int stride = width * 3;
    std::vector<uint8_t> pixels(static_cast<size_t>(stride) * height);
    for (int y = 0; y < height; ++y) {
        const uint8_t* source =
            frame.data.data() +
            static_cast<size_t>(y + trim_top) * frame.stride +
            static_cast<size_t>(trim_left) * 3;
        std::memcpy(
            pixels.data() + static_cast<size_t>(y) * stride,
            source, static_cast<size_t>(stride));
    }
    frame.width = width;
    frame.height = height;
    frame.stride = stride;
    frame.data = std::move(pixels);
}

// Capture a top-level window via PrintWindow(PW_RENDERFULLCONTENT), optionally
// cropping to a window-relative rrect=. Falls back to BitBlt at the caller.
static bool printwindow_capture_hwnd(HWND hwnd, const ScreenSpec& spec, VideoFrame& out)
{
    if (!hwnd || !::IsWindow(hwnd)) return false;

    RECT visible{};
    if (!media::win::visible_window_bounds(hwnd, visible)) return false;
    const int visible_w = visible.right - visible.left;
    const int visible_h = visible.bottom - visible.top;
    if (visible_w <= 0 || visible_h <= 0) return false;

    // PrintWindow renders in GetWindowRect space, which includes DWM's
    // invisible resize border. Picker/rrect coordinates use the visible DWM
    // frame. Render the full raw canvas, then translate back to visible space.
    // Otherwise maximized windows are commonly shifted about -7 px in X and
    // retain a one-pixel strip at the top/bottom.
    RECT raw = visible;
    RECT window_rect{};
    if (::GetWindowRect(hwnd, &window_rect) &&
        window_rect.right > window_rect.left &&
        window_rect.bottom > window_rect.top) {
        raw = window_rect;
    }
    const int raw_w = raw.right - raw.left;
    const int raw_h = raw.bottom - raw.top;
    const int visible_x = visible.left - raw.left;
    const int visible_y = visible.top - raw.top;

    HDC screen_dc = ::GetDC(nullptr);
    if (!screen_dc) return false;
    HDC mem_dc = ::CreateCompatibleDC(screen_dc);
    HBITMAP bmp = ::CreateCompatibleBitmap(screen_dc, raw_w, raw_h);
    HGDIOBJ old = mem_dc && bmp ? ::SelectObject(mem_dc, bmp) : nullptr;

    BOOL painted = FALSE;
    if (mem_dc && bmp) {
        constexpr UINT kPrintFullContent = 0x00000002;  // PW_RENDERFULLCONTENT
        painted = ::PrintWindow(hwnd, mem_dc, kPrintFullContent);
        if (!painted)
            painted = ::PrintWindow(hwnd, mem_dc, 0);
    }

    if (old) ::SelectObject(mem_dc, old);
    if (mem_dc) ::DeleteDC(mem_dc);
    ::ReleaseDC(nullptr, screen_dc);

    if (!painted || !bmp) {
        if (bmp) ::DeleteObject(bmp);
        return false;
    }

    int cx = visible_x;
    int cy = visible_y;
    int cw = visible_w;
    int ch = visible_h;
    if (spec.has_rrect) {
        cx += spec.rr_l;
        cy += spec.rr_t;
        cw = spec.rr_w;
        ch = spec.rr_h;
    }

    const bool ok = hbitmap_region_to_frame(bmp, cx, cy, cw, ch, out);
    ::DeleteObject(bmp);
    if (ok && !spec.has_rrect)
        trim_black_window_frame(out);
    return ok;
}

// ── HWND resolver ─────────────────────────────────────────────────────────────

struct FindHwndCtx {
    std::string  glob;
    HWND         result = nullptr;
};

static BOOL CALLBACK find_hwnd_cb(HWND hwnd, LPARAM lp)
{
    if (!::IsWindowVisible(hwnd)) return TRUE;
    wchar_t buf[512] = {};
    ::GetWindowTextW(hwnd, buf, 512);
    if (buf[0] == L'\0') return TRUE;
    const std::string title = wstr_to_utf8(buf);
    auto* ctx = reinterpret_cast<FindHwndCtx*>(lp);
    if (glob_match_utf8(title, ctx->glob)) {
        ctx->result = hwnd;
        return FALSE;  // stop
    }
    return TRUE;
}

static HWND resolve_hwnd(const std::string& glob)
{
    FindHwndCtx ctx;
    ctx.glob = glob;
    ::EnumWindows(find_hwnd_cb, reinterpret_cast<LPARAM>(&ctx));
    return ctx.result;
}

HWND resolve_screen_window(const ScreenSpec& spec) noexcept
{
    if (spec.hwnd != 0) {
        HWND hwnd = reinterpret_cast<HWND>(
            static_cast<uintptr_t>(spec.hwnd));
        return ::IsWindow(hwnd) ? hwnd : nullptr;
    }
    return spec.wintitle_glob.empty()
        ? nullptr : resolve_hwnd(spec.wintitle_glob);
}

bool screen_spec_promotes_desktop_hwnd(const ScreenSpec& spec, HWND hwnd) noexcept
{
    if (!hwnd || spec.has_rect || spec.has_rrect)
        return false;
    if (spec.layer == ScreenSpec::CaptureLayer::Desktop)
        return false;
    return media::win::hwnd_is_desktop_shell(hwnd);
}

HMONITOR resolve_screen_monitor(int monitor_idx) noexcept
{
    std::vector<HMONITOR> monitors;
    ::EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM context) -> BOOL {
            reinterpret_cast<std::vector<HMONITOR>*>(context)->push_back(monitor);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&monitors));
    if (monitors.empty()) return nullptr;
    const auto index = std::min(
        static_cast<std::size_t>(std::max(0, monitor_idx)),
        monitors.size() - 1);
    return monitors[index];
}

RECT resolve_screen_rect(const ScreenSpec& spec) noexcept
{
    RECT rect{};

    // 1) Fixed absolute rect — never tracks anything.
    if (spec.has_rect) {
        rect = { spec.rect_l, spec.rect_t,
                 spec.rect_l + spec.rect_w, spec.rect_t + spec.rect_h };
        return rect;
    }

    // 2) A window (explicit hwnd= or title glob), optionally a window-relative
    //    sub-rect (rrect=). Resolved against the *live* window rect.
    if (spec.hwnd != 0 || !spec.wintitle_glob.empty()) {
        HWND hwnd = resolve_screen_window(spec);
        if (hwnd && !screen_spec_promotes_desktop_hwnd(spec, hwnd)) {
            RECT wr{};
            if (media::win::visible_window_bounds(hwnd, wr)) {
                if (spec.has_rrect) {
                    rect = { wr.left + spec.rr_l, wr.top + spec.rr_t,
                             wr.left + spec.rr_l + spec.rr_w,
                             wr.top  + spec.rr_t + spec.rr_h };
                } else {
                    rect = wr;
                }
                return rect;
            }
        }
        // desktop-shell hwnd (or missing window) → monitor so apps are included
    }

    // 3) Whole monitor (by index / screen:current, clamped).
    const auto mons = ScreenSource::enumerate_monitors();
    if (!mons.empty()) {
        const int    mon_idx = resolve_effective_monitor_index(spec);
        const size_t m_idx   = static_cast<size_t>(mon_idx) < mons.size()
                               ? static_cast<size_t>(mon_idx) : mons.size() - 1;
        const auto& m = mons[m_idx];
        rect = { m.x, m.y, m.x + m.w, m.y + m.h };
    }
    return rect;
}

// ── ScreenSource::Impl ────────────────────────────────────────────────────────

struct ScreenSource::Impl {
    ScreenSpec            spec;
    std::atomic<bool>     running{false};
    std::thread           thread;
    std::string           desc;

    void run(int target_w, int target_h, int fps, Callback cb)
    {
        ThreadDpiPerMonitorScope dpi_scope;  // native-resolution capture on HiDPI
        (void)target_w; (void)target_h;  // screen capture uses native resolution

        const int    frame_ms = (fps > 0) ? (1000 / fps) : 100;
        HWND         hwnd     = nullptr;
        RECT         capture_rect{};
        bool         rect_valid = false;

        // ── Fixed-rect capture (rect=l,t,w,h) — highest precedence ─────────────
        // A precise screen region (e.g. a picked sub-window UIA element). Does not
        // track window moves; captured every frame as-is.
        if (spec.has_rect) {
            capture_rect = { spec.rect_l, spec.rect_t,
                             spec.rect_l + spec.rect_w, spec.rect_t + spec.rect_h };
            rect_valid = true;
            std::ostringstream s;
            s << "screen:" << spec.monitor_idx << ":rect=" << spec.rect_w << "x" << spec.rect_h
              << " @ (" << spec.rect_l << "," << spec.rect_t << ") " << fps << " fps";
            desc = s.str();
        }

        // ── Explicit HWND capture (hwnd=N) — tracks that window's rect ─────────
        if (!rect_valid && spec.hwnd != 0)
            hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(spec.hwnd));
        if (hwnd && !::IsWindow(hwnd))
            hwnd = nullptr;
        if (hwnd && screen_spec_promotes_desktop_hwnd(spec, hwnd))
            hwnd = nullptr;

        // ── Resolve capture target by title glob ──────────────────────────────
        if (!rect_valid && !hwnd && !spec.wintitle_glob.empty()) {
            hwnd = resolve_hwnd(spec.wintitle_glob);
            if (!hwnd) {
                // Not found on start — will retry each frame.
            }
        }

        if (!rect_valid && !hwnd) {
            // Full monitor/desktop capture.
            // Collect monitors, pick by index (screen:current → originating HWND).
            const auto monitors = ScreenSource::enumerate_monitors();
            const int  idx      = resolve_effective_monitor_index(spec);
            if (!monitors.empty()) {
                const auto& m = monitors[std::min((size_t)idx, monitors.size()-1)];
                capture_rect  = { m.x, m.y, m.x + m.w, m.y + m.h };
                rect_valid    = true;
                std::ostringstream s;
                if (spec.use_current)
                    s << "screen:current->" << idx;
                else
                    s << "screen:" << idx;
                s << " " << m.w << "x" << m.h
                  << " @ " << fps << " fps  [" << m.name << "]";
                desc = s.str();
            } else {
                // Fallback: full virtual desktop.
                capture_rect  = { 0, 0, ::GetSystemMetrics(SM_CXVIRTUALSCREEN),
                                         ::GetSystemMetrics(SM_CYVIRTUALSCREEN) };
                rect_valid    = true;
                desc = "screen:0 (virtual desktop) @ " + std::to_string(fps) + " fps";
            }
        }

        while (running) {
            const auto t0 = std::chrono::steady_clock::now();

            // If tracking a window (by glob or explicit hwnd=): refresh its rect
            // each frame (handles moves/resizes). Fixed rect= skips this. A
            // window-relative rrect= (sub-region that follows the window) also
            // tracks here.
            if (!spec.has_rect && (!spec.wintitle_glob.empty() || spec.hwnd != 0)) {
                if (!hwnd && !spec.wintitle_glob.empty()) hwnd = resolve_hwnd(spec.wintitle_glob);
                if (hwnd && screen_spec_promotes_desktop_hwnd(spec, hwnd))
                    hwnd = nullptr;
                if (hwnd) {
                    RECT wr{};
                    if (media::win::visible_window_bounds(hwnd, wr) && wr.right > wr.left) {
                        if (spec.has_rrect) {
                            // Sub-rect anchored to the live window top-left.
                            capture_rect = { wr.left + spec.rr_l, wr.top + spec.rr_t,
                                             wr.left + spec.rr_l + spec.rr_w,
                                             wr.top  + spec.rr_t + spec.rr_h };
                        } else {
                            capture_rect = wr;
                        }
                        rect_valid   = true;
                        if (desc.empty()) {
                            wchar_t buf[256] = {};
                            ::GetWindowTextW(hwnd, buf, 256);
                            std::ostringstream s;
                            s << "screen:" << spec.monitor_idx;
                            if (spec.hwnd != 0) s << ":hwnd=" << spec.hwnd;
                            else                s << ":wintitle=" << spec.wintitle_glob;
                            if (spec.has_rrect)
                                s << ":rrect=" << spec.rr_w << "x" << spec.rr_h
                                  << "+" << spec.rr_l << "," << spec.rr_t;
                            s << " [" << wstr_to_utf8(buf) << "]"
                              << " " << (capture_rect.right-capture_rect.left)
                              << "x" << (capture_rect.bottom-capture_rect.top)
                              << " @ " << fps << " fps";
                            desc = s.str();
                        }
                    } else {
                        hwnd = nullptr;  // window closed — retry next frame
                    }
                    if (!hwnd && !spec.wintitle_glob.empty())
                        hwnd = resolve_hwnd(spec.wintitle_glob);
                }
            }

            if (rect_valid) {
                VideoFrame frame;
                if (gdi_capture_rect(capture_rect, frame, true, spec.include_cursor))
                    cb(frame);
            }

            // Sleep remainder of frame budget.
            const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            const long sleep_ms = frame_ms - static_cast<long>(elapsed_ms);
            if (sleep_ms > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        }
    }
};

// ── ScreenSource public API ───────────────────────────────────────────────────

ScreenSource::ScreenSource(ScreenSpec spec)
    : impl_(std::make_unique<Impl>())
{
    impl_->spec = std::move(spec);
}

ScreenSource::ScreenSource(const std::string& input_spec)
    : impl_(std::make_unique<Impl>())
{
    if (!parse_screen_spec(input_spec, impl_->spec))
        throw std::invalid_argument("ScreenSource: not a screen spec: " + input_spec);
}

ScreenSource::~ScreenSource() { stop(); }

void ScreenSource::start(int w, int h, int fps, Callback cb)
{
    if (impl_->running) return;
    impl_->running = true;
    impl_->thread  = std::thread([this, w, h, fps, cb = std::move(cb)]() mutable {
        impl_->run(w, h, fps > 0 ? fps : 10, cb);
    });
}

void ScreenSource::stop() noexcept
{
    impl_->running = false;
    if (impl_->thread.joinable()) impl_->thread.join();
}

bool ScreenSource::is_running() const noexcept { return impl_->running; }

std::string ScreenSource::description() const { return impl_->desc; }

// ── WebcamSource ──────────────────────────────────────────────────────────────

WebcamSource::WebcamSource(const std::string& device_name)
    : device_name_(device_name) {}

void WebcamSource::start(int w, int h, int fps, Callback cb)
{
    vin_.start(device_name_, w, h, fps, std::move(cb));
    std::ostringstream s;
    s << vin_.opened_device_name();
    if (vin_.actual_width() > 0)
        s << " " << vin_.actual_width() << "x" << vin_.actual_height()
          << " @ " << fps << " fps";
    desc_ = s.str();
}

void WebcamSource::stop() noexcept
{
    try { vin_.stop(); } catch (...) {}
}

bool WebcamSource::is_running() const noexcept { return vin_.is_running(); }

std::string WebcamSource::description() const { return desc_; }

// ── Single-shot capture ───────────────────────────────────────────────────────

VideoFrame capture_still_screen(const std::string& input_spec, std::string& err,
                                bool include_layered,
                                bool screen_composition)
{
    ThreadDpiPerMonitorScope dpi_scope;  // native-resolution still capture on HiDPI
    VideoFrame out;
    out.width = 0;  // sentinel: failure

    ScreenSpec spec;
    if (!parse_screen_spec(input_spec, spec)) {
        err = "not a screen input spec: " + input_spec;
        return out;
    }

    RECT capture_rect{};
    bool rect_valid = false;
    HWND window_hwnd = nullptr;

    if (spec.has_rect) {
        capture_rect = { spec.rect_l, spec.rect_t,
                         spec.rect_l + spec.rect_w, spec.rect_t + spec.rect_h };
        rect_valid = true;
    }

    if (!rect_valid && (spec.hwnd != 0 || !spec.wintitle_glob.empty())) {
        window_hwnd = spec.hwnd != 0
            ? reinterpret_cast<HWND>(static_cast<uintptr_t>(spec.hwnd))
            : resolve_hwnd(spec.wintitle_glob);
        if (!window_hwnd || !::IsWindow(window_hwnd)) {
            err = spec.hwnd != 0 ? "window not found for hwnd"
                                 : "window not found for glob: " + spec.wintitle_glob;
            return out;
        }
        if (screen_spec_promotes_desktop_hwnd(spec, window_hwnd)) {
            window_hwnd = nullptr;
        } else {
            RECT wr{};
            if (!media::win::visible_window_bounds(window_hwnd, wr)) {
                err = "visible_window_bounds failed";
                return out;
            }
            if (spec.has_rrect) {
                capture_rect = { wr.left + spec.rr_l, wr.top + spec.rr_t,
                                 wr.left + spec.rr_l + spec.rr_w,
                                 wr.top  + spec.rr_t + spec.rr_h };
            } else {
                capture_rect = wr;
            }
            rect_valid = true;
        }
    }

    if (!rect_valid) {
        const auto monitors = ScreenSource::enumerate_monitors();
        if (monitors.empty()) {
            err = "no monitors found";
            return out;
        }
        const int idx = resolve_effective_monitor_index(spec);
        const auto& m = monitors[std::min(static_cast<size_t>(idx), monitors.size() - 1)];
        capture_rect  = { m.x, m.y, m.x + m.w, m.y + m.h };
    }

    // Full-window targets: PrintWindow re-renders GPU/Electron content sharply.
    // Sub-region (rrect=): screen BitBlt — physical coords, captures exactly what's
    // visible. PrintWindow on the root HWND paints the whole app and Win32 child
    // panels (e.g. dockers) often get black borders when cropped.
    // screen_composition: keep the desktop blit so picker HUD / highlight that
    // overlap the window (WDA cleared by PickerHudCaptureScope) are in the frame.
    if (window_hwnd && !spec.has_rrect && !screen_composition
        && printwindow_capture_hwnd(window_hwnd, spec, out))
        return out;

    if (!gdi_capture_rect(capture_rect, out, include_layered, true)) {
        err = window_hwnd ? "window capture failed (PrintWindow + BitBlt)"
                          : "GDI BitBlt failed";
        out.width = 0;
    }
    return out;
}

// ── Factory ───────────────────────────────────────────────────────────────────

bool is_screen_input_spec(const std::string& spec) noexcept
{
    return spec == "screen" ||
           (spec.size() >= 7 && spec.substr(0, 7) == "screen:");
}

FrameSourcePtr make_frame_source(const std::string& input_spec)
{
    ScreenSpec ss;
    if (parse_screen_spec(input_spec, ss))
        return std::make_unique<ScreenSource>(std::move(ss));
    return std::make_unique<WebcamSource>(input_spec);
}

} // namespace pm::video

#endif // _WIN32 && FEATURE_VIDEO
