#pragma once
//
// video/detect/render — shared software rasterizer for detection overlays.
//
// One per-task rasterizer drawing the *characteristic geometry* of each YOLO
// head onto a packed byte buffer:
//   detect  → axis-aligned box
//   obb     → rotated quad
//   pose    → skeleton bones + keypoint dots
//   segment → contour outline (no mask fill)
// plus a small label chip (embedded 8x8 bitmap font, no font deps).
//
// Pure C++, zero-dependency (no GDI+, no libvips, no OpenCV). The same code
// powers:
//   • saved annotated stills   (load image → Canvas → draw → write)
//   • burn-in video frames     (decoded BGR frame → Canvas → draw → encode)
//   • a future StreamSink      (Sunshine-style window/screen stream → same frames)
// The Win32 GDI+ live overlay keeps its own present path but can reuse the
// geometry helpers here (obb_corners, class_color, coco_pose_skeleton) so there
// is a single source of truth for shapes/colours.
//
// Coordinate convention: detection coords are ORIGINAL image pixels (decoders
// already unmap the letterbox). (sx,sy) scales those onto the target Canvas;
// pass (1,1) when the canvas matches the original image size.
//
#include "pm/video/detect/types.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace media::vdetect {

// ── Colour ──────────────────────────────────────────────────────────────────
struct RGB {
    uint8_t r = 0, g = 0, b = 0;
};

/// Stable per-class colour. Matches the live overlay palette so stills, video
/// and the on-screen overlay agree.
RGB class_color(int cls) noexcept;

// ── Target surface ──────────────────────────────────────────────────────────
/// A mutable image buffer to draw onto. Rows are `stride` bytes apart; pixels
/// are `channels` bytes (3 = packed RGB/BGR, 4 = RGBA/BGRA). `bgr` selects byte
/// order — BGR24 for native video frames, RGB for libvips sRGB buffers. When
/// channels == 4 the alpha byte of any touched pixel is forced to 255.
struct Canvas {
    uint8_t* data     = nullptr;
    int      width    = 0;
    int      height   = 0;
    int      stride   = 0;
    int      channels = 3;
    bool     bgr      = false;
};

// ── Render options ──────────────────────────────────────────────────────────
struct RenderOptions {
    bool    labels        = true;
    int     line_width    = 2;     ///< stroke width (px)
    int     font_scale    = 2;     ///< integer scale of the 8x8 glyph cell
    bool    fill          = false; ///< translucent fill for box/quad
    uint8_t fill_alpha    = 64;    ///< 0..255, used when fill
    float   kpt_min_score = 0.30f; ///< hide keypoints/bones below this
};

// ── Geometry helpers (single source of truth) ───────────────────────────────
/// Four corners (original-pixel space) of an OBB detection, in order
/// TL,TR,BR,BL relative to the box's own axes.
void obb_corners(const Detection& d, float out_x[4], float out_y[4]) noexcept;

// ── Primitives (all clipped to the canvas) ──────────────────────────────────
void put_pixel(Canvas& c, int x, int y, RGB col) noexcept;
void blend_pixel(Canvas& c, int x, int y, RGB col, uint8_t alpha) noexcept;
void draw_line(Canvas& c, int x0, int y0, int x1, int y1, RGB col, int width) noexcept;
void draw_polyline(Canvas& c, const std::vector<std::pair<float, float>>& pts,
                   float sx, float sy, RGB col, int width, bool closed) noexcept;
void draw_rect(Canvas& c, int x, int y, int w, int h, RGB col, int thickness) noexcept;
void fill_rect(Canvas& c, int x, int y, int w, int h, RGB col, uint8_t alpha) noexcept;
void fill_disc(Canvas& c, int cx, int cy, int r, RGB col) noexcept;

/// Pixel size of `text` at `scale` (8x8 cell). {w,h}.
std::pair<int, int> text_extent(const std::string& text, int scale) noexcept;
/// Draw text with top-left origin (x,y). No background.
void draw_text(Canvas& c, int x, int y, const std::string& text, RGB fg, int scale) noexcept;
/// Coloured chip (filled rect) with light text, drawn so its *bottom* sits at
/// (x, y_bottom) — i.e. above the anchor like the live overlay.
void draw_label_chip(Canvas& c, int x, int y_bottom, const std::string& text,
                     RGB chip, RGB fg, int scale) noexcept;

// ── Per-task dispatcher ─────────────────────────────────────────────────────
/// Draw one detection's characteristic geometry + label.
void draw_detection(Canvas& c, const Detection& d, float sx, float sy,
                    const RenderOptions& opt) noexcept;
/// Draw a whole result set (skips entries below opt thresholds handled upstream).
void draw_detections(Canvas& c, const std::vector<Detection>& dets,
                     float sx, float sy, const RenderOptions& opt) noexcept;

} // namespace media::vdetect
