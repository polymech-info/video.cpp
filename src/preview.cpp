#if defined(_WIN32)

#include "pm/video/preview.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

#undef min
#undef max

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace pm::video {
namespace {

void utf8_to_wide(const std::string& in, std::wstring& out) {
    if (in.empty()) {
        out.clear();
        return;
    }
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, in.c_str(), -1, nullptr, 0);
    if (n <= 0) {
        out.assign(in.begin(), in.end());
        return;
    }
    out.assign(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, in.c_str(), -1, out.data(), n);
    if (!out.empty() && out.back() == L'\0')
        out.pop_back();
}

// Command-runner children (assistant/ribbon) are spawned with CREATE_NO_WINDOW
// and historically STARTF_USESHOWWINDOW+SW_HIDE. The first ShowWindow in a
// process ignores nCmdShow and uses that startup value, so a single
// ShowWindow(SW_SHOW) leaves the preview invisible. SetWindowPos(SWP_SHOWWINDOW)
// is not subject to that rule (same as overlay_hud). A second ShowWindow then
// honors SW_SHOWNORMAL. Raise z-order: the child often has no foreground rights.
void force_preview_visible(HWND h) {
    if (!h || !::IsWindow(h))
        return;
    ::SetWindowPos(h, HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    ::ShowWindow(h, SW_SHOWNORMAL);
    ::ShowWindow(h, SW_SHOWNORMAL);
    (void)::LockSetForegroundWindow(LSFW_UNLOCK);
    (void)::AllowSetForegroundWindow(ASFW_ANY);
    const DWORD self_tid = ::GetCurrentThreadId();
    const HWND fg = ::GetForegroundWindow();
    const DWORD fg_tid = fg ? ::GetWindowThreadProcessId(fg, nullptr) : 0;
    const bool attached = (fg_tid && fg_tid != self_tid)
        && (::AttachThreadInput(self_tid, fg_tid, TRUE) != 0);
    (void)::BringWindowToTop(h);
    (void)::SetForegroundWindow(h);
    (void)::SetActiveWindow(h);
    if (attached)
        (void)::AttachThreadInput(self_tid, fg_tid, FALSE);
    if (::GetForegroundWindow() != h) {
        (void)::SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        (void)::SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE);
        (void)::BringWindowToTop(h);
        (void)::SetForegroundWindow(h);
    }
}

void fit_client_to_work_area(int fw, int fh, int& dw, int& dh) {
    dw = std::max(1, fw);
    dh = std::max(1, fh);
    RECT wa{};
    if (!::SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0))
        return;
    const int max_w = std::max(160, static_cast<int>(wa.right - wa.left) - 32);
    const int max_h = std::max(120, static_cast<int>(wa.bottom - wa.top) - 80);
    if (dw <= max_w && dh <= max_h)
        return;
    const double sx = static_cast<double>(max_w) / static_cast<double>(dw);
    const double sy = static_cast<double>(max_h) / static_cast<double>(dh);
    const double s = std::min(sx, sy);
    dw = std::max(1, static_cast<int>(dw * s));
    dh = std::max(1, static_cast<int>(dh * s));
}

} // namespace

struct FramePreview::Impl {
    std::atomic<bool> running{false};
    std::atomic<HWND> hwnd{nullptr};
    std::thread ui;

    int frame_w = 0;
    int frame_h = 0;
    int client_w = 0;
    int client_h = 0;
    std::wstring title;

    std::mutex mtx;
    std::vector<std::uint8_t> bgr;
    int bgr_w = 0;
    int bgr_h = 0;
    int bgr_stride = 0;

    static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
        auto* self = reinterpret_cast<Impl*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
            self = static_cast<Impl*>(cs->lpCreateParams);
            ::SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return ::DefWindowProcW(h, msg, w, l);

        switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = ::BeginPaint(h, &ps);
            self->paint(h, dc);
            ::EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_CLOSE:
            self->running = false;
            ::DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            self->hwnd = nullptr;
            ::PostQuitMessage(0);
            return 0;
        default:
            return ::DefWindowProcW(h, msg, w, l);
        }
    }

    void paint(HWND h, HDC dc) {
        std::vector<std::uint8_t> copy;
        int w = 0, hgt = 0, stride = 0;
        {
            std::lock_guard<std::mutex> lk(mtx);
            copy = bgr;
            w = bgr_w;
            hgt = bgr_h;
            stride = bgr_stride;
        }
        RECT rc{};
        ::GetClientRect(h, &rc);
        const int dw = std::max(1, static_cast<int>(rc.right - rc.left));
        const int dh = std::max(1, static_cast<int>(rc.bottom - rc.top));
        if (copy.empty() || w <= 0 || hgt <= 0) {
            HBRUSH br = reinterpret_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));
            ::FillRect(dc, &rc, br);
            return;
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -hgt;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 24;
        bmi.bmiHeader.biCompression = BI_RGB;

        const int packed = ((w * 3 + 3) & ~3);
        const std::uint8_t* bits = copy.data();
        std::vector<std::uint8_t> padded;
        if (stride != packed) {
            padded.resize(static_cast<std::size_t>(packed) * static_cast<std::size_t>(hgt));
            for (int y = 0; y < hgt; ++y) {
                const std::uint8_t* src =
                    copy.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
                std::uint8_t* dst =
                    padded.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(packed);
                std::memcpy(dst, src, static_cast<std::size_t>(w) * 3u);
            }
            bits = padded.data();
        }

        ::SetStretchBltMode(dc, HALFTONE);
        ::SetBrushOrgEx(dc, 0, 0, nullptr);
        ::StretchDIBits(dc, 0, 0, dw, dh, 0, 0, w, hgt, bits, &bmi, DIB_RGB_COLORS, SRCCOPY);
    }

    void ui_main() {
        const wchar_t kClass[] = L"PmFramePreview";
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &Impl::wnd_proc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = kClass;
        if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            running = false;
            return;
        }

        RECT wr{0, 0, client_w, client_h};
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        ::AdjustWindowRectEx(&wr, style, FALSE, WS_EX_APPWINDOW);
        const int ww = wr.right - wr.left;
        const int wh = wr.bottom - wr.top;
        const int x = (::GetSystemMetrics(SM_CXSCREEN) - ww) / 2;
        const int y = (::GetSystemMetrics(SM_CYSCREEN) - wh) / 2;

        HWND h = ::CreateWindowExW(WS_EX_APPWINDOW, kClass, title.c_str(), style, x, y, ww, wh,
                                   nullptr, nullptr, ::GetModuleHandleW(nullptr), this);
        if (!h) {
            running = false;
            return;
        }
        hwnd = h;
        BOOL dark = TRUE;
        ::DwmSetWindowAttribute(h, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        force_preview_visible(h);
        ::UpdateWindow(h);

        MSG msg{};
        while (running && ::GetMessageW(&msg, nullptr, 0, 0) > 0) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        if (hwnd) {
            ::DestroyWindow(hwnd);
            hwnd = nullptr;
        }
        running = false;
    }
};

FramePreview::FramePreview() : impl_(std::make_unique<Impl>()) {}

FramePreview::~FramePreview() { close(); }

bool FramePreview::show(int width, int height, const std::string& title) {
    if (impl_->running)
        return impl_->hwnd != nullptr;
    impl_->frame_w = std::max(1, width);
    impl_->frame_h = std::max(1, height);
    fit_client_to_work_area(impl_->frame_w, impl_->frame_h, impl_->client_w, impl_->client_h);
    std::string shown = title.empty() ? "video record" : title;
    shown += "  " + std::to_string(impl_->frame_w) + "x" + std::to_string(impl_->frame_h);
    if (impl_->client_w != impl_->frame_w || impl_->client_h != impl_->frame_h)
        shown += " (fit)";
    utf8_to_wide(shown, impl_->title);
    impl_->running = true;
    impl_->ui = std::thread([this] { impl_->ui_main(); });
    for (int i = 0; i < 200 && impl_->running && !impl_->hwnd; ++i)
        ::Sleep(10);
    return impl_->hwnd != nullptr;
}

void FramePreview::update(const std::uint8_t* bgr24, int width, int height, int stride) {
    if (!bgr24 || width <= 0 || height <= 0)
        return;
    const int row = stride > 0 ? stride : width * 3;
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->bgr.assign(bgr24, bgr24 + static_cast<std::size_t>(row) * static_cast<std::size_t>(height));
        impl_->bgr_w = width;
        impl_->bgr_h = height;
        impl_->bgr_stride = row;
    }
    HWND h = impl_->hwnd;
    if (h)
        ::InvalidateRect(h, nullptr, FALSE);
}

bool FramePreview::is_open() const noexcept { return impl_->running; }

void FramePreview::close() {
    if (!impl_->running && !impl_->ui.joinable())
        return;
    impl_->running = false;
    HWND h = impl_->hwnd;
    if (h)
        ::PostMessageW(h, WM_CLOSE, 0, 0);
    if (impl_->ui.joinable())
        impl_->ui.join();
}

} // namespace pm::video

#endif
