#pragma once
//
// creator_fx.hpp — creator option configs (cursor / camera / audio polish).
//
// See docs/video/video-next.md (three slices).
// Slice 1 (cursor): WinInteractiveZoomController overlay.
// Slice 2 (camera): WinCameraOverlayController (MF + layered PiP) + camera mic mix.
// Slice 3 (audio polish): live DeepFilterNet on video mux (mic/mix); AGC/normalize reserved.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "pm/video/filters/beauty_config.hpp"

namespace pm::video {

inline std::uint32_t creator_fx_parse_rgb(std::string hex, std::uint32_t fallback) noexcept
{
    if (!hex.empty() && hex[0] == '#') hex.erase(0, 1);
    if (hex.size() != 6) return fallback;
    try {
        return static_cast<std::uint32_t>(std::stoul(hex, nullptr, 16)) & 0xFFFFFFu;
    } catch (...) {
        return fallback;
    }
}

/// Rotate @p rgb hue by @p degrees (HSL). Keeps saturation/lightness so derived
/// click colors stay complementary to the user-chosen left-click color.
inline std::uint32_t creator_fx_shift_hue(
    std::uint32_t rgb, float degrees) noexcept
{
    const float r = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f;
    const float g = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f;
    const float b = static_cast<float>(rgb & 0xFF) / 255.0f;
    const float max_c = std::max({r, g, b});
    const float min_c = std::min({r, g, b});
    const float delta = max_c - min_c;
    const float l = (max_c + min_c) * 0.5f;
    float h = 0.0f;
    float s = 0.0f;
    if (delta > 1e-6f) {
        s = (l > 0.5f)
            ? delta / (2.0f - max_c - min_c)
            : delta / (max_c + min_c);
        if (max_c == r)
            h = (g - b) / delta + (g < b ? 6.0f : 0.0f);
        else if (max_c == g)
            h = (b - r) / delta + 2.0f;
        else
            h = (r - g) / delta + 4.0f;
        h *= 60.0f;
    }
    h = std::fmod(h + degrees, 360.0f);
    if (h < 0.0f) h += 360.0f;

    const auto hue_to_rgb = [](float p, float q, float t) noexcept {
        if (t < 0.0f) t += 1.0f;
        if (t > 1.0f) t -= 1.0f;
        if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
        if (t < 0.5f) return q;
        if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
        return p;
    };

    float out_r = l, out_g = l, out_b = l;
    if (s > 1e-6f) {
        const float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
        const float p = 2.0f * l - q;
        const float hk = h / 360.0f;
        out_r = hue_to_rgb(p, q, hk + 1.0f / 3.0f);
        out_g = hue_to_rgb(p, q, hk);
        out_b = hue_to_rgb(p, q, hk - 1.0f / 3.0f);
    }
    const auto to8 = [](float v) noexcept -> std::uint32_t {
        return static_cast<std::uint32_t>(
            std::clamp(static_cast<int>(std::lround(v * 255.0f)), 0, 255));
    };
    return (to8(out_r) << 16) | (to8(out_g) << 8) | to8(out_b);
}

enum class CursorClickButton : int {
    left = 0,
    right = 1,
    middle = 2,
    other = 3,
};

/// Left uses @p base; right = complementary (+180°); middle = triadic (+120°).
inline std::uint32_t cursor_click_color_for_button(
    std::uint32_t base, CursorClickButton button) noexcept
{
    switch (button) {
    case CursorClickButton::right:
        return creator_fx_shift_hue(base, 180.0f);
    case CursorClickButton::middle:
        return creator_fx_shift_hue(base, 120.0f);
    case CursorClickButton::other:
        return creator_fx_shift_hue(base, 240.0f);
    case CursorClickButton::left:
    default:
        return base & 0xFFFFFFu;
    }
}

inline std::string creator_fx_lower(std::string s) noexcept
{
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c == '_') c = '-';
    }
    return s;
}

// ── Slice 1: cursor ───────────────────────────────────────────────────────────

struct CursorFxConfig {
    bool highlight = false;
    std::uint32_t highlight_color = 0xF5C542u;
    int highlight_size = 0;  // 0 = auto
    bool clicks = false;
    std::uint32_t click_color = 0xEB445Au;
    bool trail = false;
    bool hide_when_typing = false;

    bool any() const noexcept
    {
        return highlight || clicks || trail || hide_when_typing;
    }
};

inline CursorFxConfig cursor_fx_from_json(const nlohmann::json& j)
{
    CursorFxConfig c;
    c.highlight = j.value("cursorHighlight", false);
    c.highlight_color = creator_fx_parse_rgb(
        j.value("cursorHighlightColor", std::string("#F5C542")), 0xF5C542u);
    c.highlight_size = std::max(0, j.value("cursorHighlightSize", 0));
    c.clicks = j.value("cursorClicks", false);
    c.click_color = creator_fx_parse_rgb(
        j.value("cursorClickColor", std::string("#EB445A")), 0xEB445Au);
    c.trail = j.value("cursorTrail", false);
    c.hide_when_typing = j.value("hideCursorWhenTyping", false);
    return c;
}

inline nlohmann::json cursor_fx_defaults_json()
{
    return {
        {"cursorHighlight", false},
        {"cursorHighlightColor", "#F5C542"},
        {"cursorHighlightSize", 0},
        {"cursorClicks", false},
        {"cursorClickColor", "#EB445A"},
        {"cursorTrail", false},
        {"hideCursorWhenTyping", false},
    };
}

// ── Slice 2: camera overlay ───────────────────────────────────────────────────

enum class CameraOverlayPosition : int {
    top_left = 0,
    top_right = 1,
    bottom_left = 2,
    bottom_right = 3,
};

enum class CameraOverlayShape : int {
    circle = 0,
    rounded = 1,
    square = 2,
};

struct CameraOverlayConfig {
    bool enabled = false;
    std::string input;  // empty = default camera
    CameraOverlayPosition position = CameraOverlayPosition::bottom_right;
    float size = 22.0f;  // % of short edge when <= 100; else treated as px hint
    CameraOverlayShape shape = CameraOverlayShape::circle;
    bool border = true;
    std::uint32_t border_color = 0xFFFFFFFFu;
    bool shadow = true;
    bool audio = false;           // include webcam mic in record mix
    float audio_gain = 1.0f;      // linear gain for camera mic path
    /// Capture mode: "auto" (PiP-sized), "max" (highest native), or "WxH" / "WxH@fps"
    /// (e.g. "1920x1080", "3840x2160@30"). Also accepts 720p / 1080p / 1440p / 4k.
    std::string resolution = "auto";
    /// When `enabled`, face / bg knobs run on the PiP webcam only (not the screen).
    BeautyConfig beauty{};

    bool any() const noexcept { return enabled; }
    bool audio_any() const noexcept { return audio; }
};

inline BeautyConfig beauty_config_from_json(const nlohmann::json& j)
{
    BeautyConfig c;
    c.skin = j.value("skin", false);
    c.skin_strength = static_cast<float>(
        std::clamp(j.value("skinBlend", j.value("skinStrength", 0.5)), 0.0, 1.0));
    c.lips = j.value("lips", false);
    c.lips_color = j.value("lipsColor", std::string("#C45C6A"));
    c.lips_alpha = static_cast<float>(
        std::clamp(j.value("lipsBlend", j.value("lipsAlpha", 0.35)), 0.0, 1.0));
    c.teeth = j.value("teeth", false);
    c.teeth_strength = static_cast<float>(
        std::clamp(j.value("teethBlend", j.value("teethStrength", 0.4)), 0.0, 1.0));
    c.show_landmarks = j.value("landmarks", j.value("showLandmarks", false));
    c.mirror = j.value("mirror", false);
    c.face_gain = static_cast<float>(std::clamp(j.value("faceGain", 0.0), 0.0, 1.0));
    c.warmth = static_cast<float>(std::clamp(j.value("warmth", 0.0), 0.0, 1.0));
    c.undereye = j.value("undereye", false);
    c.undereye_blend = static_cast<float>(
        std::clamp(j.value("undereyeBlend", 0.45), 0.0, 1.0));
    c.redness = j.value("redness", false);
    c.redness_blend = static_cast<float>(
        std::clamp(j.value("rednessBlend", 0.4), 0.0, 1.0));
    c.eye_sharpen = j.value("eyeSharpen", false);
    c.eye_sharpen_blend = static_cast<float>(
        std::clamp(j.value("eyeSharpenBlend", 0.35), 0.0, 1.0));
    c.blur = j.value("blur", false);
    c.background = j.value("background", false);
    c.blur_strength = static_cast<float>(
        std::clamp(j.value("blurBlend", j.value("blurStrength", 0.65)), 0.0, 1.0));
    c.feather_px = std::clamp(j.value("featherPx", 8), 0, 64);
    c.bg_color = j.value("bgColor", std::string("#1A1A1A"));
    c.bg_image = j.value("bgImage", std::string{});
    c.face = c.skin || c.lips || c.teeth || c.show_landmarks || c.undereye ||
             c.redness || c.eye_sharpen || c.face_gain > 0.f || c.warmth > 0.f;
    c.running_mode = "video";
    return c;
}

inline CameraOverlayPosition parse_camera_overlay_position(std::string name) noexcept
{
    name = creator_fx_lower(std::move(name));
    if (name == "top-left" || name == "topleft") return CameraOverlayPosition::top_left;
    if (name == "top-right" || name == "topright") return CameraOverlayPosition::top_right;
    if (name == "bottom-left" || name == "bottomleft")
        return CameraOverlayPosition::bottom_left;
    return CameraOverlayPosition::bottom_right;
}

inline CameraOverlayShape parse_camera_overlay_shape(std::string name) noexcept
{
    name = creator_fx_lower(std::move(name));
    if (name == "rounded") return CameraOverlayShape::rounded;
    if (name == "square") return CameraOverlayShape::square;
    return CameraOverlayShape::circle;
}

inline CameraOverlayConfig camera_overlay_from_json(const nlohmann::json& j)
{
    CameraOverlayConfig c;
    c.enabled = j.value("cameraOverlay", false);
    c.input = j.value("cameraInput", std::string{});
    c.position = parse_camera_overlay_position(
        j.value("cameraPosition", std::string("bottom-right")));
    c.size = static_cast<float>(std::clamp(j.value("cameraSize", 22.0), 4.0, 80.0));
    c.shape = parse_camera_overlay_shape(j.value("cameraShape", std::string("circle")));
    c.border = j.value("cameraBorder", true);
    c.border_color = creator_fx_parse_rgb(
        j.value("cameraBorderColor", std::string("#FFFFFF")), 0xFFFFFFFFu);
    c.shadow = j.value("cameraShadow", true);
    c.audio = j.value("cameraAudio", false);
    c.audio_gain = static_cast<float>(
        std::clamp(j.value("cameraAudioGain", 1.0), 0.0, 4.0));
    c.resolution = j.value("cameraResolution", std::string("auto"));
    if (c.resolution.empty()) c.resolution = "auto";
    c.beauty = beauty_config_from_json(j);
    return c;
}

inline nlohmann::json camera_overlay_defaults_json()
{
    return {
        {"cameraOverlay", false},
        {"cameraInput", ""},
        {"cameraPosition", "bottom-right"},
        {"cameraSize", 22.0},
        {"cameraShape", "circle"},
        {"cameraBorder", true},
        {"cameraBorderColor", "#FFFFFF"},
        {"cameraShadow", true},
        {"cameraAudio", false},
        {"cameraAudioGain", 1.0},
        {"cameraResolution", "auto"},
    };
}

/// Screen-space square for the PiP over `anchor` (monitor/window bounds).
/// `size` ≤ 100 → percent of short edge; > 100 → treated as pixels.
inline void camera_overlay_layout_rect(
    int anchor_l, int anchor_t, int anchor_w, int anchor_h,
    const CameraOverlayConfig& c,
    int& out_x, int& out_y, int& out_size) noexcept
{
    const int short_edge = std::max(1, std::min(anchor_w, anchor_h));
    int size_px = c.size > 100.0f
        ? static_cast<int>(c.size)
        : static_cast<int>(short_edge * (c.size / 100.0f) + 0.5f);
    size_px = std::clamp(size_px, 64, std::max(64, short_edge));
    const int margin = std::max(12, size_px / 16);
    switch (c.position) {
    case CameraOverlayPosition::top_left:
        out_x = anchor_l + margin;
        out_y = anchor_t + margin;
        break;
    case CameraOverlayPosition::top_right:
        out_x = anchor_l + anchor_w - margin - size_px;
        out_y = anchor_t + margin;
        break;
    case CameraOverlayPosition::bottom_left:
        out_x = anchor_l + margin;
        out_y = anchor_t + anchor_h - margin - size_px;
        break;
    case CameraOverlayPosition::bottom_right:
    default:
        out_x = anchor_l + anchor_w - margin - size_px;
        out_y = anchor_t + anchor_h - margin - size_px;
        break;
    }
    out_size = size_px;
}

// ── Slice 3: audio polish (low risk only) ─────────────────────────────────────

struct AudioPolishConfig {
    bool normalize = false;  // post-mix normalize before AAC (reserved)
    bool auto_gain = false;  // gentle mic AGC (reserved)
    std::string filter;      // empty/off | deepfilter | gtcrn
    std::string filter_model;
    float atten_lim_db = 100.f;
    bool post_filter = false;
    bool compensate_delay = true;

    bool reserved_any() const noexcept { return normalize || auto_gain; }
    bool filter_on() const noexcept {
        if (post_filter || !filter_model.empty())
            return true;
        return !filter.empty() && filter != "off" && filter != "none";
    }
    bool any() const noexcept { return reserved_any() || filter_on(); }
};

inline AudioPolishConfig audio_polish_from_json(const nlohmann::json& j)
{
    AudioPolishConfig c;
    c.normalize = j.value("audioNormalize", false);
    c.auto_gain = j.value("audioAutoGain", false);
    c.filter = j.value("filter", std::string{});
    c.filter_model = j.value("filterModel", std::string{});
    c.atten_lim_db = static_cast<float>(j.value("attenLim", 100.0));
    c.post_filter = j.value("postFilter", false);
    c.compensate_delay = j.value("compensateDelay", true);
    return c;
}

inline nlohmann::json audio_polish_defaults_json()
{
    return {
        {"audioNormalize", false},
        {"audioAutoGain", false},
        {"filter", ""},
        {"filterModel", ""},
        {"attenLim", 100.0},
        {"postFilter", false},
        {"compensateDelay", true},
    };
}

// ── Slice 4: capture idle ─────────────────────────────────────────────────────

enum class AutoPauseMode : int {
    off = 0,
    visual = 1,
    audio = 2,
    both = 3,
};

inline AutoPauseMode parse_auto_pause_mode(std::string name) noexcept
{
    name = creator_fx_lower(std::move(name));
    if (name == "visual" || name == "video") return AutoPauseMode::visual;
    if (name == "audio" || name == "mic") return AutoPauseMode::audio;
    if (name == "both" || name == "all" || name == "on" || name == "true")
        return AutoPauseMode::both;
    return AutoPauseMode::off;
}

inline const char* auto_pause_mode_name(AutoPauseMode m) noexcept
{
    switch (m) {
    case AutoPauseMode::visual: return "visual";
    case AutoPauseMode::audio:  return "audio";
    case AutoPauseMode::both:   return "both";
    case AutoPauseMode::off:
    default:                    return "off";
    }
}

struct IdlePauseConfig {
    AutoPauseMode mode = AutoPauseMode::off;
    int idle_ms = 3000;
    int resume_ms = 300;
    int grid_w = 32;
    int grid_h = 18;
    int probe_w = 64;
    int probe_h = 36;
    int pixel_delta = 12;
    int tile_percent = 8;
    int min_tiles = 4;
    float min_coverage_percent = 2.f;
    int sample_fps = 8;
    bool mic_wake = true;
    float mic_activity_db = -40.f;
    int mic_hangover_ms = 500;
    int key_hold_ms = 750;
    int mouse_hold_ms = 350;
    bool wake_key = true;
    bool wake_mouse = true;
    int mouse_delta_px = 4;
    int preroll_ms = 500;

    bool enabled() const noexcept
    {
        return mode != AutoPauseMode::off && idle_ms > 0;
    }
    bool uses_visual() const noexcept
    {
        return mode == AutoPauseMode::visual || mode == AutoPauseMode::both;
    }
    bool uses_audio() const noexcept
    {
        return mode == AutoPauseMode::audio || mode == AutoPauseMode::both;
    }
    int preroll_frames(int fps) const noexcept
    {
        const int f = std::max(1, fps);
        const int n = (std::max(0, preroll_ms) * f + 999) / 1000;
        return std::clamp(n, 0, 60);
    }
};

inline IdlePauseConfig idle_pause_from_json(const nlohmann::json& j)
{
    IdlePauseConfig c;
    c.mode = parse_auto_pause_mode(j.value("autoPause", std::string("off")));
    c.idle_ms = std::max(0, j.value("autoPauseIdleMs", 3000));
    c.resume_ms = std::clamp(j.value("autoPauseResumeMs", 300), 0, 5000);
    c.grid_w = std::clamp(j.value("autoPauseGridW", 32), 8, 128);
    c.grid_h = std::clamp(j.value("autoPauseGridH", 18), 8, 72);
    c.probe_w = std::clamp(j.value("autoPauseProbeW", 64), 64, 192);
    c.probe_h = std::clamp(j.value("autoPauseProbeH", 36), 36, 108);
    c.pixel_delta = std::clamp(j.value("autoPausePixelDelta", 12), 1, 255);
    c.tile_percent = std::clamp(j.value("autoPauseTilePercent", 8), 1, 100);
    c.min_tiles = std::clamp(j.value("autoPauseMinTiles", 4), 1, 512);
    c.min_coverage_percent = static_cast<float>(
        std::clamp(j.value("autoPauseMinCoveragePercent", 2.0), 0.0, 100.0));
    c.sample_fps = std::clamp(j.value("autoPauseSampleFps", 8), 1, 30);
    c.mic_wake = j.value("autoPauseMicWake", true);
    c.mic_activity_db = static_cast<float>(
        std::clamp(j.value("autoPauseMicActivityDb", -40.0), -90.0, 0.0));
    c.mic_hangover_ms = std::clamp(j.value("autoPauseMicHangoverMs", 500), 0, 5000);
    c.key_hold_ms = std::clamp(j.value("autoPauseKeyHoldMs", 750), 0, 5000);
    c.mouse_hold_ms = std::clamp(j.value("autoPauseMouseHoldMs", 350), 0, 5000);
    c.wake_key = j.value("autoPauseWakeKey", true);
    c.wake_mouse = j.value("autoPauseWakeMouse", true);
    c.mouse_delta_px = std::clamp(j.value("autoPauseMouseDeltaPx", 4), 0, 64);
    c.preroll_ms = std::clamp(j.value("autoPausePrerollMs", 500), 0, 2000);
    return c;
}

inline nlohmann::json idle_pause_defaults_json()
{
    return {
        {"autoPause", "off"},
        {"autoPauseIdleMs", 3000},
        {"autoPauseResumeMs", 300},
        {"autoPauseGridW", 32},
        {"autoPauseGridH", 18},
        {"autoPauseProbeW", 64},
        {"autoPauseProbeH", 36},
        {"autoPausePixelDelta", 12},
        {"autoPauseTilePercent", 8},
        {"autoPauseMinTiles", 4},
        {"autoPauseMinCoveragePercent", 2.0},
        {"autoPauseSampleFps", 8},
        {"autoPauseMicWake", true},
        {"autoPauseMicActivityDb", -40.0},
        {"autoPauseMicHangoverMs", 500},
        {"autoPauseKeyHoldMs", 750},
        {"autoPauseMouseHoldMs", 350},
        {"autoPauseWakeKey", true},
        {"autoPauseWakeMouse", true},
        {"autoPauseMouseDeltaPx", 4},
        {"autoPausePrerollMs", 500},
    };
}

// ── Slice 5: captions ─────────────────────────────────────────────────────────

struct SttRoute {
    std::string preset;
    std::string provider;
    std::string model;
    nlohmann::json options = nlohmann::json::object();
};

inline SttRoute stt_route_from_json(const nlohmann::json& j)
{
    SttRoute r;
    if (j.is_null() || j.empty())
        return r;
    if (j.is_string()) {
        r.provider = j.get<std::string>();
        return r;
    }
    if (!j.is_object())
        return r;
    r.preset = j.value("preset", std::string{});
    r.provider = j.value("provider", std::string{});
    r.model = j.value("model", std::string{});
    if (j.contains("options") && j["options"].is_object())
        r.options = j["options"];
    return r;
}

inline nlohmann::json stt_route_to_json(const SttRoute& r)
{
    return {
        {"preset", r.preset},
        {"provider", r.provider},
        {"model", r.model},
        {"options", r.options.is_object() ? r.options : nlohmann::json::object()},
    };
}

struct CaptionConfig {
    bool sidecar = false;
    std::string format = "srt"; // srt | vtt | sbv | all
    SttRoute stt{};
    int max_chars = 42;
    int max_lines = 2;

    bool any() const noexcept { return sidecar; }
};

inline CaptionConfig caption_from_json(const nlohmann::json& j)
{
    CaptionConfig c;
    std::string cap = j.value("captions", std::string("off"));
    cap = creator_fx_lower(std::move(cap));
    c.sidecar = cap == "sidecar" || cap == "on" || cap == "true" || cap == "srt"
                || cap == "vtt" || cap == "sbv" || cap == "all";
    c.format = j.value("captionFormat", std::string("srt"));
    if (cap == "srt" || cap == "vtt" || cap == "sbv" || cap == "all")
        c.format = cap;
    if (j.contains("captionStt"))
        c.stt = stt_route_from_json(j["captionStt"]);
    c.max_chars = std::clamp(j.value("subtitleMaxChars", 42), 8, 40000);
    c.max_lines = std::clamp(j.value("subtitleMaxLines", 2), 1, 4);
    return c;
}

inline nlohmann::json caption_defaults_json()
{
    return {
        {"captions", "off"},
        {"captionFormat", "srt"},
        {"captionStt", nlohmann::json::object()},
        {"subtitleMaxChars", 42},
        {"subtitleMaxLines", 2},
    };
}

} // namespace pm::video
