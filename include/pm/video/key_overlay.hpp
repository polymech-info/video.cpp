#pragma once
//
// key_overlay.hpp — reusable keystroke OSD config + layout helpers.
//
// Shared by videoCapture / videoStream / CLI record. Params are registered via
// `pd_key_overlay_params()` (flat fields, group "key_overlay") so any block can
// append the same bundle without inventing a nested ParamKind widget.
//
// Alignment mirrors VLC marquee / DVB subpicture positions:
//   0 center, 1 left, 2 right, 4 top, 8 bottom,
//   5 top-left, 6 top-right, 9 bottom-left, 10 bottom-right.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>

namespace pm::video {

/// VLC-style subpicture / marquee alignment bitfield.
enum class KeyOverlayAlign : int {
    center       = 0,
    left         = 1,
    right        = 2,
    top          = 4,
    bottom       = 8,
    top_left     = 5,   // top|left
    top_right    = 6,   // top|right
    bottom_left  = 9,   // bottom|left
    bottom_right = 10,  // bottom|right
};

enum class KeyOverlayFilter : int {
    shortcuts = 0,  // only chords with modifiers
    printable = 1,  // letters / digits / space / punctuation
    all       = 2,  // every non-modifier key event
};

/// Vertical growth of the chip stack (newest always at the "active" end).
enum class KeyOverlayStack : int {
    automatic = 0,  // bottom-* → up; top-* / center / left / right → down
    up        = 1,  // newest at bottom of stack; history grows upward
    down      = 2,  // newest at top of stack; history grows downward
};

struct KeyOverlayConfig {
    bool enabled = false;
    bool burn_in = false;  // reserved: composite into encoded frames
    KeyOverlayAlign align = KeyOverlayAlign::bottom;
    KeyOverlayStack stack = KeyOverlayStack::automatic;
    int margin_x = 0;      // 0 = auto from short edge
    int margin_y = 0;
    int font_size = 0;     // 0 = auto from short edge
    std::uint32_t color = 0xF8F9FBu;     // text RGB
    std::uint32_t bg_color = 0x181B21u;  // chip fill RGB
    std::uint32_t accent_color = 0xEB445Au;
    float opacity = 0.9f;
    int hold_ms = 1200;
    int fade_ms = 450;
    int max_entries = 5;
    KeyOverlayFilter filter = KeyOverlayFilter::all;
    bool show_modifiers = true;
    // Default visible so chips appear in the MP4 even when the zoom HUD is
    // excluded from capture. Burn-in (composited into frames) is still reserved.
    std::string capture = "visible";  // inherit | auto | exclude | visible
};

struct KeyOverlayLayout {
    int font_size = 18;
    int margin_x = 16;
    int margin_y = 16;
    int line_height = 28;
    int chip_pad_x = 10;
    int stack_gap = 6;
    int max_entries = 5;
};

inline KeyOverlayAlign parse_key_overlay_align(std::string name) noexcept
{
    for (char& c : name) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c == '_') c = '-';
    }
    if (name == "0" || name == "center") return KeyOverlayAlign::center;
    if (name == "1" || name == "left") return KeyOverlayAlign::left;
    if (name == "2" || name == "right") return KeyOverlayAlign::right;
    if (name == "4" || name == "top") return KeyOverlayAlign::top;
    if (name == "8" || name == "bottom") return KeyOverlayAlign::bottom;
    if (name == "5" || name == "top-left" || name == "topleft")
        return KeyOverlayAlign::top_left;
    if (name == "6" || name == "top-right" || name == "topright")
        return KeyOverlayAlign::top_right;
    if (name == "9" || name == "bottom-left" || name == "bottomleft")
        return KeyOverlayAlign::bottom_left;
    if (name == "10" || name == "bottom-right" || name == "bottomright")
        return KeyOverlayAlign::bottom_right;
    return KeyOverlayAlign::bottom;
}

inline const char* key_overlay_align_name(KeyOverlayAlign a) noexcept
{
    switch (a) {
    case KeyOverlayAlign::center: return "center";
    case KeyOverlayAlign::left: return "left";
    case KeyOverlayAlign::right: return "right";
    case KeyOverlayAlign::top: return "top";
    case KeyOverlayAlign::bottom: return "bottom";
    case KeyOverlayAlign::top_left: return "top-left";
    case KeyOverlayAlign::top_right: return "top-right";
    case KeyOverlayAlign::bottom_left: return "bottom-left";
    case KeyOverlayAlign::bottom_right: return "bottom-right";
    }
    return "bottom";
}

inline KeyOverlayFilter parse_key_overlay_filter(std::string name) noexcept
{
    for (char& c : name) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (name == "printable") return KeyOverlayFilter::printable;
    if (name == "all") return KeyOverlayFilter::all;
    return KeyOverlayFilter::shortcuts;
}

inline const char* key_overlay_filter_name(KeyOverlayFilter f) noexcept
{
    switch (f) {
    case KeyOverlayFilter::shortcuts: return "shortcuts";
    case KeyOverlayFilter::printable: return "printable";
    case KeyOverlayFilter::all: return "all";
    }
    return "shortcuts";
}

inline KeyOverlayStack parse_key_overlay_stack(std::string name) noexcept
{
    for (char& c : name) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c == '_') c = '-';
    }
    if (name == "up") return KeyOverlayStack::up;
    if (name == "down") return KeyOverlayStack::down;
    return KeyOverlayStack::automatic;
}

inline const char* key_overlay_stack_name(KeyOverlayStack s) noexcept
{
    switch (s) {
    case KeyOverlayStack::up: return "up";
    case KeyOverlayStack::down: return "down";
    case KeyOverlayStack::automatic:
    default: return "auto";
    }
}

/// True when newest chip should sit at the bottom of the window (history above).
inline bool key_overlay_stack_from_bottom(const KeyOverlayConfig& cfg) noexcept
{
    switch (cfg.stack) {
    case KeyOverlayStack::up: return true;
    case KeyOverlayStack::down: return false;
    case KeyOverlayStack::automatic:
    default:
        // Only true bottom-* aligns stack upward. Center / left / right / top
        // stack downward so chips don't pile through the middle of the frame.
        return (static_cast<int>(cfg.align) & 8) != 0;
    }
}

/// Parse `#RRGGBB` / `0xRRGGBB` / decimal into 0xRRGGBB. On failure keep @p fallback.
inline std::uint32_t parse_rgb_color(const std::string& s,
                                     std::uint32_t fallback) noexcept
{
    if (s.empty()) return fallback;
    std::string t = s;
    if (t[0] == '#') t = t.substr(1);
    if (t.rfind("0x", 0) == 0 || t.rfind("0X", 0) == 0) t = t.substr(2);
    try {
        const auto v = static_cast<std::uint32_t>(std::stoul(t, nullptr, 16));
        return v & 0xFFFFFFu;
    } catch (...) {
        return fallback;
    }
}

inline std::string rgb_to_hex(std::uint32_t rgb) 
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%06X", static_cast<unsigned>(rgb & 0xFFFFFFu));
    return buf;
}

/// Proportional defaults from capture/output short edge (matches zoom-HUD feel).
inline KeyOverlayLayout key_overlay_layout_for_size(int width, int height,
                                                    const KeyOverlayConfig& cfg) noexcept
{
    const int short_edge = std::max(1, std::min(std::max(1, width), std::max(1, height)));
    KeyOverlayLayout lay;
    lay.font_size = cfg.font_size > 0
        ? cfg.font_size
        : std::clamp(static_cast<int>(std::lround(short_edge * 0.022)), 14, 32);
    const int auto_margin =
        std::clamp(static_cast<int>(std::lround(short_edge * 0.012)), 12, 48);
    lay.margin_x = cfg.margin_x > 0 ? cfg.margin_x : auto_margin;
    lay.margin_y = cfg.margin_y > 0 ? cfg.margin_y : auto_margin;
    lay.line_height = lay.font_size + static_cast<int>(std::lround(lay.font_size * 0.35));
    lay.chip_pad_x = static_cast<int>(std::lround(lay.font_size * 0.55));
    lay.stack_gap = static_cast<int>(std::lround(lay.font_size * 0.25));
    lay.max_entries = cfg.max_entries > 0
        ? cfg.max_entries
        : std::clamp(short_edge / 200, 3, 8);
    return lay;
}

/// Anchor a content box of size (cw, ch) inside a parent of size (W, H).
inline void key_overlay_anchor_rect(KeyOverlayAlign align,
                                    int W, int H, int cw, int h,
                                    int margin_x, int margin_y,
                                    int& out_x, int& out_y) noexcept
{
    const int a = static_cast<int>(align);
    const bool left = (a & 1) != 0;
    const bool right = (a & 2) != 0;
    const bool top = (a & 4) != 0;
    const bool bottom = (a & 8) != 0;

    if (left) out_x = margin_x;
    else if (right) out_x = W - margin_x - cw;
    else out_x = (W - cw) / 2;

    if (top) out_y = margin_y;
    else if (bottom) out_y = H - margin_y - h;
    else out_y = (H - h) / 2;

    out_x = std::clamp(out_x, 0, std::max(0, W - cw));
    out_y = std::clamp(out_y, 0, std::max(0, H - h));
}

inline KeyOverlayConfig key_overlay_from_json(const nlohmann::json& j)
{
    KeyOverlayConfig c;
    if (!j.is_object()) return c;
    c.enabled = j.value("keyOverlay", j.value("enabled", false));
    c.burn_in = j.value("keyOverlayBurnIn", j.value("burnIn", false));
    c.align = parse_key_overlay_align(
        j.value("keyOverlayAlign", j.value("align", std::string("bottom"))));
    c.margin_x = static_cast<int>(j.value("keyOverlayMarginX", j.value("marginX", 0)));
    c.margin_y = static_cast<int>(j.value("keyOverlayMarginY", j.value("marginY", 0)));
    c.font_size = static_cast<int>(j.value("keyOverlayFontSize", j.value("fontSize", 0)));
    c.color = parse_rgb_color(
        j.value("keyOverlayColor", j.value("color", std::string("#F8F9FB"))),
        c.color);
    c.bg_color = parse_rgb_color(
        j.value("keyOverlayBgColor", j.value("bgColor", std::string("#181B21"))),
        c.bg_color);
    c.accent_color = parse_rgb_color(
        j.value("keyOverlayAccentColor",
                j.value("accentColor", std::string("#EB445A"))),
        c.accent_color);
    c.opacity = static_cast<float>(
        j.value("keyOverlayOpacity", j.value("opacity", 0.9)));
    c.hold_ms = static_cast<int>(
        j.value("keyOverlayHoldMs", j.value("holdMs", 1200)));
    c.fade_ms = static_cast<int>(
        j.value("keyOverlayFadeMs", j.value("fadeMs", 450)));
    c.max_entries = static_cast<int>(
        j.value("keyOverlayMaxEntries", j.value("maxEntries", 5)));
    c.filter = parse_key_overlay_filter(
        j.value("keyOverlayFilter", j.value("filter", std::string("all"))));
    c.stack = parse_key_overlay_stack(
        j.value("keyOverlayStack", j.value("stack", std::string("auto"))));
    c.show_modifiers = j.value("keyOverlayModifiers", j.value("showModifiers", true));
    c.capture = j.value("keyOverlayCapture", j.value("capture", std::string("visible")));
    c.opacity = std::clamp(c.opacity, 0.0f, 1.0f);
    c.hold_ms = std::clamp(c.hold_ms, 100, 10000);
    c.fade_ms = std::clamp(c.fade_ms, 0, 5000);
    c.max_entries = std::clamp(c.max_entries, 1, 16);
    return c;
}

inline nlohmann::json key_overlay_defaults_json()
{
    return nlohmann::json{
        {"keyOverlay", false},
        {"keyOverlayBurnIn", false},
        {"keyOverlayAlign", "bottom"},
        {"keyOverlayMarginX", 0},
        {"keyOverlayMarginY", 0},
        {"keyOverlayFontSize", 0},
        {"keyOverlayColor", "#F8F9FB"},
        {"keyOverlayBgColor", "#181B21"},
        {"keyOverlayAccentColor", "#EB445A"},
        {"keyOverlayOpacity", 0.9},
        {"keyOverlayHoldMs", 1200},
        {"keyOverlayFadeMs", 450},
        {"keyOverlayMaxEntries", 5},
        {"keyOverlayFilter", "all"},
        {"keyOverlayStack", "auto"},
        {"keyOverlayModifiers", true},
        {"keyOverlayCapture", "visible"},
    };
}

} // namespace pm::video
