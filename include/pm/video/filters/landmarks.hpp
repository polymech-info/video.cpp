#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace pm::video {

struct Landmark {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};

struct LandmarkSet {
    std::vector<Landmark> points;
    std::uint32_t face_count = 0;
    std::uint32_t landmarks_per_face = 0;

    bool empty() const noexcept { return face_count == 0 || points.empty(); }
};

struct IVec2 {
    int x = 0;
    int y = 0;
};

struct IRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    bool empty() const noexcept { return w <= 0 || h <= 0; }
};

inline IRect clamp_rect(IRect r, int frame_w, int frame_h) {
    if (frame_w <= 0 || frame_h <= 0) return {};
    int x0 = r.x;
    int y0 = r.y;
    int x1 = r.x + r.w;
    int y1 = r.y + r.h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > frame_w) x1 = frame_w;
    if (y1 > frame_h) y1 = frame_h;
    if (x1 <= x0 || y1 <= y0) return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

inline IRect bounds_of(const IVec2* pts, int n, int pad, int frame_w, int frame_h) {
    if (!pts || n <= 0) return {};
    int x0 = pts[0].x, y0 = pts[0].y, x1 = x0, y1 = y0;
    for (int i = 1; i < n; ++i) {
        if (pts[i].x < x0) x0 = pts[i].x;
        if (pts[i].y < y0) y0 = pts[i].y;
        if (pts[i].x > x1) x1 = pts[i].x;
        if (pts[i].y > y1) y1 = pts[i].y;
    }
    return clamp_rect({x0 - pad, y0 - pad, (x1 - x0 + 1) + pad * 2,
                       (y1 - y0 + 1) + pad * 2},
                      frame_w, frame_h);
}

inline IRect union_rect(IRect a, IRect b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const int x0 = a.x < b.x ? a.x : b.x;
    const int y0 = a.y < b.y ? a.y : b.y;
    const int x1 = (a.x + a.w) > (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    const int y1 = (a.y + a.h) > (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    return {x0, y0, x1 - x0, y1 - y0};
}

inline void translate_pts(std::vector<IVec2>& pts, int dx, int dy) {
    for (auto& p : pts) {
        p.x += dx;
        p.y += dy;
    }
}

// MediaPipe Face Mesh lip arcs (same topology as SegmeCam ApplyLipRefinerBGR).
// OUTER_UP goes through 17 (lower outer); OUTER_LO through 0 (upper outer).
inline constexpr int kLipOuterUp[] = {61, 146, 91, 181, 84, 17, 314, 405, 321, 375, 291};
inline constexpr int kLipOuterLo[] = {61, 185, 40, 39, 37, 0, 267, 269, 270, 409, 291};
inline constexpr int kLipInnerUp[] = {78, 95, 88, 178, 87, 14, 317, 402, 318, 324, 308};
inline constexpr int kLipInnerLo[] = {78, 191, 80, 81, 82, 13, 312, 311, 310, 415, 308};
inline constexpr int kFaceOval[] = {
    10,  338, 297, 332, 284, 251, 389, 356, 454, 323, 361, 288,
    397, 365, 379, 378, 400, 377, 152, 148, 176, 149, 150, 136,
    172, 58,  132, 93,  234, 127, 162, 21,  54,  103, 67,  109};
inline constexpr int kLeftEye[] = {33,  7,   163, 144, 145, 153, 154, 155,
                                   133, 173, 157, 158, 159, 160, 161, 246};
inline constexpr int kRightEye[] = {263, 249, 390, 373, 374, 380, 381, 382,
                                    362, 398, 384, 385, 386, 387, 388, 466};
// Lower lid + infraorbital cheek (468-pt Face Mesh).
inline constexpr int kLeftUnderEye[] = {33,  7,   163, 144, 145, 153, 154, 155,
                                        133, 128, 121, 120, 119, 118, 117, 111};
inline constexpr int kRightUnderEye[] = {362, 382, 381, 380, 374, 373, 390, 249,
                                         263, 340, 346, 347, 348, 349, 350, 357};

inline IVec2 landmark_px(const Landmark& p, int width, int height) {
    auto coord = [](float v, int extent) {
        if (extent <= 0) return 0;
        if (v >= -0.25f && v <= 1.25f)
            return static_cast<int>(std::lround(v * static_cast<float>(extent - 1)));
        return static_cast<int>(std::lround(v));
    };
    IVec2 out;
    out.x = coord(p.x, width);
    out.y = coord(p.y, height);
    if (out.x < 0) out.x = 0;
    if (out.y < 0) out.y = 0;
    if (width > 0 && out.x >= width) out.x = width - 1;
    if (height > 0 && out.y >= height) out.y = height - 1;
    return out;
}

inline const Landmark* face_begin(const LandmarkSet& set, std::uint32_t face) {
    if (face >= set.face_count || set.landmarks_per_face == 0) return nullptr;
    const std::size_t off =
        static_cast<std::size_t>(face) * set.landmarks_per_face;
    if (off >= set.points.size()) return nullptr;
    return set.points.data() + off;
}

template <std::size_t N>
inline bool lip_arc_px(const Landmark* face, std::uint32_t per_face, int width,
                       int height, const int (&idx)[N], std::vector<IVec2>& out) {
    out.clear();
    if (!face || per_face == 0) return false;
    out.reserve(N);
    for (std::size_t i = 0; i < N; ++i) {
        if (static_cast<std::uint32_t>(idx[i]) >= per_face) {
            out.clear();
            return false;
        }
        out.push_back(landmark_px(face[idx[i]], width, height));
    }
    return !out.empty();
}

} // namespace pm::video
