#include "pm/video/zoom.hpp"

#if defined(_WIN32)

#include "pm/video/hooks.hpp"
#include "pm/video/win_dpi.hpp"

#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <iterator>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <utility>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "Gdiplus.lib")

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#ifndef LLMHF_INJECTED
#define LLMHF_INJECTED 0x00000001
#endif

namespace pm::video {
namespace {

constexpr UINT kRefreshMessage = WM_APP + 0x421;
constexpr UINT kWheelMessage = WM_APP + 0x422;
constexpr UINT kStopMessage = WM_APP + 0x423;
constexpr UINT kPauseMessage = WM_APP + 0x424;
constexpr UINT kKeyOverlayMessage = WM_APP + 0x425;
constexpr UINT_PTR kFadeTimer = 1;
constexpr UINT kFadeTimerMs = 33;
constexpr wchar_t kHudClassName[] = L"PmInteractiveRecordingZoomHud";
constexpr wchar_t kViewportClassName[] = L"PmInteractiveRecordingZoomViewport";
constexpr wchar_t kKeysClassName[] = L"PmInteractiveRecordingKeyOverlay";
constexpr wchar_t kCursorClassName[] = L"PmInteractiveRecordingCursorFx";
constexpr COLORREF kTransparentKey = RGB(1, 2, 3);
constexpr UINT kCursorClickMessage = WM_APP + 0x426;
constexpr UINT kStopKeyMessage = WM_APP + 0x427;
constexpr unsigned kClickRippleMs = 420;
constexpr unsigned kTypingHideMs = 900;
constexpr unsigned kTrailMs = 480;          // how long the comet persists
constexpr unsigned kTrailTimerMs = 16;      // paint/age cadence while trailing
constexpr int kTrailMinDistPx = 3;         // skip sub-pixel jitter duplicates
constexpr std::size_t kTrailMaxPoints = 64;

COLORREF rgb_to_colorref(std::uint32_t rgb) noexcept
{
    return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

Gdiplus::Color argb_from_rgb(std::uint32_t rgb, BYTE a = 255) noexcept
{
    return Gdiplus::Color(
        a,
        static_cast<BYTE>((rgb >> 16) & 0xFF),
        static_cast<BYTE>((rgb >> 8) & 0xFF),
        static_cast<BYTE>(rgb & 0xFF));
}

// 32-bit top-down DIB for UpdateLayeredWindow(ULW_ALPHA) — same pattern as
// win_camera_overlay_controller (antialiased GDI+ into a premultiplied layer).
struct CursorFxLayerBuf {
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

bool is_modifier_vk(DWORD vk) noexcept
{
    return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
           vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
           vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
           vk == VK_LWIN || vk == VK_RWIN || vk == VK_CAPITAL ||
           vk == VK_NUMLOCK || vk == VK_SCROLL;
}

unsigned current_modifier_mask() noexcept
{
    unsigned mods = 0;
    if ((::GetAsyncKeyState(VK_CONTROL) & 0x8000) ||
        (::GetAsyncKeyState(VK_LCONTROL) & 0x8000) ||
        (::GetAsyncKeyState(VK_RCONTROL) & 0x8000))
        mods |= MOD_CONTROL;
    if ((::GetAsyncKeyState(VK_MENU) & 0x8000) ||
        (::GetAsyncKeyState(VK_LMENU) & 0x8000) ||
        (::GetAsyncKeyState(VK_RMENU) & 0x8000))
        mods |= MOD_ALT;
    if ((::GetAsyncKeyState(VK_SHIFT) & 0x8000) ||
        (::GetAsyncKeyState(VK_LSHIFT) & 0x8000) ||
        (::GetAsyncKeyState(VK_RSHIFT) & 0x8000))
        mods |= MOD_SHIFT;
    if ((::GetAsyncKeyState(VK_LWIN) & 0x8000) ||
        (::GetAsyncKeyState(VK_RWIN) & 0x8000))
        mods |= MOD_WIN;
    return mods;
}

bool key_passes_filter(KeyOverlayFilter filter, unsigned mods, DWORD vk) noexcept
{
    if (is_modifier_vk(vk)) return false;
    switch (filter) {
    case KeyOverlayFilter::shortcuts:
        return mods != 0;
    case KeyOverlayFilter::printable:
        return (vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9') ||
               vk == VK_SPACE || (vk >= VK_OEM_1 && vk <= VK_OEM_8) ||
               (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE);
    case KeyOverlayFilter::all:
        return true;
    }
    return false;
}

std::wstring utf8_to_wide_local(const std::string& s)
{
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

bool valid_rect(const RECT& rect) noexcept
{
    return rect.right > rect.left && rect.bottom > rect.top;
}

template <typename T>
T clamp_value(T value, T low, T high) noexcept
{
    return std::max(low, std::min(value, high));
}

BYTE fade_alpha(ULONGLONG elapsed, unsigned int hold_ms,
                unsigned int fade_ms) noexcept
{
    if (elapsed <= hold_ms)
        return 255;
    if (fade_ms == 0 || elapsed >= static_cast<ULONGLONG>(hold_ms) + fade_ms)
        return 0;

    const auto fade_elapsed = elapsed - hold_ms;
    return static_cast<BYTE>(
        255u - (255u * fade_elapsed / std::max(1u, fade_ms)));
}

const wchar_t* follow_mode_wname(InteractiveFollowMode mode) noexcept
{
    switch (mode) {
    case InteractiveFollowMode::none: return L"NONE";
    case InteractiveFollowMode::anchor: return L"ANCHOR";
    case InteractiveFollowMode::cursor: return L"CURSOR";
    }
    return L"UNKNOWN";
}

} // namespace

struct WinInteractiveZoomController::Impl {
    struct WheelEvent {
        int steps = 0;
        POINT cursor{};
    };

    enum class OverlayRole { viewport, hud, keys, cursor_fx };

    struct WindowContext {
        Impl* owner = nullptr;
        OverlayRole role = OverlayRole::viewport;
    };

    struct KeyChip {
        std::wstring label;
        ULONGLONG shown_at = 0;
    };

    struct ClickRipple {
        POINT pt{};
        ULONGLONG shown_at = 0;
        CursorClickButton button = CursorClickButton::left;
    };

    struct TrailPoint {
        POINT pt{};
        ULONGLONG at = 0;
    };

    struct ClickEvent {
        POINT pt{};
        CursorClickButton button = CursorClickButton::left;
    };

    static constexpr std::size_t kWheelQueueSize = 64;
    static constexpr std::size_t kClickQueueSize = 32;
    static std::atomic<Impl*> active_hook_owner;

    std::mutex api_mutex;
    std::mutex state_mutex;
    std::mutex keys_mutex;
    std::mutex init_mutex;
    std::condition_variable init_cv;
    std::thread ui_thread;

    InteractiveZoomOptions options;
    InteractiveZoomState state;
    InteractiveZoomWheelCallback callback;
    InteractivePauseCallback pause_callback;
    InteractiveStopCallback stop_callback;
    InteractiveInputActivityCallback input_activity;

    std::atomic<bool> consume_input{false};
    std::atomic<bool> is_running{false};
    std::atomic<bool> refresh_pending{false};
    std::atomic<bool> refresh_reveal_hud{false};
    std::atomic<DWORD> ui_thread_id{0};

    bool init_done = false;
    bool init_ok = false;
    std::string init_error;

    HHOOK mouse_hook = nullptr;
    HHOOK keyboard_hook = nullptr;
    HWND hud_window = nullptr;
    HWND viewport_window = nullptr;
    HWND keys_window = nullptr;
    HWND cursor_window = nullptr;
    WindowContext hud_context{this, OverlayRole::hud};
    WindowContext viewport_context{this, OverlayRole::viewport};
    WindowContext keys_context{this, OverlayRole::keys};
    WindowContext cursor_context{this, OverlayRole::cursor_fx};
    ULONGLONG hud_changed_at = 0;
    BYTE last_hud_alpha = 0;
    bool viewport_applied = false;
    bool viewport_visible = false;
    RECT viewport_rect{};
    bool hud_state_applied = false;
    InteractiveZoomState hud_state{};
    int hud_x = 0;
    int hud_y = 0;
    int wheel_remainder = 0;
    bool pause_key_down = false;
    bool stop_key_down = false;
    std::deque<KeyChip> key_chips;
    std::unordered_set<DWORD> keys_down;
    int keys_x = 0;
    int keys_y = 0;
    int keys_w = 0;
    int keys_h = 0;
    int keys_cached_chip_w = 0;
    int keys_cached_content_h = 0;
    int keys_cached_chip_count = 0;
    int keys_layout_font_size = 0;
    ULONGLONG keys_last_reposition_ms = 0;

    std::mutex cursor_mutex;
    std::deque<ClickRipple> click_ripples;
    std::deque<TrailPoint> cursor_trail;
    int cursor_x = 0;
    int cursor_y = 0;
    int cursor_w = 0;
    int cursor_h = 0;
    POINT cursor_last_pt{};
    bool cursor_last_pt_valid = false;
    std::atomic<ULONGLONG> typing_hidden_until{0};
    bool cursor_window_shown = false;
    CursorFxLayerBuf cursor_layer{};
    ULONG_PTR cursor_gdip_token = 0;

    std::array<WheelEvent, kWheelQueueSize> wheel_queue{};
    std::atomic<std::size_t> wheel_write{0};
    std::atomic<std::size_t> wheel_read{0};

    std::array<ClickEvent, kClickQueueSize> click_queue{};
    std::atomic<std::size_t> click_write{0};
    std::atomic<std::size_t> click_read{0};

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message,
                                        WPARAM wparam, LPARAM lparam)
    {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            ::SetWindowLongPtrW(
                hwnd, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }

        auto* context = reinterpret_cast<WindowContext*>(
            ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        Impl* self = context ? context->owner : nullptr;

        switch (message) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER:
            if (self && wparam == kFadeTimer) {
                if (context->role == OverlayRole::hud)
                    self->update_hud_alpha();
                if (context->role == OverlayRole::keys ||
                    self->options.key_overlay.enabled)
                    self->update_keys_overlay();
                if (self->options.cursor_fx.any())
                    self->update_cursor_fx();
                return 0;
            }
            break;
        case WM_PAINT:
            if (self) {
                if (context->role == OverlayRole::hud)
                    self->paint_hud(hwnd);
                else if (context->role == OverlayRole::keys)
                    self->paint_keys(hwnd);
                else if (context->role == OverlayRole::cursor_fx)
                    self->paint_cursor_fx(hwnd);
                else
                    self->paint_viewport(hwnd);
                return 0;
            }
            break;
        default:
            break;
        }
        return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }

    static bool register_window_class(const wchar_t* name)
    {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = &Impl::window_proc;
        window_class.hInstance = ::GetModuleHandleW(nullptr);
        window_class.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        window_class.hbrBackground = nullptr;
        window_class.lpszClassName = name;
        if (::RegisterClassExW(&window_class))
            return true;
        return ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    static LRESULT CALLBACK mouse_hook_proc(int code, WPARAM wparam,
                                             LPARAM lparam)
    {
        Impl* self = active_hook_owner.load(std::memory_order_acquire);
        if (code == HC_ACTION && self) {
            const auto* mouse = reinterpret_cast<MSLLHOOKSTRUCT*>(lparam);
            if (!(mouse->flags & LLMHF_INJECTED)) {
                if (self->input_activity) {
                    IdleInputKind kind = IdleInputKind::move;
                    if (wparam == WM_LBUTTONDOWN || wparam == WM_RBUTTONDOWN ||
                        wparam == WM_MBUTTONDOWN || wparam == WM_XBUTTONDOWN ||
                        wparam == WM_MOUSEWHEEL || wparam == WM_MOUSEHWHEEL)
                        kind = IdleInputKind::click;
                    else if (wparam != WM_MOUSEMOVE)
                        kind = IdleInputKind::move;
                    try {
                        self->input_activity(kind, mouse->pt);
                    } catch (...) {
                    }
                }
                if (self->options.cursor_fx.trail &&
                    (wparam == WM_MOUSEMOVE || wparam == WM_LBUTTONDOWN ||
                     wparam == WM_RBUTTONDOWN || wparam == WM_MBUTTONDOWN ||
                     wparam == WM_XBUTTONDOWN)) {
                    self->push_trail_sample(mouse->pt);
                }
                if (self->options.cursor_fx.clicks &&
                    (wparam == WM_LBUTTONDOWN || wparam == WM_RBUTTONDOWN ||
                     wparam == WM_MBUTTONDOWN || wparam == WM_XBUTTONDOWN)) {
                    CursorClickButton button = CursorClickButton::other;
                    if (wparam == WM_LBUTTONDOWN)
                        button = CursorClickButton::left;
                    else if (wparam == WM_RBUTTONDOWN)
                        button = CursorClickButton::right;
                    else if (wparam == WM_MBUTTONDOWN)
                        button = CursorClickButton::middle;
                    self->enqueue_click(mouse->pt, button);
                }
            }
            if (self->options.enable_wheel_input &&
                wparam == WM_MOUSEWHEEL &&
                (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) {
                const auto delta =
                    static_cast<short>(HIWORD(mouse->mouseData));
                self->wheel_remainder += delta;
                const int steps = self->wheel_remainder / WHEEL_DELTA;
                self->wheel_remainder %= WHEEL_DELTA;

                if (steps != 0)
                    self->enqueue_wheel(steps, mouse->pt);

                if (self->consume_input.load(std::memory_order_relaxed))
                    return 1;
            }
        }
        return ::CallNextHookEx(nullptr, code, wparam, lparam);
    }

    static bool modifiers_down(unsigned int modifiers) noexcept
    {
        const auto down = [](int key) {
            return (::GetAsyncKeyState(key) & 0x8000) != 0;
        };
        if ((modifiers & MOD_CONTROL) &&
            !(down(VK_CONTROL) || down(VK_LCONTROL) ||
              down(VK_RCONTROL))) return false;
        if ((modifiers & MOD_ALT) &&
            !(down(VK_MENU) || down(VK_LMENU) ||
              down(VK_RMENU))) return false;
        if ((modifiers & MOD_SHIFT) &&
            !(down(VK_SHIFT) || down(VK_LSHIFT) ||
              down(VK_RSHIFT))) return false;
        if ((modifiers & MOD_WIN) &&
            !(down(VK_LWIN) || down(VK_RWIN))) return false;
        return true;
    }

    static LRESULT CALLBACK keyboard_hook_proc(
        int code, WPARAM wparam, LPARAM lparam)
    {
        Impl* self =
            active_hook_owner.load(std::memory_order_acquire);
        if (code == HC_ACTION && self) {
            const auto* key =
                reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
            const bool is_down =
                wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
            const bool is_up =
                wparam == WM_KEYUP || wparam == WM_SYSKEYUP;

            if (self->options.stop_virtual_key != 0 &&
                key->vkCode == self->options.stop_virtual_key) {
                bool handled = false;
                if (is_down && !self->stop_key_down &&
                    modifiers_down(
                        self->options.stop_modifiers)) {
                    self->stop_key_down = true;
                    handled = true;
                    const DWORD thread_id = self->ui_thread_id.load(
                        std::memory_order_relaxed);
                    if (thread_id)
                        ::PostThreadMessageW(
                            thread_id, kStopKeyMessage, 0, 0);
                } else if (is_up && self->stop_key_down) {
                    self->stop_key_down = false;
                    handled = true;
                }
                if (handled &&
                    self->options.consume_stop_input)
                    return 1;
            }

            if (self->options.pause_virtual_key != 0 &&
                key->vkCode == self->options.pause_virtual_key) {
                bool handled = false;
                if (is_down && !self->pause_key_down &&
                    modifiers_down(
                        self->options.pause_modifiers)) {
                    self->pause_key_down = true;
                    handled = true;
                    const DWORD thread_id = self->ui_thread_id.load(
                        std::memory_order_relaxed);
                    if (thread_id)
                        ::PostThreadMessageW(
                            thread_id, kPauseMessage, 0, 0);
                } else if (is_up && self->pause_key_down) {
                    self->pause_key_down = false;
                    handled = true;
                }
                if (handled &&
                    self->options.consume_pause_input)
                    return 1;
            }

            if (!(key->flags & LLKHF_INJECTED) &&
                !is_modifier_vk(key->vkCode)) {
                if (is_down && self->input_activity &&
                    key->vkCode != self->options.pause_virtual_key &&
                    key->vkCode != self->options.stop_virtual_key) {
                    POINT pt{};
                    ::GetCursorPos(&pt);
                    try {
                        self->input_activity(IdleInputKind::key, pt);
                    } catch (...) {
                    }
                }
                if (is_down && self->options.cursor_fx.hide_when_typing) {
                    self->typing_hidden_until.store(
                        ::GetTickCount64() + kTypingHideMs,
                        std::memory_order_relaxed);
                }
                if (self->options.key_overlay.enabled) {
                    if (is_down) {
                        if (self->keys_down.insert(key->vkCode).second) {
                            const unsigned mods = current_modifier_mask();
                            if (key_passes_filter(
                                    self->options.key_overlay.filter,
                                    mods, key->vkCode)) {
                                const DWORD thread_id =
                                    self->ui_thread_id.load(
                                        std::memory_order_relaxed);
                                if (thread_id)
                                    ::PostThreadMessageW(
                                        thread_id, kKeyOverlayMessage,
                                        static_cast<WPARAM>(mods),
                                        static_cast<LPARAM>(key->vkCode));
                            }
                        }
                    } else if (is_up) {
                        self->keys_down.erase(key->vkCode);
                    }
                }
            }
        }
        return ::CallNextHookEx(nullptr, code, wparam, lparam);
    }

    void enqueue_wheel(int steps, POINT cursor) noexcept
    {
        const std::size_t write = wheel_write.load(std::memory_order_relaxed);
        const std::size_t next = (write + 1) % kWheelQueueSize;
        if (next == wheel_read.load(std::memory_order_acquire))
            return;

        wheel_queue[write] = WheelEvent{steps, cursor};
        wheel_write.store(next, std::memory_order_release);
        const DWORD thread_id = ui_thread_id.load(std::memory_order_relaxed);
        if (thread_id)
            ::PostThreadMessageW(thread_id, kWheelMessage, 0, 0);
    }

    void drain_wheel_events()
    {
        for (;;) {
            const std::size_t read = wheel_read.load(std::memory_order_relaxed);
            if (read == wheel_write.load(std::memory_order_acquire))
                break;

            const WheelEvent event = wheel_queue[read];
            wheel_read.store((read + 1) % kWheelQueueSize,
                             std::memory_order_release);
            if (callback) {
                try {
                    callback(event.steps, event.cursor);
                } catch (...) {
                    // A client exception must not terminate the hook/message thread.
                }
            }
        }
    }

    void enqueue_click(POINT pt, CursorClickButton button) noexcept
    {
        const std::size_t write = click_write.load(std::memory_order_relaxed);
        const std::size_t next = (write + 1) % kClickQueueSize;
        if (next == click_read.load(std::memory_order_acquire))
            return;
        click_queue[write] = ClickEvent{pt, button};
        click_write.store(next, std::memory_order_release);
        const DWORD thread_id = ui_thread_id.load(std::memory_order_relaxed);
        if (thread_id)
            ::PostThreadMessageW(thread_id, kCursorClickMessage, 0, 0);
    }

    void drain_click_events()
    {
        for (;;) {
            const std::size_t read = click_read.load(std::memory_order_relaxed);
            if (read == click_write.load(std::memory_order_acquire))
                break;
            const ClickEvent event = click_queue[read];
            click_read.store((read + 1) % kClickQueueSize,
                             std::memory_order_release);
            push_click_ripple(event.pt, event.button);
        }
    }

    bool create_overlay(HWND& window, WindowContext* context,
                        const wchar_t* class_name, std::string& error)
    {
        window = ::CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
                WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            class_name, L"", WS_POPUP,
            0, 0, 1, 1, nullptr, nullptr, ::GetModuleHandleW(nullptr), context);
        if (!window) {
            error = "CreateWindowExW failed (" +
                    std::to_string(::GetLastError()) + ")";
            return false;
        }
        return true;
    }

    bool exclude_overlay_from_capture(
        HWND& window, const char* label, std::string& warning)
    {
        // SetWindowDisplayAffinity can reject a newly-created hidden layered
        // window. Realize it first, apply exclusion, then hide until state is
        // positioned. Never show an unexcluded HUD during monitor capture.
        ::ShowWindow(window, SW_SHOWNOACTIVATE);
        ::UpdateWindow(window);
        if (::SetWindowDisplayAffinity(
                window, WDA_EXCLUDEFROMCAPTURE)) {
            ::ShowWindow(window, SW_HIDE);
            return true;
        }
        const DWORD code = ::GetLastError();
        ::ShowWindow(window, SW_HIDE);
        ::DestroyWindow(window);
        window = nullptr;
        if (!warning.empty()) warning += "; ";
        warning += std::string(label) +
            " hidden because Windows capture exclusion failed (" +
            std::to_string(code) + ")";
        return false;
    }

    bool initialize(std::string& error)
    {
        if (!register_window_class(kHudClassName) ||
            !register_window_class(kViewportClassName) ||
            !register_window_class(kKeysClassName) ||
            !register_window_class(kCursorClassName)) {
            error = "failed to register interactive zoom window classes";
            return false;
        }

        if (options.show_hud) {
            if (!create_overlay(hud_window, &hud_context, kHudClassName, error))
                return false;
            ::SetLayeredWindowAttributes(
                hud_window, kTransparentKey, 255, LWA_COLORKEY | LWA_ALPHA);
            ::SetTimer(hud_window, kFadeTimer, kFadeTimerMs, nullptr);
            if (!options.overlays_visible_to_capture)
                (void)exclude_overlay_from_capture(
                    hud_window, "zoom HUD", error);
        }

        if (options.show_viewport_border) {
            if (!create_overlay(viewport_window, &viewport_context,
                                kViewportClassName, error))
                return false;
            ::SetLayeredWindowAttributes(viewport_window, 0, 230, LWA_ALPHA);
            if (!options.overlays_visible_to_capture)
                (void)exclude_overlay_from_capture(
                    viewport_window, "viewport border", error);
        }

        if (options.key_overlay.enabled) {
            if (!create_overlay(keys_window, &keys_context, kKeysClassName, error))
                return false;
            const BYTE alpha = static_cast<BYTE>(std::clamp(
                options.key_overlay.opacity, 0.0f, 1.0f) * 255.0f);
            ::SetLayeredWindowAttributes(
                keys_window, kTransparentKey, alpha, LWA_COLORKEY | LWA_ALPHA);
            if (!hud_window)
                ::SetTimer(keys_window, kFadeTimer, kFadeTimerMs, nullptr);
            bool exclude_keys = !options.overlays_visible_to_capture;
            if (options.key_overlay.capture == "visible")
                exclude_keys = false;
            else if (options.key_overlay.capture == "exclude" ||
                     options.key_overlay.capture == "auto")
                exclude_keys = true;
            // inherit → use overlays_visible_to_capture above
            if (exclude_keys)
                (void)exclude_overlay_from_capture(
                    keys_window, "key overlay", error);
        }

        if (options.cursor_fx.any()) {
            Gdiplus::GdiplusStartupInput gdiin;
            if (Gdiplus::GdiplusStartup(
                    &cursor_gdip_token, &gdiin, nullptr) != Gdiplus::Ok) {
                error = "GdiplusStartup failed (cursor FX)";
                return false;
            }
            if (!create_overlay(
                    cursor_window, &cursor_context, kCursorClassName, error))
                return false;
            // Per-pixel alpha via UpdateLayeredWindow — do not set color-key
            // attributes (they force aliased GDI edges and fight ULW_ALPHA).
            // Always tick on the cursor window so trail/highlight keep updating
            // even when the zoom HUD owns the shared fade timer.
            const UINT trail_ms = options.cursor_fx.trail
                ? kTrailTimerMs : kFadeTimerMs;
            ::SetTimer(cursor_window, kFadeTimer, trail_ms, nullptr);
            // Always capture-visible so tutorial FX lands in the MP4 even when
            // the zoom HUD uses WDA_EXCLUDEFROMCAPTURE.
        }

        Impl* expected = nullptr;
        if (!active_hook_owner.compare_exchange_strong(
                expected, this, std::memory_order_acq_rel)) {
            error = "another recording input controller is already started";
            return false;
        }

        if (options.enable_wheel_input || options.cursor_fx.clicks ||
            options.cursor_fx.trail || options.track_input_activity) {
            mouse_hook = ::SetWindowsHookExW(
                WH_MOUSE_LL, &Impl::mouse_hook_proc,
                ::GetModuleHandleW(nullptr), 0);
            if (!mouse_hook) {
                active_hook_owner.store(nullptr, std::memory_order_release);
                error = "SetWindowsHookExW(WH_MOUSE_LL) failed (" +
                        std::to_string(::GetLastError()) + ")";
                return false;
            }
        }
        if (options.pause_virtual_key != 0 ||
            options.stop_virtual_key != 0 ||
            options.key_overlay.enabled ||
            options.cursor_fx.hide_when_typing ||
            options.track_input_activity) {
            keyboard_hook = ::SetWindowsHookExW(
                WH_KEYBOARD_LL, &Impl::keyboard_hook_proc,
                ::GetModuleHandleW(nullptr), 0);
            if (!keyboard_hook) {
                error = "SetWindowsHookExW(WH_KEYBOARD_LL) failed (" +
                        std::to_string(::GetLastError()) + ")";
                return false;
            }
        }
        return true;
    }

    void destroy_thread_resources() noexcept
    {
        if (keyboard_hook) {
            ::UnhookWindowsHookEx(keyboard_hook);
            keyboard_hook = nullptr;
        }
        if (mouse_hook) {
            ::UnhookWindowsHookEx(mouse_hook);
            mouse_hook = nullptr;
        }
        Impl* expected = this;
        active_hook_owner.compare_exchange_strong(
            expected, nullptr, std::memory_order_acq_rel);

        if (hud_window) {
            ::KillTimer(hud_window, kFadeTimer);
            ::DestroyWindow(hud_window);
            hud_window = nullptr;
        }
        if (keys_window) {
            ::KillTimer(keys_window, kFadeTimer);
            ::DestroyWindow(keys_window);
            keys_window = nullptr;
        }
        if (cursor_window) {
            ::KillTimer(cursor_window, kFadeTimer);
            ::DestroyWindow(cursor_window);
            cursor_window = nullptr;
        }
        cursor_layer.free();
        if (cursor_gdip_token) {
            Gdiplus::GdiplusShutdown(cursor_gdip_token);
            cursor_gdip_token = 0;
        }
        if (viewport_window) {
            ::DestroyWindow(viewport_window);
            viewport_window = nullptr;
        }
        keys_down.clear();
        key_chips.clear();
        {
            std::lock_guard<std::mutex> lock(cursor_mutex);
            click_ripples.clear();
            cursor_trail.clear();
        }
        cursor_window_shown = false;
        cursor_last_pt_valid = false;
    }

    void thread_main()
    {
        ui_thread_id.store(::GetCurrentThreadId(), std::memory_order_release);

        // Force creation of the thread message queue before start() can post.
        MSG message{};
        ::PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

        std::string error;
        const bool initialized = initialize(error);
        if (initialized) {
            apply_state(true);
            is_running.store(true, std::memory_order_release);
        } else {
            destroy_thread_resources();
        }

        {
            std::lock_guard<std::mutex> lock(init_mutex);
            init_ok = initialized;
            init_error = std::move(error);
            init_done = true;
        }
        init_cv.notify_all();

        if (initialized) {
            bool stopping = false;
            while (!stopping && ::GetMessageW(&message, nullptr, 0, 0) > 0) {
                switch (message.message) {
                case kRefreshMessage:
                    refresh_pending.store(false, std::memory_order_release);
                    apply_state(
                        refresh_reveal_hud.exchange(
                            false, std::memory_order_acq_rel));
                    break;
                case kWheelMessage:
                    drain_wheel_events();
                    break;
                case kPauseMessage:
                    if (pause_callback) {
                        try {
                            pause_callback();
                        } catch (...) {
                            // A client exception must not terminate controls.
                        }
                    }
                    break;
                case kStopKeyMessage:
                    if (stop_callback) {
                        try {
                            stop_callback();
                        } catch (...) {
                            // A client exception must not terminate controls.
                        }
                    }
                    break;
                case kKeyOverlayMessage:
                    push_key_chip(static_cast<unsigned>(message.wParam),
                                 static_cast<unsigned>(message.lParam));
                    break;
                case kCursorClickMessage:
                    drain_click_events();
                    break;
                case kStopMessage:
                    stopping = true;
                    break;
                default:
                    ::TranslateMessage(&message);
                    ::DispatchMessageW(&message);
                    break;
                }
            }
        }

        is_running.store(false, std::memory_order_release);
        destroy_thread_resources();
        ui_thread_id.store(0, std::memory_order_release);
    }

    void apply_state(bool reveal_hud)
    {
        InteractiveZoomState snapshot;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            snapshot = state;
        }

        if (viewport_window) {
            if (snapshot.viewport_visible && valid_rect(snapshot.viewport_screen)) {
                const RECT& rect = snapshot.viewport_screen;
                const int width = rect.right - rect.left;
                const int height = rect.bottom - rect.top;
                const bool size_changed =
                    !viewport_applied ||
                    viewport_rect.right - viewport_rect.left != width ||
                    viewport_rect.bottom - viewport_rect.top != height;
                const bool position_changed =
                    !viewport_applied || !viewport_visible ||
                    !::EqualRect(&viewport_rect, &rect);
                if (size_changed) {
                    const int thickness = clamp_value(
                        options.viewport_thickness, 1,
                        std::max(1, std::min(width, height) / 2));

                    HRGN outer = ::CreateRectRgn(0, 0, width, height);
                    HRGN inner = ::CreateRectRgn(
                        thickness, thickness,
                        std::max(thickness, width - thickness),
                        std::max(thickness, height - thickness));
                    ::CombineRgn(outer, outer, inner, RGN_DIFF);
                    ::DeleteObject(inner);
                    ::SetWindowRgn(viewport_window, outer, FALSE);
                }
                if (position_changed) {
                    ::SetWindowPos(
                        viewport_window, HWND_TOPMOST, rect.left, rect.top,
                        width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
                }
                if (size_changed)
                    ::InvalidateRect(viewport_window, nullptr, FALSE);
                if (position_changed)
                    viewport_rect = rect;
                viewport_applied = true;
                viewport_visible = true;
            } else if (!viewport_applied || viewport_visible) {
                ::ShowWindow(viewport_window, SW_HIDE);
                viewport_applied = true;
                viewport_visible = false;
            }
        }

        if (hud_window) {
            const bool content_changed =
                !hud_state_applied ||
                hud_state.recording != snapshot.recording ||
                hud_state.paused != snapshot.paused ||
                hud_state.zoom != snapshot.zoom ||
                hud_state.follow_mode != snapshot.follow_mode ||
                hud_state.output_width != snapshot.output_width ||
                hud_state.output_height != snapshot.output_height;
            if (reveal_hud || ::IsWindowVisible(hud_window)) {
                position_hud(snapshot);
                if (reveal_hud || content_changed)
                    ::InvalidateRect(hud_window, nullptr, FALSE);
            }
            hud_state = snapshot;
            hud_state_applied = true;
            if (reveal_hud) {
                hud_changed_at = ::GetTickCount64();
                last_hud_alpha = 255;
                ::SetLayeredWindowAttributes(
                    hud_window, kTransparentKey, 255,
                    LWA_COLORKEY | LWA_ALPHA);
                ::ShowWindow(hud_window, SW_SHOWNOACTIVATE);
            }
        }

        // Cheap follow/zoom tracking: move only, never remasure fonts here.
        if (keys_window && options.key_overlay.enabled)
            reposition_keys_for_viewport(/*force*/ false);
        if (cursor_window && options.cursor_fx.any())
            update_cursor_fx();
    }

    void position_hud(const InteractiveZoomState& snapshot)
    {
        constexpr int width = 290;
        constexpr int height = 56;
        constexpr int gap = 8;
        const int virtual_left = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int virtual_top = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int virtual_right =
            virtual_left + ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int virtual_bottom =
            virtual_top + ::GetSystemMetrics(SM_CYVIRTUALSCREEN);

        int x = virtual_left + 12;
        int y = virtual_bottom - height - 12;
        if (snapshot.viewport_visible && valid_rect(snapshot.viewport_screen)) {
            x = snapshot.viewport_screen.left;
            y = snapshot.viewport_screen.bottom + gap;
            if (y + height > virtual_bottom)
                y = snapshot.viewport_screen.top - height - gap;
        }
        x = clamp_value(x, virtual_left, std::max(virtual_left, virtual_right - width));
        y = clamp_value(y, virtual_top, std::max(virtual_top, virtual_bottom - height));
        if (!hud_state_applied || x != hud_x || y != hud_y) {
            ::SetWindowPos(hud_window, HWND_TOPMOST, x, y, width, height,
                           SWP_NOACTIVATE);
            hud_x = x;
            hud_y = y;
        }
    }

    void update_hud_alpha()
    {
        if (!hud_window || hud_changed_at == 0)
            return;
        const BYTE alpha = fade_alpha(
            ::GetTickCount64() - hud_changed_at,
            options.hud_hold_ms, options.hud_fade_ms);
        if (alpha == last_hud_alpha)
            return;

        last_hud_alpha = alpha;
        if (alpha == 0) {
            ::ShowWindow(hud_window, SW_HIDE);
        } else {
            ::SetLayeredWindowAttributes(
                hud_window, kTransparentKey, alpha,
                LWA_COLORKEY | LWA_ALPHA);
        }
    }

    void paint_hud(HWND window)
    {
        InteractiveZoomState snapshot;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            snapshot = state;
        }

        PAINTSTRUCT paint{};
        HDC dc = ::BeginPaint(window, &paint);
        RECT client{};
        ::GetClientRect(window, &client);
        HBRUSH transparent = ::CreateSolidBrush(kTransparentKey);
        ::FillRect(dc, &client, transparent);
        ::DeleteObject(transparent);

        RECT panel{0, 0, client.right, client.bottom};
        HBRUSH background = ::CreateSolidBrush(RGB(24, 27, 33));
        ::FillRect(dc, &panel, background);
        ::DeleteObject(background);

        RECT accent{0, 0, 4, client.bottom};
        HBRUSH accent_brush = ::CreateSolidBrush(options.accent_color);
        ::FillRect(dc, &accent, accent_brush);
        ::DeleteObject(accent_brush);

        wchar_t primary[128]{};
        wchar_t secondary[128]{};
        std::swprintf(primary, std::size(primary), L"%s  %.2fx  %s",
                      snapshot.paused
                          ? L"PAUSED"
                          : (snapshot.recording ? L"REC" : L"READY"),
                      std::max(0.0, snapshot.zoom),
                      follow_mode_wname(snapshot.follow_mode));
        if (snapshot.output_width > 0 && snapshot.output_height > 0) {
            std::swprintf(secondary, std::size(secondary), L"%d x %d",
                          snapshot.output_width, snapshot.output_height);
        } else {
            std::swprintf(secondary, std::size(secondary), L"NATIVE");
        }

        ::SetBkMode(dc, TRANSPARENT);
        ::SelectObject(dc, ::GetStockObject(DEFAULT_GUI_FONT));
        ::SetTextColor(dc, RGB(248, 249, 251));
        RECT primary_rect{16, 6, client.right - 10, 30};
        ::DrawTextW(dc, primary, -1, &primary_rect,
                    DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        ::SetTextColor(dc, RGB(174, 181, 194));
        RECT secondary_rect{16, 30, client.right - 10, 51};
        ::DrawTextW(dc, secondary, -1, &secondary_rect,
                    DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        ::EndPaint(window, &paint);
    }

    void paint_viewport(HWND window)
    {
        PAINTSTRUCT paint{};
        HDC dc = ::BeginPaint(window, &paint);
        RECT client{};
        ::GetClientRect(window, &client);
        HBRUSH brush = ::CreateSolidBrush(options.viewport_color);
        ::FillRect(dc, &client, brush);
        ::DeleteObject(brush);
        ::EndPaint(window, &paint);
    }

    void push_key_chip(unsigned mods, unsigned vk)
    {
        if (!keys_window || !options.key_overlay.enabled) return;
        KeyChip chip;
        chip.label = utf8_to_wide_local(pm::video::key_label(
            mods, vk, options.key_overlay.show_modifiers));
        chip.shown_at = ::GetTickCount64();
        {
            std::lock_guard<std::mutex> lock(keys_mutex);
            if (!key_chips.empty() && key_chips.back().label == chip.label &&
                chip.shown_at - key_chips.back().shown_at < 250)
                key_chips.back().shown_at = chip.shown_at;
            else
                key_chips.push_back(std::move(chip));
            const int max_n = std::max(1, options.key_overlay.max_entries);
            while (static_cast<int>(key_chips.size()) > max_n)
                key_chips.pop_front();
        }
        layout_and_show_keys();
        ::InvalidateRect(keys_window, nullptr, FALSE);
    }

    void update_keys_overlay()
    {
        if (!keys_window || !options.key_overlay.enabled) return;
        const ULONGLONG now = ::GetTickCount64();
        const unsigned hold = static_cast<unsigned>(
            std::max(100, options.key_overlay.hold_ms));
        const unsigned fade = static_cast<unsigned>(
            std::max(0, options.key_overlay.fade_ms));
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(keys_mutex);
            while (!key_chips.empty()) {
                const auto age = now - key_chips.front().shown_at;
                if (age >= static_cast<ULONGLONG>(hold) + fade) {
                    key_chips.pop_front();
                    changed = true;
                } else {
                    break;
                }
            }
        }
        if (changed) {
            layout_and_show_keys();
            ::InvalidateRect(keys_window, nullptr, FALSE);
        } else if (::IsWindowVisible(keys_window)) {
            // Drive per-chip fade via layered alpha of the whole window from the
            // oldest chip — fine for a small stack.
            ULONGLONG oldest = 0;
            {
                std::lock_guard<std::mutex> lock(keys_mutex);
                if (!key_chips.empty())
                    oldest = key_chips.front().shown_at;
            }
            if (oldest) {
                const BYTE alpha = fade_alpha(now - oldest, hold, fade);
                const BYTE base = static_cast<BYTE>(std::clamp(
                    options.key_overlay.opacity, 0.0f, 1.0f) * 255.0f);
                const BYTE out = static_cast<BYTE>(
                    (static_cast<unsigned>(alpha) * base) / 255u);
                ::SetLayeredWindowAttributes(
                    keys_window, kTransparentKey, out,
                    LWA_COLORKEY | LWA_ALPHA);
                if (out == 0)
                    ::ShowWindow(keys_window, SW_HIDE);
            }
        }
    }

    // Prefer the active recording viewport; else the monitor under the cursor.
    // Never use the full virtual desktop — center align lands between monitors.
    void keys_anchor_bounds(int& left, int& top, int& width, int& height) noexcept
    {
        InteractiveZoomState snapshot;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            snapshot = state;
        }
        if (snapshot.viewport_visible &&
            valid_rect(snapshot.viewport_screen)) {
            left = snapshot.viewport_screen.left;
            top = snapshot.viewport_screen.top;
            width = snapshot.viewport_screen.right - left;
            height = snapshot.viewport_screen.bottom - top;
            return;
        }
        POINT pt{};
        ::GetCursorPos(&pt);
        HMONITOR mon = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (mon && ::GetMonitorInfoW(mon, &mi)) {
            left = mi.rcMonitor.left;
            top = mi.rcMonitor.top;
            width = mi.rcMonitor.right - left;
            height = mi.rcMonitor.bottom - top;
            return;
        }
        left = 0;
        top = 0;
        width = ::GetSystemMetrics(SM_CXSCREEN);
        height = ::GetSystemMetrics(SM_CYSCREEN);
    }

    // Move an already-measured key stack with the follow/zoom crop.
    // Avoids CreateFont/GetTextExtent on the encode/UI refresh path.
    void reposition_keys_for_viewport(bool force) noexcept
    {
        if (!keys_window || !options.key_overlay.enabled) return;
        if (keys_cached_chip_count <= 0 || keys_cached_chip_w <= 0 ||
            keys_cached_content_h <= 0) {
            return;
        }
        if (!force && !::IsWindowVisible(keys_window)) return;

        // Cap DWM/WGC damage from a capture-visible overlay while follow runs.
        constexpr ULONGLONG kMinRepositionMs = 33; // ~30 Hz
        constexpr int kMinMovePx = 2;
        const ULONGLONG now = ::GetTickCount64();
        if (!force && keys_last_reposition_ms != 0 &&
            now - keys_last_reposition_ms < kMinRepositionMs)
            return;

        int anchor_left = 0, anchor_top = 0, anchor_w = 0, anchor_h = 0;
        keys_anchor_bounds(anchor_left, anchor_top, anchor_w, anchor_h);
        const auto lay = key_overlay_layout_for_size(
            anchor_w, anchor_h, options.key_overlay);
        int x = 0, y = 0;
        key_overlay_anchor_rect(
            options.key_overlay.align, anchor_w, anchor_h,
            keys_cached_chip_w, keys_cached_content_h,
            lay.margin_x, lay.margin_y, x, y);
        x += anchor_left;
        y += anchor_top;

        const int dx = std::abs(x - keys_x);
        const int dy = std::abs(y - keys_y);
        if (!force && dx < kMinMovePx && dy < kMinMovePx &&
            keys_cached_chip_w == keys_w &&
            keys_cached_content_h == keys_h)
            return;

        ::SetWindowPos(
            keys_window, HWND_TOPMOST, x, y,
            keys_cached_chip_w, keys_cached_content_h,
            SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOSENDCHANGING);
        keys_x = x;
        keys_y = y;
        keys_w = keys_cached_chip_w;
        keys_h = keys_cached_content_h;
        keys_last_reposition_ms = now;
    }

    void layout_and_show_keys()
    {
        if (!keys_window) return;
        std::deque<KeyChip> chips;
        {
            std::lock_guard<std::mutex> lock(keys_mutex);
            chips = key_chips;
        }
        if (chips.empty()) {
            keys_cached_chip_count = 0;
            keys_cached_chip_w = 0;
            keys_cached_content_h = 0;
            ::ShowWindow(keys_window, SW_HIDE);
            return;
        }

        int anchor_left = 0, anchor_top = 0, anchor_w = 0, anchor_h = 0;
        keys_anchor_bounds(anchor_left, anchor_top, anchor_w, anchor_h);
        const auto lay = key_overlay_layout_for_size(
            anchor_w, anchor_h, options.key_overlay);

        // Measure only on content changes (key press / eviction) — not on the
        // per-frame follow refresh path (see reposition_keys_for_viewport).
        HDC screen = ::GetDC(nullptr);
        HFONT font = ::CreateFontW(
            -lay.font_size, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HGDIOBJ old = font ? ::SelectObject(screen, font) : nullptr;
        int max_text_w = 0;
        for (const auto& c : chips) {
            SIZE sz{};
            ::GetTextExtentPoint32W(
                screen, c.label.c_str(),
                static_cast<int>(c.label.size()), &sz);
            max_text_w = std::max(max_text_w, static_cast<int>(sz.cx));
        }
        if (old) ::SelectObject(screen, old);
        if (font) ::DeleteObject(font);
        ::ReleaseDC(nullptr, screen);

        const int chip_h = lay.line_height + 6;
        const int n = static_cast<int>(chips.size());
        const int chip_w = max_text_w + lay.chip_pad_x * 2 + 8;
        const int content_h =
            n * chip_h + std::max(0, n - 1) * lay.stack_gap;
        keys_cached_chip_w = chip_w;
        keys_cached_content_h = content_h;
        keys_cached_chip_count = n;
        keys_layout_font_size = lay.font_size;

        int x = 0, y = 0;
        key_overlay_anchor_rect(
            options.key_overlay.align, anchor_w, anchor_h,
            chip_w, content_h, lay.margin_x, lay.margin_y, x, y);
        x += anchor_left;
        y += anchor_top;

        if (x != keys_x || y != keys_y || chip_w != keys_w ||
            content_h != keys_h) {
            ::SetWindowPos(keys_window, HWND_TOPMOST, x, y, chip_w, content_h,
                           SWP_NOACTIVATE);
            keys_x = x; keys_y = y; keys_w = chip_w; keys_h = content_h;
            keys_last_reposition_ms = ::GetTickCount64();
        }
        const BYTE base = static_cast<BYTE>(std::clamp(
            options.key_overlay.opacity, 0.0f, 1.0f) * 255.0f);
        ::SetLayeredWindowAttributes(
            keys_window, kTransparentKey, base, LWA_COLORKEY | LWA_ALPHA);
        ::ShowWindow(keys_window, SW_SHOWNOACTIVATE);
    }

    void paint_keys(HWND window)
    {
        std::deque<KeyChip> chips;
        {
            std::lock_guard<std::mutex> lock(keys_mutex);
            chips = key_chips;
        }
        PAINTSTRUCT paint{};
        HDC dc = ::BeginPaint(window, &paint);
        RECT client{};
        ::GetClientRect(window, &client);
        HBRUSH transparent = ::CreateSolidBrush(kTransparentKey);
        ::FillRect(dc, &client, transparent);
        ::DeleteObject(transparent);
        if (chips.empty()) {
            ::EndPaint(window, &paint);
            return;
        }

        int anchor_left = 0, anchor_top = 0, anchor_w = 0, anchor_h = 0;
        keys_anchor_bounds(anchor_left, anchor_top, anchor_w, anchor_h);
        (void)anchor_left;
        (void)anchor_top;
        const auto lay = key_overlay_layout_for_size(
            anchor_w, anchor_h, options.key_overlay);
        HFONT font = ::CreateFontW(
            -lay.font_size, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HGDIOBJ old_font = font ? ::SelectObject(dc, font) : nullptr;
        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, rgb_to_colorref(options.key_overlay.color));

        const int chip_h = lay.line_height + 6;
        int y = 0;
        // Newest at the active end; direction from keyOverlayStack / align.
        const bool from_bottom =
            key_overlay_stack_from_bottom(options.key_overlay);
        auto paint_one = [&](const KeyChip& chip, int top) {
            RECT panel{0, top, client.right, top + chip_h};
            HBRUSH bg = ::CreateSolidBrush(
                rgb_to_colorref(options.key_overlay.bg_color));
            ::FillRect(dc, &panel, bg);
            ::DeleteObject(bg);
            RECT accent{0, top, 4, top + chip_h};
            HBRUSH ab = ::CreateSolidBrush(
                rgb_to_colorref(options.key_overlay.accent_color));
            ::FillRect(dc, &accent, ab);
            ::DeleteObject(ab);
            RECT text{lay.chip_pad_x + 4, top, client.right - 4, top + chip_h};
            ::DrawTextW(dc, chip.label.c_str(), -1, &text,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        };
        if (from_bottom) {
            // Newest glued to the bottom edge; older chips stack upward.
            y = client.bottom - chip_h;
            for (auto it = chips.rbegin(); it != chips.rend(); ++it) {
                paint_one(*it, y);
                y -= chip_h + lay.stack_gap;
            }
        } else {
            // Newest at the top; older chips stack downward.
            for (auto it = chips.rbegin(); it != chips.rend(); ++it) {
                paint_one(*it, y);
                y += chip_h + lay.stack_gap;
            }
        }
        if (old_font) ::SelectObject(dc, old_font);
        if (font) ::DeleteObject(font);
        ::EndPaint(window, &paint);
    }

    UINT cursor_fx_dpi(POINT pt) const noexcept
    {
        media::win::ThreadDpiPerMonitorScope dpi_scope;
        return media::win::monitor_dpi_for_point(pt);
    }

    int cursor_highlight_diameter_at(POINT pt) const noexcept
    {
        const UINT dpi = cursor_fx_dpi(pt);
        if (options.cursor_fx.highlight_size > 0) {
            // Explicit sizes are authored at 96 DPI logical px.
            return clamp_value(
                media::win::scale_px_for_dpi(
                    options.cursor_fx.highlight_size, dpi),
                16, 512);
        }
        return media::win::scale_px_for_dpi(72, dpi);
    }

    int cursor_ripple_max_radius_at(POINT pt) const noexcept
    {
        const UINT dpi = cursor_fx_dpi(pt);
        const int d = cursor_highlight_diameter_at(pt);
        return std::max(
            media::win::scale_px_for_dpi(28, dpi),
            d / 2 + media::win::scale_px_for_dpi(8, dpi));
    }

    void push_click_ripple(POINT pt, CursorClickButton button)
    {
        if (!cursor_window || !options.cursor_fx.clicks) return;
        {
            std::lock_guard<std::mutex> lock(cursor_mutex);
            click_ripples.push_back(
                ClickRipple{pt, ::GetTickCount64(), button});
            while (click_ripples.size() > 12)
                click_ripples.pop_front();
        }
        update_cursor_fx();
    }

    // Called from the low-level mouse hook — keep it lock-short, no paint.
    void push_trail_sample(POINT pt) noexcept
    {
        if (!options.cursor_fx.trail) return;
        const ULONGLONG now = ::GetTickCount64();
        if (options.cursor_fx.hide_when_typing &&
            now < typing_hidden_until.load(std::memory_order_relaxed))
            return;
        std::lock_guard<std::mutex> lock(cursor_mutex);
        if (!cursor_trail.empty()) {
            const POINT& last = cursor_trail.back().pt;
            const int dx = pt.x - last.x;
            const int dy = pt.y - last.y;
            if (dx * dx + dy * dy < kTrailMinDistPx * kTrailMinDistPx)
                return;
        }
        cursor_trail.push_back(TrailPoint{pt, now});
        while (cursor_trail.size() > kTrailMaxPoints)
            cursor_trail.pop_front();
        while (!cursor_trail.empty() &&
               now - cursor_trail.front().at >= kTrailMs)
            cursor_trail.pop_front();
    }

    void update_cursor_fx()
    {
        if (!cursor_window || !options.cursor_fx.any()) return;

        media::win::ThreadDpiPerMonitorScope dpi_scope;
        CURSORINFO ci{};
        ci.cbSize = sizeof(ci);
        POINT cursor_pt{};
        bool cursor_ok = false;
        if (::GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
            cursor_pt = ci.ptScreenPos;
            cursor_ok = true;
        } else if (::GetCursorPos(&cursor_pt)) {
            cursor_ok = true;
        }

        const ULONGLONG now = ::GetTickCount64();
        const bool typing_hidden =
            options.cursor_fx.hide_when_typing &&
            now < typing_hidden_until.load(std::memory_order_relaxed);

        const POINT dpi_pt = cursor_ok ? cursor_pt : POINT{0, 0};
        const int hl_d = cursor_highlight_diameter_at(dpi_pt);
        const int ripple_r = cursor_ripple_max_radius_at(dpi_pt);
        const UINT dpi = cursor_fx_dpi(dpi_pt);
        const int pad = media::win::scale_px_for_dpi(4, dpi);
        const int trail_r = media::win::scale_px_for_dpi(10, dpi);

        bool needs_paint = false;
        RECT bounds{};
        bool have_bounds = false;
        auto grow = [&](int l, int t, int r, int b) {
            RECT next{l, t, r, b};
            if (!have_bounds) {
                bounds = next;
                have_bounds = true;
            } else {
                ::UnionRect(&bounds, &bounds, &next);
            }
        };

        {
            std::lock_guard<std::mutex> lock(cursor_mutex);
            while (!click_ripples.empty() &&
                   now - click_ripples.front().shown_at >= kClickRippleMs) {
                click_ripples.pop_front();
                needs_paint = true;
            }
            if (!click_ripples.empty())
                needs_paint = true;

            if (!options.cursor_fx.trail || typing_hidden) {
                if (!cursor_trail.empty()) {
                    cursor_trail.clear();
                    needs_paint = true;
                }
            } else {
                // Timer path seeds the trail when the hook coalesces moves.
                if (cursor_ok) {
                    bool accept = cursor_trail.empty();
                    if (!accept) {
                        const POINT& last = cursor_trail.back().pt;
                        const int dx = cursor_pt.x - last.x;
                        const int dy = cursor_pt.y - last.y;
                        accept = dx * dx + dy * dy >=
                            kTrailMinDistPx * kTrailMinDistPx;
                    }
                    if (accept) {
                        cursor_trail.push_back(
                            TrailPoint{cursor_pt, now});
                        while (cursor_trail.size() > kTrailMaxPoints)
                            cursor_trail.pop_front();
                        needs_paint = true;
                    }
                }
                while (!cursor_trail.empty() &&
                       now - cursor_trail.front().at >= kTrailMs) {
                    cursor_trail.pop_front();
                    needs_paint = true;
                }
                if (!cursor_trail.empty())
                    needs_paint = true;
            }

            if (cursor_ok &&
                (!cursor_last_pt_valid ||
                 cursor_pt.x != cursor_last_pt.x ||
                 cursor_pt.y != cursor_last_pt.y)) {
                cursor_last_pt = cursor_pt;
                cursor_last_pt_valid = true;
                needs_paint = true;
            }

            if (options.cursor_fx.highlight && cursor_ok && !typing_hidden) {
                const int half = hl_d / 2;
                grow(cursor_pt.x - half - pad, cursor_pt.y - half - pad,
                     cursor_pt.x + half + pad, cursor_pt.y + half + pad);
            }
            if (options.cursor_fx.trail && !typing_hidden) {
                for (const TrailPoint& p : cursor_trail) {
                    grow(p.pt.x - trail_r, p.pt.y - trail_r,
                         p.pt.x + trail_r, p.pt.y + trail_r);
                }
            }
            for (const ClickRipple& rip : click_ripples) {
                grow(rip.pt.x - ripple_r - pad, rip.pt.y - ripple_r - pad,
                     rip.pt.x + ripple_r + pad, rip.pt.y + ripple_r + pad);
            }
        }

        if (!have_bounds) {
            if (cursor_window_shown) {
                ::ShowWindow(cursor_window, SW_HIDE);
                cursor_window_shown = false;
            }
            return;
        }

        const int x = bounds.left;
        const int y = bounds.top;
        const int w = std::max(1, static_cast<int>(bounds.right - bounds.left));
        const int h = std::max(1, static_cast<int>(bounds.bottom - bounds.top));
        const bool moved =
            !cursor_window_shown || x != cursor_x || y != cursor_y ||
            w != cursor_w || h != cursor_h;
        if (moved) {
            ::SetWindowPos(cursor_window, HWND_TOPMOST, x, y, w, h,
                           SWP_NOACTIVATE);
            cursor_x = x;
            cursor_y = y;
            cursor_w = w;
            cursor_h = h;
        }
        if (!cursor_window_shown) {
            ::ShowWindow(cursor_window, SW_SHOWNOACTIVATE);
            cursor_window_shown = true;
            needs_paint = true;
        }
        if (needs_paint || moved)
            paint_cursor_fx(cursor_window);
    }

    void paint_cursor_fx(HWND window)
    {
        std::deque<ClickRipple> ripples;
        std::deque<TrailPoint> trail;
        POINT cursor_pt{};
        bool have_cursor = false;
        {
            std::lock_guard<std::mutex> lock(cursor_mutex);
            ripples = click_ripples;
            trail = cursor_trail;
            if (cursor_last_pt_valid) {
                cursor_pt = cursor_last_pt;
                have_cursor = true;
            }
        }

        // Clear any pending WM_PAINT; pixels come from UpdateLayeredWindow.
        if (::GetUpdateRect(window, nullptr, FALSE)) {
            PAINTSTRUCT paint{};
            ::BeginPaint(window, &paint);
            ::EndPaint(window, &paint);
        }

        const int win_w = std::max(1, cursor_w);
        const int win_h = std::max(1, cursor_h);
        if (!cursor_gdip_token || !cursor_layer.ensure(win_w, win_h))
            return;

        Gdiplus::Graphics g(cursor_layer.dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        g.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);

        const ULONGLONG now = ::GetTickCount64();
        const bool typing_hidden =
            options.cursor_fx.hide_when_typing &&
            now < typing_hidden_until.load(std::memory_order_relaxed);
        const float origin_x = static_cast<float>(cursor_x);
        const float origin_y = static_cast<float>(cursor_y);
        const UINT paint_dpi =
            cursor_fx_dpi(have_cursor ? cursor_pt : POINT{0, 0});

        if (options.cursor_fx.trail && !typing_hidden && !trail.empty()) {
            // Prefer highlight color; fall back to click color so trail-only
            // still reads clearly without enabling the ring.
            const std::uint32_t trail_rgb = options.cursor_fx.highlight
                ? options.cursor_fx.highlight_color
                : (options.cursor_fx.clicks
                       ? options.cursor_fx.click_color
                       : options.cursor_fx.highlight_color);
            const float max_dot = static_cast<float>(
                media::win::scale_px_for_dpi(7, paint_dpi));
            const std::size_t n = trail.size();
            for (std::size_t i = 1; i < n; ++i) {
                const float age0 = static_cast<float>(
                    now - trail[i - 1].at) / static_cast<float>(kTrailMs);
                const float age1 = static_cast<float>(
                    now - trail[i].at) / static_cast<float>(kTrailMs);
                const float life = 1.0f - std::clamp(
                    (age0 + age1) * 0.5f, 0.0f, 1.0f);
                if (life <= 0.02f) continue;
                const float width = std::max(1.5f, max_dot * (0.35f + life));
                const BYTE alpha = static_cast<BYTE>(
                    std::clamp(30.0f + life * 200.0f, 0.0f, 255.0f));
                Gdiplus::Pen pen(argb_from_rgb(trail_rgb, alpha), width);
                pen.SetLineCap(
                    Gdiplus::LineCapRound, Gdiplus::LineCapRound,
                    Gdiplus::DashCapRound);
                pen.SetLineJoin(Gdiplus::LineJoinRound);
                g.DrawLine(
                    &pen,
                    static_cast<Gdiplus::REAL>(trail[i - 1].pt.x) - origin_x,
                    static_cast<Gdiplus::REAL>(trail[i - 1].pt.y) - origin_y,
                    static_cast<Gdiplus::REAL>(trail[i].pt.x) - origin_x,
                    static_cast<Gdiplus::REAL>(trail[i].pt.y) - origin_y);
            }
            // Soft head dots so the comet reads even on short segments.
            for (std::size_t i = 0; i < n; ++i) {
                const float age = static_cast<float>(
                    now - trail[i].at) / static_cast<float>(kTrailMs);
                const float life = 1.0f - std::clamp(age, 0.0f, 1.0f);
                if (life <= 0.05f) continue;
                const float r = std::max(1.0f, max_dot * 0.55f * life);
                const BYTE alpha = static_cast<BYTE>(
                    std::clamp(40.0f + life * 180.0f, 0.0f, 255.0f));
                Gdiplus::SolidBrush brush(argb_from_rgb(trail_rgb, alpha));
                const float cx =
                    static_cast<float>(trail[i].pt.x) - origin_x;
                const float cy =
                    static_cast<float>(trail[i].pt.y) - origin_y;
                g.FillEllipse(&brush, cx - r, cy - r, r * 2.0f, r * 2.0f);
            }
        }

        if (options.cursor_fx.highlight && have_cursor && !typing_hidden) {
            const float d = static_cast<float>(
                cursor_highlight_diameter_at(cursor_pt));
            const float half = d * 0.5f;
            const float cx =
                static_cast<float>(cursor_pt.x) - origin_x;
            const float cy =
                static_cast<float>(cursor_pt.y) - origin_y;
            const float outer_w = static_cast<float>(
                media::win::scale_px_for_dpi(6, paint_dpi));
            const float inner_w = static_cast<float>(
                media::win::scale_px_for_dpi(2, paint_dpi));
            const float inner_inset = static_cast<float>(
                media::win::scale_px_for_dpi(8, paint_dpi));
            Gdiplus::Pen outer(
                argb_from_rgb(options.cursor_fx.highlight_color, 170),
                outer_w);
            outer.SetAlignment(Gdiplus::PenAlignmentCenter);
            g.DrawEllipse(&outer, cx - half, cy - half, d, d);
            Gdiplus::Pen inner(
                argb_from_rgb(options.cursor_fx.highlight_color, 230),
                inner_w);
            const float inner_r = std::max(inner_inset, half - inner_inset);
            g.DrawEllipse(
                &inner, cx - inner_r, cy - inner_r,
                inner_r * 2.0f, inner_r * 2.0f);
        }

        if (options.cursor_fx.clicks) {
            for (const ClickRipple& rip : ripples) {
                const ULONGLONG age = now - rip.shown_at;
                if (age >= kClickRippleMs) continue;
                const int max_r = cursor_ripple_max_radius_at(rip.pt);
                const UINT rip_dpi = cursor_fx_dpi(rip.pt);
                const float t =
                    static_cast<float>(age) /
                    static_cast<float>(kClickRippleMs);
                const float r = std::max(
                    static_cast<float>(
                        media::win::scale_px_for_dpi(4, rip_dpi)),
                    4.0f + t * static_cast<float>(max_r));
                const float pen_w = std::max(
                    1.0f,
                    static_cast<float>(media::win::scale_px_for_dpi(
                        std::max(1, 4 - static_cast<int>(t * 3.0f)),
                        rip_dpi)));
                const BYTE alpha = static_cast<BYTE>(
                    std::clamp(220.0f * (1.0f - t), 0.0f, 255.0f));
                const std::uint32_t rip_rgb = cursor_click_color_for_button(
                    options.cursor_fx.click_color, rip.button);
                Gdiplus::Pen pen(argb_from_rgb(rip_rgb, alpha), pen_w);
                const float cx =
                    static_cast<float>(rip.pt.x) - origin_x;
                const float cy =
                    static_cast<float>(rip.pt.y) - origin_y;
                g.DrawEllipse(&pen, cx - r, cy - r, r * 2.0f, r * 2.0f);
                if (t < 0.55f) {
                    const float r2 = std::max(2.0f, r * 0.5f);
                    g.DrawEllipse(
                        &pen, cx - r2, cy - r2, r2 * 2.0f, r2 * 2.0f);
                }
            }
        }

        POINT dst{cursor_x, cursor_y};
        SIZE sz_win{win_w, win_h};
        POINT src_pt{0, 0};
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;
        HDC screen = ::GetDC(nullptr);
        if (screen) {
            ::UpdateLayeredWindow(
                window, screen, &dst, &sz_win, cursor_layer.dc, &src_pt, 0,
                &blend, ULW_ALPHA);
            ::ReleaseDC(nullptr, screen);
        }
    }
};

std::atomic<WinInteractiveZoomController::Impl*>
    WinInteractiveZoomController::Impl::active_hook_owner{nullptr};

WinInteractiveZoomController::WinInteractiveZoomController()
    : impl_(std::make_unique<Impl>())
{
}

WinInteractiveZoomController::~WinInteractiveZoomController()
{
    stop();
}

bool WinInteractiveZoomController::start(
    const InteractiveZoomOptions& options,
    const InteractiveZoomState& initial_state,
    InteractiveZoomWheelCallback wheel_callback,
    InteractivePauseCallback pause_callback,
    InteractiveStopCallback stop_callback,
    std::string* error)
{
    if (impl_->ui_thread_id.load(std::memory_order_acquire) ==
        ::GetCurrentThreadId()) {
        if (error)
            *error = "cannot start from the controller callback thread";
        return false;
    }

    std::unique_lock<std::mutex> api_lock(impl_->api_mutex);
    if (impl_->is_running.load(std::memory_order_acquire)) {
        if (error)
            *error = "interactive zoom controller is already started";
        return false;
    }
    if (impl_->ui_thread.joinable()) {
        if (impl_->ui_thread.get_id() == std::this_thread::get_id()) {
            if (error)
                *error = "cannot restart from the controller callback thread";
            return false;
        }
        impl_->ui_thread.join();
    }

    impl_->options = options;
    impl_->options.viewport_thickness =
        std::max(1, impl_->options.viewport_thickness);
    {
        std::lock_guard<std::mutex> state_lock(impl_->state_mutex);
        impl_->state = initial_state;
    }
    impl_->callback = std::move(wheel_callback);
    impl_->pause_callback = std::move(pause_callback);
    impl_->stop_callback = std::move(stop_callback);
    impl_->pause_key_down = false;
    impl_->stop_key_down = false;
    impl_->consume_input.store(options.consume_input, std::memory_order_release);
    impl_->refresh_pending.store(false, std::memory_order_release);
    impl_->refresh_reveal_hud.store(false, std::memory_order_release);
    impl_->viewport_applied = false;
    impl_->hud_state_applied = false;
    impl_->wheel_remainder = 0;
    impl_->wheel_read.store(0, std::memory_order_relaxed);
    impl_->wheel_write.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> init_lock(impl_->init_mutex);
        impl_->init_done = false;
        impl_->init_ok = false;
        impl_->init_error.clear();
    }

    try {
        impl_->ui_thread = std::thread([implementation = impl_.get()] {
            implementation->thread_main();
        });
    } catch (const std::exception& exception) {
        if (error)
            *error = std::string("failed to create controller thread: ") +
                     exception.what();
        return false;
    }

    std::unique_lock<std::mutex> init_lock(impl_->init_mutex);
    impl_->init_cv.wait(init_lock, [this] { return impl_->init_done; });
    const bool ok = impl_->init_ok;
    if (error)
        *error = impl_->init_error;
    init_lock.unlock();

    if (!ok && impl_->ui_thread.joinable())
        impl_->ui_thread.join();
    return ok;
}

void WinInteractiveZoomController::update(
    const InteractiveZoomState& state, bool reveal_hud)
{
    {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        impl_->state = state;
    }
    const DWORD thread_id = impl_->ui_thread_id.load(std::memory_order_acquire);
    if (thread_id && impl_->is_running.load(std::memory_order_acquire)) {
        if (reveal_hud)
            impl_->refresh_reveal_hud.store(true, std::memory_order_release);
        bool expected = false;
        if (impl_->refresh_pending.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            if (!::PostThreadMessageW(
                    thread_id, kRefreshMessage, 0, 0)) {
                impl_->refresh_pending.store(false, std::memory_order_release);
            }
        }
    }
}

void WinInteractiveZoomController::set_input_activity_callback(
    InteractiveInputActivityCallback cb)
{
    impl_->input_activity = std::move(cb);
}

void WinInteractiveZoomController::set_consume_input(bool consume_input) noexcept
{
    impl_->consume_input.store(consume_input, std::memory_order_release);
}

void WinInteractiveZoomController::stop() noexcept
{
    const DWORD initial_thread_id =
        impl_->ui_thread_id.load(std::memory_order_acquire);
    if (initial_thread_id == ::GetCurrentThreadId()) {
        ::PostThreadMessageW(initial_thread_id, kStopMessage, 0, 0);
        return;
    }

    std::unique_lock<std::mutex> api_lock(impl_->api_mutex);
    const DWORD thread_id = impl_->ui_thread_id.load(std::memory_order_acquire);
    if (thread_id)
        ::PostThreadMessageW(thread_id, kStopMessage, 0, 0);

    if (!impl_->ui_thread.joinable())
        return;
    impl_->ui_thread.join();
}

bool WinInteractiveZoomController::running() const noexcept
{
    return impl_->is_running.load(std::memory_order_acquire);
}

const char* interactive_follow_mode_name(InteractiveFollowMode mode) noexcept
{
    switch (mode) {
    case InteractiveFollowMode::none: return "NONE";
    case InteractiveFollowMode::anchor: return "ANCHOR";
    case InteractiveFollowMode::cursor: return "CURSOR";
    }
    return "UNKNOWN";
}

} // namespace pm::video

#endif // _WIN32
