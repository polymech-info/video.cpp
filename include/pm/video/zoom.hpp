#pragma once

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "pm/video/key_overlay.hpp"
#include "pm/video/creator_fx.hpp"

#include <functional>
#include <memory>
#include <string>

namespace pm::video {

enum class InteractiveFollowMode {
    none,
    anchor,
    cursor,
};

struct InteractiveZoomState {
    double zoom = 1.0;
    InteractiveFollowMode follow_mode = InteractiveFollowMode::none;
    int output_width = 0;
    int output_height = 0;

    // Screen coordinates of the source viewport represented by the border.
    RECT viewport_screen{};
    bool viewport_visible = false;
    bool recording = true;
    bool paused = false;
};

struct InteractiveZoomOptions {
    bool enable_wheel_input = true;
    // When true, handled Shift+wheel messages do not reach other applications.
    bool consume_input = false;
    bool show_hud = true;
    bool show_viewport_border = true;
    // When true, OBS and other screen capture tools can record the overlays.
    bool overlays_visible_to_capture = false;
    unsigned int hud_hold_ms = 1200;
    unsigned int hud_fade_ms = 450;
    COLORREF accent_color = RGB(235, 68, 90);
    COLORREF viewport_color = RGB(64, 190, 255);
    int viewport_thickness = 3;
    unsigned int pause_modifiers = 0;
    unsigned int pause_virtual_key = 0;
    bool consume_pause_input = true;
    unsigned int stop_modifiers = 0;
    unsigned int stop_virtual_key = 0;
    bool consume_stop_input = true;
    /// Keystroke OSD (optional). When enabled, the controller installs the
    /// keyboard hook even without pauseKey / stopKey / interactiveZoom.
    KeyOverlayConfig key_overlay{};
    /// Cursor highlight / click ripples (slice 1). Drawn as a click-through
    /// layered window; included in capture by default for tutorial recordings.
    CursorFxConfig cursor_fx{};
    /// Install LL hooks even without zoom/pause/overlay (auto-pause input).
    bool track_input_activity = false;
};

// Invoked on the controller's message-loop thread, after the low-level hook
// has returned. Keep callback work short; hand recording work to its owner.
using InteractiveZoomWheelCallback =
    std::function<void(int wheel_steps, POINT screen_cursor)>;
using InteractivePauseCallback = std::function<void()>;
using InteractiveStopCallback = std::function<void()>;
enum class IdleInputKind { key = 0, click = 1, move = 2 };
using InteractiveInputActivityCallback =
    std::function<void(IdleInputKind kind, POINT screen)>;

// Reusable process-local Windows controller for interactive recording zoom.
// All public methods are thread-safe. Only one instance may be started in a
// process at a time because WH_MOUSE_LL is process-global for this facility.
class WinInteractiveZoomController {
public:
    WinInteractiveZoomController();
    ~WinInteractiveZoomController();

    WinInteractiveZoomController(const WinInteractiveZoomController&) = delete;
    WinInteractiveZoomController& operator=(const WinInteractiveZoomController&) = delete;

    bool start(const InteractiveZoomOptions& options,
               const InteractiveZoomState& initial_state,
               InteractiveZoomWheelCallback wheel_callback,
               InteractivePauseCallback pause_callback,
               InteractiveStopCallback stop_callback = {},
               std::string* error = nullptr);

    void set_input_activity_callback(InteractiveInputActivityCallback cb);

    // Refreshes the HUD and viewport. Set reveal_hud=false for continuous
    // tracking updates so the border moves without restarting the HUD fade.
    // Safe to call from any thread, including from wheel_callback.
    void update(
        const InteractiveZoomState& state, bool reveal_hud = true);

    // Changes whether Shift+wheel is swallowed without reinstalling the hook.
    void set_consume_input(bool consume_input) noexcept;

    // Safe and idempotent. If called by wheel_callback, teardown is requested
    // and the thread is joined by a later stop() call or by the destructor.
    void stop() noexcept;

    bool running() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

const char* interactive_follow_mode_name(InteractiveFollowMode mode) noexcept;

} // namespace pm::video

#endif // _WIN32
