//
// video/detect/render — software rasterizer implementation. See render.hpp.
//
#include "pm/video/detect/render.hpp"
#include "pm/video/detect/commons.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace media::vdetect {

// ── Palette ─────────────────────────────────────────────────────────────────
// Mirrors pm::win::kPalette (detect_overlay.cpp) so overlay / still / video agree.
namespace {
constexpr RGB kPalette[] = {
    {255,  80,  80}, { 80, 200,  80}, { 80, 140, 255}, {255, 200,   0},
    {200,  80, 255}, {  0, 220, 220}, {255, 140,   0}, {200, 255,  80},
    {255, 100, 180}, { 80, 255, 200},
};
constexpr int kPaletteN = static_cast<int>(sizeof(kPalette) / sizeof(kPalette[0]));

inline int clampi(int v, int lo, int hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }
} // namespace

RGB class_color(int cls) noexcept {
    if (cls < 0) cls = 0;
    return kPalette[cls % kPaletteN];
}

// ── Geometry ────────────────────────────────────────────────────────────────
void obb_corners(const Detection& d, float out_x[4], float out_y[4]) noexcept {
    const float cx = d.cx(), cy = d.cy();
    const float hw = d.w * 0.5f, hh = d.h * 0.5f;
    const float ca = std::cos(d.angle), sa = std::sin(d.angle);
    const float corn[4][2] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
    for (int i = 0; i < 4; ++i) {
        out_x[i] = cx + corn[i][0] * ca - corn[i][1] * sa;
        out_y[i] = cy + corn[i][0] * sa + corn[i][1] * ca;
    }
}

// ── Primitives ──────────────────────────────────────────────────────────────
void put_pixel(Canvas& c, int x, int y, RGB col) noexcept {
    if (!c.data || x < 0 || y < 0 || x >= c.width || y >= c.height) return;
    uint8_t* p = c.data + static_cast<size_t>(y) * c.stride + static_cast<size_t>(x) * c.channels;
    if (c.bgr) { p[0] = col.b; p[1] = col.g; p[2] = col.r; }
    else       { p[0] = col.r; p[1] = col.g; p[2] = col.b; }
    if (c.channels == 4) p[3] = 255;
}

void blend_pixel(Canvas& c, int x, int y, RGB col, uint8_t a) noexcept {
    if (!c.data || x < 0 || y < 0 || x >= c.width || y >= c.height) return;
    if (a >= 255) { put_pixel(c, x, y, col); return; }
    if (a == 0) return;
    uint8_t* p = c.data + static_cast<size_t>(y) * c.stride + static_cast<size_t>(x) * c.channels;
    const int ia = 255 - a;
    uint8_t r0, g0, b0;
    if (c.bgr) { b0 = p[0]; g0 = p[1]; r0 = p[2]; }
    else       { r0 = p[0]; g0 = p[1]; b0 = p[2]; }
    const uint8_t r = static_cast<uint8_t>((col.r * a + r0 * ia) / 255);
    const uint8_t g = static_cast<uint8_t>((col.g * a + g0 * ia) / 255);
    const uint8_t b = static_cast<uint8_t>((col.b * a + b0 * ia) / 255);
    if (c.bgr) { p[0] = b; p[1] = g; p[2] = r; }
    else       { p[0] = r; p[1] = g; p[2] = b; }
    if (c.channels == 4) p[3] = 255;
}

namespace {
// Stamp a (w x w) square centred on (x,y) — gives thickness to lines/points.
inline void stamp(Canvas& c, int x, int y, RGB col, int w) noexcept {
    if (w <= 1) { put_pixel(c, x, y, col); return; }
    const int h0 = w / 2;
    for (int dy = -h0; dy < w - h0; ++dy)
        for (int dx = -h0; dx < w - h0; ++dx)
            put_pixel(c, x + dx, y + dy, col);
}
} // namespace

void draw_line(Canvas& c, int x0, int y0, int x1, int y1, RGB col, int width) noexcept {
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    const int w = std::max(1, width);
    for (;;) {
        stamp(c, x0, y0, col, w);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void draw_polyline(Canvas& c, const std::vector<std::pair<float, float>>& pts,
                   float sx, float sy, RGB col, int width, bool closed) noexcept {
    if (pts.size() < 2) return;
    auto px = [&](size_t i) { return static_cast<int>(std::lround(pts[i].first  * sx)); };
    auto py = [&](size_t i) { return static_cast<int>(std::lround(pts[i].second * sy)); };
    for (size_t i = 1; i < pts.size(); ++i)
        draw_line(c, px(i - 1), py(i - 1), px(i), py(i), col, width);
    if (closed)
        draw_line(c, px(pts.size() - 1), py(pts.size() - 1), px(0), py(0), col, width);
}

void fill_rect(Canvas& c, int x, int y, int w, int h, RGB col, uint8_t alpha) noexcept {
    const int x0 = clampi(x, 0, c.width), y0 = clampi(y, 0, c.height);
    const int x1 = clampi(x + w, 0, c.width), y1 = clampi(y + h, 0, c.height);
    for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx)
            blend_pixel(c, xx, yy, col, alpha);
}

void draw_rect(Canvas& c, int x, int y, int w, int h, RGB col, int thickness) noexcept {
    const int t = std::max(1, thickness);
    fill_rect(c, x,         y,         w, t, col, 255);   // top
    fill_rect(c, x,         y + h - t, w, t, col, 255);   // bottom
    fill_rect(c, x,         y,         t, h, col, 255);   // left
    fill_rect(c, x + w - t, y,         t, h, col, 255);   // right
}

void fill_disc(Canvas& c, int cx, int cy, int r, RGB col) noexcept {
    const int r2 = r * r;
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx)
            if (dx * dx + dy * dy <= r2) put_pixel(c, cx + dx, cy + dy, col);
}

// ── Embedded 8x8 bitmap font (font8x8_basic, public domain) ─────────────────
// Printable ASCII 0x20..0x7E. Each glyph = 8 rows; bit (1<<col) = left→right.
namespace {
const uint8_t kFont[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // !
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, // "
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, // #
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, // $
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, // %
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, // &
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, // '
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, // (
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, // )
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // *
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, // +
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, // ,
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, // -
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, // .
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, // /
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, // 0
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, // 1
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, // 2
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, // 3
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, // 4
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, // 5
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, // 6
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, // 7
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, // 8
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, // 9
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, // :
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, // ;
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, // <
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, // =
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, // >
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, // ?
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, // @
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, // A
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, // B
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, // C
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, // D
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, // E
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, // F
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, // G
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, // H
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // I
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, // J
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, // K
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, // L
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, // M
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, // N
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, // O
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, // P
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, // Q
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, // R
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, // S
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // T
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, // U
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, // V
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, // W
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, // X
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, // Y
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, // Z
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, // [
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, // backslash
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, // ]
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, // ^
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, // _
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, // `
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, // a
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, // b
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, // c
    {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00}, // d
    {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00}, // e
    {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00}, // f
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, // g
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, // h
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, // i
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, // j
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, // k
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // l
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, // m
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, // n
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, // o
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, // p
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, // q
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, // r
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, // s
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, // t
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, // u
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, // v
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, // w
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, // x
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, // y
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, // z
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, // {
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, // |
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, // }
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, // ~
};
constexpr int kGlyphW = 8, kGlyphH = 8, kAdvance = 8;
} // namespace

std::pair<int, int> text_extent(const std::string& text, int scale) noexcept {
    const int s = std::max(1, scale);
    return {static_cast<int>(text.size()) * kAdvance * s, kGlyphH * s};
}

void draw_text(Canvas& c, int x, int y, const std::string& text, RGB fg, int scale) noexcept {
    const int s = std::max(1, scale);
    int pen = x;
    for (unsigned char ch : text) {
        if (ch >= 0x20 && ch <= 0x7E) {
            const uint8_t* g = kFont[ch - 0x20];
            for (int row = 0; row < kGlyphH; ++row) {
                const uint8_t bits = g[row];
                for (int col = 0; col < kGlyphW; ++col) {
                    if (bits & (1u << col)) {
                        if (s == 1) put_pixel(c, pen + col, y + row, fg);
                        else fill_rect(c, pen + col * s, y + row * s, s, s, fg, 255);
                    }
                }
            }
        }
        pen += kAdvance * s;
    }
}

void draw_label_chip(Canvas& c, int x, int y_bottom, const std::string& text,
                     RGB chip, RGB fg, int scale) noexcept {
    const auto [tw, th] = text_extent(text, scale);
    const int pad = std::max(1, scale);
    int cy = y_bottom - th - pad * 2;
    if (cy < 0) cy = y_bottom;                  // flip below anchor if clipped at top
    fill_rect(c, x, cy, tw + pad * 2, th + pad * 2, chip, 220);
    draw_text(c, x + pad, cy + pad, text, fg, scale);
}

// ── Per-task dispatcher ─────────────────────────────────────────────────────
namespace {
std::string label_text(const Detection& d) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), " %.2f", d.conf);
    return (d.label.empty() ? std::string("?") : d.label) + buf;
}
} // namespace

void draw_detection(Canvas& c, const Detection& d, float sx, float sy,
                    const RenderOptions& opt) noexcept {
    const RGB col = class_color(d.cls);
    const int lw  = std::max(1, opt.line_width);

    int anchor_x = static_cast<int>(std::lround(d.x * sx));
    int anchor_y = static_cast<int>(std::lround(d.y * sy));

    if (d.contour.size() >= 3) {
        // segment → contour outline only
        draw_polyline(c, d.contour, sx, sy, col, std::max(2, lw), /*closed=*/true);
        for (const auto& p : d.contour) {
            const int py = static_cast<int>(std::lround(p.second * sy));
            const int px = static_cast<int>(std::lround(p.first  * sx));
            if (py < anchor_y) { anchor_y = py; anchor_x = px; }
        }
    } else if (!d.keypoints.empty()) {
        // pose → skeleton bones + keypoint dots
        const auto& sk = coco_pose_skeleton();
        const int n = static_cast<int>(d.keypoints.size());
        for (const auto& e : sk) {
            if (e.first >= n || e.second >= n) continue;
            const auto& a = d.keypoints[e.first];
            const auto& b = d.keypoints[e.second];
            if (a.score < opt.kpt_min_score || b.score < opt.kpt_min_score) continue;
            draw_line(c,
                      static_cast<int>(std::lround(a.x * sx)), static_cast<int>(std::lround(a.y * sy)),
                      static_cast<int>(std::lround(b.x * sx)), static_cast<int>(std::lround(b.y * sy)),
                      col, std::max(2, lw));
        }
        const int r = std::max(2, lw + 1);
        for (const auto& k : d.keypoints) {
            if (k.score < opt.kpt_min_score) continue;
            const int kx = static_cast<int>(std::lround(k.x * sx));
            const int ky = static_cast<int>(std::lround(k.y * sy));
            fill_disc(c, kx, ky, r, RGB{255, 255, 255});
            fill_disc(c, kx, ky, std::max(1, r - 1), col);
        }
    } else if (d.oriented) {
        // obb → rotated quad
        float qx[4], qy[4];
        obb_corners(d, qx, qy);
        std::vector<std::pair<float, float>> quad = {
            {qx[0], qy[0]}, {qx[1], qy[1]}, {qx[2], qy[2]}, {qx[3], qy[3]}};
        if (opt.fill) {
            // crude convex fill: scanline over bbox of the quad
            float minx = qx[0], maxx = qx[0], miny = qy[0], maxy = qy[0];
            for (int i = 1; i < 4; ++i) {
                minx = std::min(minx, qx[i]); maxx = std::max(maxx, qx[i]);
                miny = std::min(miny, qy[i]); maxy = std::max(maxy, qy[i]);
            }
            (void)minx; (void)maxx; (void)miny; (void)maxy;  // fill omitted (outline preferred)
        }
        draw_polyline(c, quad, sx, sy, col, lw, /*closed=*/true);
        for (int i = 0; i < 4; ++i) {
            const int py = static_cast<int>(std::lround(qy[i] * sy));
            const int px = static_cast<int>(std::lround(qx[i] * sx));
            if (py < anchor_y) { anchor_y = py; anchor_x = px; }
        }
    } else {
        // detect → axis-aligned box
        const int bx = static_cast<int>(std::lround(d.x * sx));
        const int by = static_cast<int>(std::lround(d.y * sy));
        const int bw = static_cast<int>(std::lround(d.w * sx));
        const int bh = static_cast<int>(std::lround(d.h * sy));
        if (opt.fill) fill_rect(c, bx, by, bw, bh, col, opt.fill_alpha);
        draw_rect(c, bx, by, bw, bh, col, lw);
    }

    if (opt.labels && !d.label.empty())
        draw_label_chip(c, anchor_x, anchor_y, label_text(d), col, RGB{255, 255, 255},
                        opt.font_scale);
}

void draw_detections(Canvas& c, const std::vector<Detection>& dets,
                     float sx, float sy, const RenderOptions& opt) noexcept {
    for (const auto& d : dets) draw_detection(c, d, sx, sy, opt);
}

} // namespace media::vdetect
