#include "pm/video/filters/face_effects.hpp"

#include "pm/video/filters/image_ops.hpp"
#include "pm/video/filters/skin_gpu.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace pm::video {
namespace {

void append_reversed(std::vector<IVec2>& poly, const std::vector<IVec2>& inner) {
    for (int i = static_cast<int>(inner.size()) - 1; i >= 0; --i)
        poly.push_back(inner[static_cast<std::size_t>(i)]);
}

bool make_band(const Landmark* face, std::uint32_t per_face, int w, int h,
               const int* outer, int outer_n, const int* inner, int inner_n,
               std::vector<IVec2>& poly) {
    std::vector<IVec2> o;
    std::vector<IVec2> i;
    o.reserve(static_cast<std::size_t>(outer_n));
    i.reserve(static_cast<std::size_t>(inner_n));
    for (int k = 0; k < outer_n; ++k) {
        if (static_cast<std::uint32_t>(outer[k]) >= per_face) return false;
        o.push_back(landmark_px(face[outer[k]], w, h));
    }
    for (int k = 0; k < inner_n; ++k) {
        if (static_cast<std::uint32_t>(inner[k]) >= per_face) return false;
        i.push_back(landmark_px(face[inner[k]], w, h));
    }
    poly = o;
    append_reversed(poly, i);
    return poly.size() >= 6;
}

IRect bounds_of_polys(const std::vector<std::vector<IVec2>>& polys, int pad,
                      int frame_w, int frame_h) {
    IRect out{};
    for (const auto& p : polys) {
        if (p.empty()) continue;
        out = union_rect(out, bounds_of(p.data(), static_cast<int>(p.size()), pad,
                                        frame_w, frame_h));
    }
    return out;
}

void fill_roi(std::uint8_t* mask, const IRect& roi, std::vector<IVec2> poly,
              std::uint8_t value) {
    if (roi.empty() || poly.empty()) return;
    translate_pts(poly, -roi.x, -roi.y);
    fill_polygon_u8(mask, roi.w, roi.h, poly.data(), static_cast<int>(poly.size()),
                    value);
}

bool mask_any(const std::uint8_t* mask, int n) {
    for (int i = 0; i < n; ++i) {
        if (mask[i]) return true;
    }
    return false;
}

void copy_bgr_roi(const VideoFrame& frame, const IRect& roi,
                  std::vector<std::uint8_t>& crop) {
    const int stride = frame.stride > 0 ? frame.stride : frame.width * 3;
    crop.resize(static_cast<std::size_t>(roi.w) * roi.h * 3u);
    for (int y = 0; y < roi.h; ++y) {
        const std::uint8_t* src =
            frame.data.data() + static_cast<std::size_t>(roi.y + y) * stride +
            static_cast<std::size_t>(roi.x) * 3u;
        std::memcpy(crop.data() + static_cast<std::size_t>(y) * roi.w * 3, src,
                    static_cast<std::size_t>(roi.w) * 3u);
    }
}

void box_blur_bgr(std::uint8_t* bgr, int w, int h, int radius) {
    if (!bgr || w <= 0 || h <= 0 || radius <= 0) return;
    const int k = radius * 2 + 1;
    std::vector<std::uint8_t> tmp(static_cast<std::size_t>(w) * h * 3u);

    for (int y = 0; y < h; ++y) {
        int acc[3] = {0, 0, 0};
        for (int i = -radius; i <= radius; ++i) {
            const int x = std::clamp(i, 0, w - 1);
            const std::uint8_t* p =
                bgr + static_cast<std::size_t>(y) * w * 3 + static_cast<std::size_t>(x) * 3u;
            acc[0] += p[0];
            acc[1] += p[1];
            acc[2] += p[2];
        }
        for (int x = 0; x < w; ++x) {
            std::uint8_t* d =
                tmp.data() + static_cast<std::size_t>(y) * w * 3 + static_cast<std::size_t>(x) * 3u;
            d[0] = static_cast<std::uint8_t>((acc[0] + k / 2) / k);
            d[1] = static_cast<std::uint8_t>((acc[1] + k / 2) / k);
            d[2] = static_cast<std::uint8_t>((acc[2] + k / 2) / k);
            const int x_out = std::clamp(x - radius, 0, w - 1);
            const int x_in = std::clamp(x + radius + 1, 0, w - 1);
            const std::uint8_t* po =
                bgr + static_cast<std::size_t>(y) * w * 3 + static_cast<std::size_t>(x_out) * 3u;
            const std::uint8_t* pi =
                bgr + static_cast<std::size_t>(y) * w * 3 + static_cast<std::size_t>(x_in) * 3u;
            acc[0] += static_cast<int>(pi[0]) - static_cast<int>(po[0]);
            acc[1] += static_cast<int>(pi[1]) - static_cast<int>(po[1]);
            acc[2] += static_cast<int>(pi[2]) - static_cast<int>(po[2]);
        }
    }

    for (int x = 0; x < w; ++x) {
        int acc[3] = {0, 0, 0};
        for (int i = -radius; i <= radius; ++i) {
            const int y = std::clamp(i, 0, h - 1);
            const std::uint8_t* p =
                tmp.data() + static_cast<std::size_t>(y) * w * 3 + static_cast<std::size_t>(x) * 3u;
            acc[0] += p[0];
            acc[1] += p[1];
            acc[2] += p[2];
        }
        for (int y = 0; y < h; ++y) {
            std::uint8_t* d =
                bgr + static_cast<std::size_t>(y) * w * 3 + static_cast<std::size_t>(x) * 3u;
            d[0] = static_cast<std::uint8_t>((acc[0] + k / 2) / k);
            d[1] = static_cast<std::uint8_t>((acc[1] + k / 2) / k);
            d[2] = static_cast<std::uint8_t>((acc[2] + k / 2) / k);
            const int y_out = std::clamp(y - radius, 0, h - 1);
            const int y_in = std::clamp(y + radius + 1, 0, h - 1);
            const std::uint8_t* po =
                tmp.data() + static_cast<std::size_t>(y_out) * w * 3 +
                static_cast<std::size_t>(x) * 3u;
            const std::uint8_t* pi =
                tmp.data() + static_cast<std::size_t>(y_in) * w * 3 +
                static_cast<std::size_t>(x) * 3u;
            acc[0] += static_cast<int>(pi[0]) - static_cast<int>(po[0]);
            acc[1] += static_cast<int>(pi[1]) - static_cast<int>(po[1]);
            acc[2] += static_cast<int>(pi[2]) - static_cast<int>(po[2]);
        }
    }
}

void downsample2_bgr(const std::uint8_t* src, int sw, int sh,
                     std::vector<std::uint8_t>& dst, int& dw, int& dh) {
    dw = std::max(1, sw / 2);
    dh = std::max(1, sh / 2);
    dst.assign(static_cast<std::size_t>(dw) * dh * 3u, 0);
    for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
            const int x0 = x * 2;
            const int y0 = y * 2;
            const int x1 = std::min(x0 + 1, sw - 1);
            const int y1 = std::min(y0 + 1, sh - 1);
            int acc[3] = {0, 0, 0};
            int n = 0;
            for (int yy = y0; yy <= y1; ++yy) {
                for (int xx = x0; xx <= x1; ++xx) {
                    const std::uint8_t* p =
                        src + static_cast<std::size_t>(yy) * sw * 3 +
                        static_cast<std::size_t>(xx) * 3u;
                    acc[0] += p[0];
                    acc[1] += p[1];
                    acc[2] += p[2];
                    ++n;
                }
            }
            std::uint8_t* d =
                dst.data() + static_cast<std::size_t>(y) * dw * 3 +
                static_cast<std::size_t>(x) * 3u;
            d[0] = static_cast<std::uint8_t>((acc[0] + n / 2) / n);
            d[1] = static_cast<std::uint8_t>((acc[1] + n / 2) / n);
            d[2] = static_cast<std::uint8_t>((acc[2] + n / 2) / n);
        }
    }
}

void upsample_bgr(const std::uint8_t* src, int sw, int sh, std::uint8_t* dst,
                  int dw, int dh) {
    if (sw <= 1 || sh <= 1) {
        for (int y = 0; y < dh; ++y) {
            for (int x = 0; x < dw; ++x) {
                const std::uint8_t* p = src;
                std::uint8_t* d =
                    dst + static_cast<std::size_t>(y) * dw * 3 +
                    static_cast<std::size_t>(x) * 3u;
                d[0] = p[0];
                d[1] = p[1];
                d[2] = p[2];
            }
        }
        return;
    }
    const float sx = static_cast<float>(sw - 1) / static_cast<float>(dw - 1);
    const float sy = static_cast<float>(sh - 1) / static_cast<float>(dh - 1);
    for (int y = 0; y < dh; ++y) {
        const float fy = static_cast<float>(y) * sy;
        const int y0 = static_cast<int>(fy);
        const int y1 = std::min(y0 + 1, sh - 1);
        const float ty = fy - static_cast<float>(y0);
        for (int x = 0; x < dw; ++x) {
            const float fx = static_cast<float>(x) * sx;
            const int x0 = static_cast<int>(fx);
            const int x1 = std::min(x0 + 1, sw - 1);
            const float tx = fx - static_cast<float>(x0);
            const std::uint8_t* p00 =
                src + static_cast<std::size_t>(y0) * sw * 3 + static_cast<std::size_t>(x0) * 3u;
            const std::uint8_t* p10 =
                src + static_cast<std::size_t>(y0) * sw * 3 + static_cast<std::size_t>(x1) * 3u;
            const std::uint8_t* p01 =
                src + static_cast<std::size_t>(y1) * sw * 3 + static_cast<std::size_t>(x0) * 3u;
            const std::uint8_t* p11 =
                src + static_cast<std::size_t>(y1) * sw * 3 + static_cast<std::size_t>(x1) * 3u;
            std::uint8_t* d =
                dst + static_cast<std::size_t>(y) * dw * 3 + static_cast<std::size_t>(x) * 3u;
            for (int c = 0; c < 3; ++c) {
                const float a = static_cast<float>(p00[c]) * (1.f - tx) +
                                static_cast<float>(p10[c]) * tx;
                const float b = static_cast<float>(p01[c]) * (1.f - tx) +
                                static_cast<float>(p11[c]) * tx;
                d[c] = static_cast<std::uint8_t>(
                    std::clamp(a * (1.f - ty) + b * ty + 0.5f, 0.f, 255.f));
            }
        }
    }
}

bool build_oval_skin_mask(const LandmarkSet& set, int w, int h, int pad,
                          IRect& roi, std::vector<std::uint8_t>& mask) {
    struct FacePolys {
        std::vector<IVec2> oval;
        std::vector<IVec2> lip_top;
        std::vector<IVec2> lip_bot;
        std::vector<IVec2> leye;
        std::vector<IVec2> reye;
    };
    std::vector<FacePolys> faces;
    std::vector<std::vector<IVec2>> ovals;
    for (std::uint32_t fi = 0; fi < set.face_count; ++fi) {
        const Landmark* face = face_begin(set, fi);
        if (!face) continue;
        FacePolys fp;
        if (!lip_arc_px(face, set.landmarks_per_face, w, h, kFaceOval, fp.oval))
            continue;
        make_band(face, set.landmarks_per_face, w, h, kLipOuterUp, 11, kLipInnerUp,
                  11, fp.lip_top);
        make_band(face, set.landmarks_per_face, w, h, kLipOuterLo, 11, kLipInnerLo,
                  11, fp.lip_bot);
        lip_arc_px(face, set.landmarks_per_face, w, h, kLeftEye, fp.leye);
        lip_arc_px(face, set.landmarks_per_face, w, h, kRightEye, fp.reye);
        ovals.push_back(fp.oval);
        faces.push_back(std::move(fp));
    }
    roi = bounds_of_polys(ovals, pad, w, h);
    if (roi.empty()) return false;
    mask.assign(static_cast<std::size_t>(roi.w) * roi.h, 0);
    for (const auto& fp : faces) {
        fill_roi(mask.data(), roi, fp.oval, 220);
        fill_roi(mask.data(), roi, fp.lip_top, 0);
        fill_roi(mask.data(), roi, fp.lip_bot, 0);
        fill_roi(mask.data(), roi, fp.leye, 0);
        fill_roi(mask.data(), roi, fp.reye, 0);
    }
    feather_gaussian_u8(mask.data(), roi.w, roi.h, 9);
    return mask_any(mask.data(), roi.w * roi.h);
}

} // namespace

void apply_lip_tint_bgr(VideoFrame& frame, const LandmarkSet& set,
                        const BeautyConfig& cfg) {
    const float strength = std::clamp(cfg.lips_alpha, 0.f, 1.f);
    if (strength <= 0.f || set.empty() || frame.data.empty()) return;
    if (frame.width <= 0 || frame.height <= 0) return;

    std::uint8_t tr = 0xC4, tg = 0x5C, tb = 0x6A;
    parse_hex_rgb(cfg.lips_color, tr, tg, tb);

    const int w = frame.width;
    const int h = frame.height;
    const int stride = frame.stride > 0 ? frame.stride : w * 3;
    const int band = cfg.lips_band_px > 0 ? cfg.lips_band_px : 4;
    const int feather = cfg.lips_feather_px > 0 ? cfg.lips_feather_px : 6;

    std::vector<std::vector<IVec2>> bands;
    for (std::uint32_t fi = 0; fi < set.face_count; ++fi) {
        const Landmark* face = face_begin(set, fi);
        if (!face) continue;
        std::vector<IVec2> top;
        std::vector<IVec2> bot;
        if (make_band(face, set.landmarks_per_face, w, h, kLipOuterUp, 11,
                      kLipInnerUp, 11, top))
            bands.push_back(std::move(top));
        if (make_band(face, set.landmarks_per_face, w, h, kLipOuterLo, 11,
                      kLipInnerLo, 11, bot))
            bands.push_back(std::move(bot));
    }
    const IRect roi = bounds_of_polys(bands, band + feather + 2, w, h);
    if (roi.empty()) return;

    std::vector<std::uint8_t> mask(static_cast<std::size_t>(roi.w) * roi.h, 0);
    for (const auto& band_pts : bands)
        fill_roi(mask.data(), roi, band_pts, 255);
    if (band > 0)
        dilate_u8(mask.data(), roi.w, roi.h, std::max(1, band / 2));
    if (feather > 0) {
        feather_gaussian_u8(mask.data(), roi.w, roi.h, feather);
        feather_gaussian_u8(mask.data(), roi.w, roi.h, feather);
    }
    if (!mask_any(mask.data(), roi.w * roi.h)) return;

    float tL = 0, tA = 0, tB = 0;
    bgr_to_lab_u8(tb, tg, tr, tL, tA, tB);

    for (int y = 0; y < roi.h; ++y) {
        std::uint8_t* row =
            frame.data.data() + static_cast<std::size_t>(roi.y + y) * stride;
        const std::uint8_t* mrow = mask.data() + static_cast<std::size_t>(y) * roi.w;
        for (int x = 0; x < roi.w; ++x) {
            const float wgt = (static_cast<float>(mrow[x]) / 255.f) * strength;
            if (wgt <= 1e-4f) continue;
            std::uint8_t* p = row + (roi.x + x) * 3;
            float L, A, B;
            bgr_to_lab_u8(p[0], p[1], p[2], L, A, B);
            A = A * (1.f - wgt) + tA * wgt;
            B = B * (1.f - wgt) + tB * wgt;
            lab_to_bgr_u8(L, A, B, p[0], p[1], p[2]);
        }
    }
}

void apply_teeth_whiten_bgr(VideoFrame& frame, const LandmarkSet& set,
                            const BeautyConfig& cfg) {
    const float strength = std::clamp(cfg.teeth_strength, 0.f, 1.f);
    if (strength <= 0.f || set.empty() || frame.data.empty()) return;
    if (frame.width <= 0 || frame.height <= 0) return;

    const int w = frame.width;
    const int h = frame.height;
    const int stride = frame.stride > 0 ? frame.stride : w * 3;
    const int margin = cfg.teeth_margin_px > 0 ? cfg.teeth_margin_px : 3;

    std::vector<std::vector<IVec2>> openings;
    for (std::uint32_t fi = 0; fi < set.face_count; ++fi) {
        const Landmark* face = face_begin(set, fi);
        if (!face) continue;
        std::vector<IVec2> opening;
        if (!make_band(face, set.landmarks_per_face, w, h, kLipInnerUp, 11,
                       kLipInnerLo, 11, opening))
            continue;
        openings.push_back(std::move(opening));
    }
    const IRect roi = bounds_of_polys(openings, margin + 6, w, h);
    if (roi.empty()) return;

    std::vector<std::uint8_t> mask(static_cast<std::size_t>(roi.w) * roi.h, 0);
    for (const auto& opening : openings)
        fill_roi(mask.data(), roi, opening, 255);
    if (margin > 0)
        erode_u8(mask.data(), roi.w, roi.h, std::max(1, margin / 2));
    feather_gaussian_u8(mask.data(), roi.w, roi.h, 5);
    if (!mask_any(mask.data(), roi.w * roi.h)) return;

    const float k = 0.35f * strength;
    const float kL = 0.15f * strength;
    for (int y = 0; y < roi.h; ++y) {
        std::uint8_t* row =
            frame.data.data() + static_cast<std::size_t>(roi.y + y) * stride;
        const std::uint8_t* mrow = mask.data() + static_cast<std::size_t>(y) * roi.w;
        for (int x = 0; x < roi.w; ++x) {
            const float wgt = static_cast<float>(mrow[x]) / 255.f;
            if (wgt <= 1e-4f) continue;
            std::uint8_t* p = row + (roi.x + x) * 3;
            float L, A, B;
            bgr_to_lab_u8(p[0], p[1], p[2], L, A, B);
            B = 128.f + (B - 128.f) * (1.f - k * wgt);
            L = L * (1.f + kL * wgt);
            lab_to_bgr_u8(L, A, B, p[0], p[1], p[2]);
        }
    }
}

void apply_skin_smooth_bgr(VideoFrame& frame, const LandmarkSet& set,
                           const BeautyConfig& cfg, SkinGpu* gpu) {
    const float strength = std::clamp(cfg.skin_strength, 0.f, 1.f);
    if (strength <= 0.f || set.empty() || frame.data.empty()) return;
    if (frame.width <= 0 || frame.height <= 0) return;

    const int w = frame.width;
    const int h = frame.height;
    const int stride = frame.stride > 0 ? frame.stride : w * 3;

    IRect roi{};
    std::vector<std::uint8_t> mask;
    if (!build_oval_skin_mask(set, w, h, 16, roi, mask)) return;

    if (gpu && gpu->valid() && gpu->smooth_roi(frame, roi, mask.data(), strength))
        return;

    std::vector<std::uint8_t> crop;
    copy_bgr_roi(frame, roi, crop);

    int sw = 0, sh = 0;
    std::vector<std::uint8_t> small;
    downsample2_bgr(crop.data(), roi.w, roi.h, small, sw, sh);
    const int radius = 2 + static_cast<int>(std::lround(2.f * strength));
    box_blur_bgr(small.data(), sw, sh, radius);

    std::vector<std::uint8_t> blur(static_cast<std::size_t>(roi.w) * roi.h * 3u);
    upsample_bgr(small.data(), sw, sh, blur.data(), roi.w, roi.h);

    for (int y = 0; y < roi.h; ++y) {
        std::uint8_t* drow =
            frame.data.data() + static_cast<std::size_t>(roi.y + y) * stride +
            static_cast<std::size_t>(roi.x) * 3u;
        const std::uint8_t* brow =
            blur.data() + static_cast<std::size_t>(y) * roi.w * 3;
        const std::uint8_t* mrow = mask.data() + static_cast<std::size_t>(y) * roi.w;
        for (int x = 0; x < roi.w; ++x) {
            const float wgt = (static_cast<float>(mrow[x]) / 255.f) * strength;
            if (wgt <= 1e-4f) continue;
            std::uint8_t* p = drow + x * 3;
            const std::uint8_t* b = brow + x * 3;
            for (int c = 0; c < 3; ++c) {
                const float o = static_cast<float>(p[c]) * (1.f - wgt) +
                                static_cast<float>(b[c]) * wgt;
                p[c] = static_cast<std::uint8_t>(std::clamp(o + 0.5f, 0.f, 255.f));
            }
        }
    }
}

void apply_face_grade_bgr(VideoFrame& frame, const LandmarkSet& set,
                          const BeautyConfig& cfg) {
    const float gain = std::clamp(cfg.face_gain, 0.f, 1.f);
    const float warm = std::clamp(cfg.warmth, 0.f, 1.f);
    if ((gain <= 0.f && warm <= 0.f) || set.empty() || frame.data.empty()) return;
    if (frame.width <= 0 || frame.height <= 0) return;

    IRect roi{};
    std::vector<std::uint8_t> mask;
    if (!build_oval_skin_mask(set, frame.width, frame.height, 12, roi, mask))
        return;

    const int stride = frame.stride > 0 ? frame.stride : frame.width * 3;
    constexpr float kTargetL = 152.f;
    for (int y = 0; y < roi.h; ++y) {
        std::uint8_t* row =
            frame.data.data() + static_cast<std::size_t>(roi.y + y) * stride;
        const std::uint8_t* mrow = mask.data() + static_cast<std::size_t>(y) * roi.w;
        for (int x = 0; x < roi.w; ++x) {
            const float wgt = static_cast<float>(mrow[x]) / 255.f;
            if (wgt <= 1e-4f) continue;
            std::uint8_t* p = row + (roi.x + x) * 3;
            float L, A, B;
            bgr_to_lab_u8(p[0], p[1], p[2], L, A, B);
            if (gain > 0.f)
                L = L + (kTargetL - L) * (0.55f * gain * wgt);
            if (warm > 0.f) {
                A = A + 5.f * warm * wgt;
                B = B + 16.f * warm * wgt;
            }
            lab_to_bgr_u8(L, A, B, p[0], p[1], p[2]);
        }
    }
}

void apply_redness_mute_bgr(VideoFrame& frame, const LandmarkSet& set,
                            const BeautyConfig& cfg) {
    const float strength = std::clamp(cfg.redness_blend, 0.f, 1.f);
    if (!cfg.redness || strength <= 0.f || set.empty() || frame.data.empty())
        return;
    if (frame.width <= 0 || frame.height <= 0) return;

    IRect roi{};
    std::vector<std::uint8_t> mask;
    if (!build_oval_skin_mask(set, frame.width, frame.height, 12, roi, mask))
        return;

    const int stride = frame.stride > 0 ? frame.stride : frame.width * 3;
    for (int y = 0; y < roi.h; ++y) {
        std::uint8_t* row =
            frame.data.data() + static_cast<std::size_t>(roi.y + y) * stride;
        const std::uint8_t* mrow = mask.data() + static_cast<std::size_t>(y) * roi.w;
        for (int x = 0; x < roi.w; ++x) {
            const float wgt = (static_cast<float>(mrow[x]) / 255.f) * strength;
            if (wgt <= 1e-4f) continue;
            std::uint8_t* p = row + (roi.x + x) * 3;
            float L, A, B;
            bgr_to_lab_u8(p[0], p[1], p[2], L, A, B);
            if (A > 128.f)
                A = 128.f + (A - 128.f) * (1.f - wgt);
            lab_to_bgr_u8(L, A, B, p[0], p[1], p[2]);
        }
    }
}

void apply_undereye_lift_bgr(VideoFrame& frame, const LandmarkSet& set,
                             const BeautyConfig& cfg) {
    const float strength = std::clamp(cfg.undereye_blend, 0.f, 1.f);
    if (!cfg.undereye || strength <= 0.f || set.empty() || frame.data.empty())
        return;
    if (frame.width <= 0 || frame.height <= 0) return;

    const int w = frame.width;
    const int h = frame.height;
    const int stride = frame.stride > 0 ? frame.stride : w * 3;

    std::vector<std::vector<IVec2>> bands;
    for (std::uint32_t fi = 0; fi < set.face_count; ++fi) {
        const Landmark* face = face_begin(set, fi);
        if (!face) continue;
        std::vector<IVec2> left;
        std::vector<IVec2> right;
        if (lip_arc_px(face, set.landmarks_per_face, w, h, kLeftUnderEye, left))
            bands.push_back(std::move(left));
        if (lip_arc_px(face, set.landmarks_per_face, w, h, kRightUnderEye, right))
            bands.push_back(std::move(right));
    }
    const IRect roi = bounds_of_polys(bands, 14, w, h);
    if (roi.empty()) return;

    std::vector<std::uint8_t> mask(static_cast<std::size_t>(roi.w) * roi.h, 0);
    for (const auto& band : bands)
        fill_roi(mask.data(), roi, band, 255);
    dilate_u8(mask.data(), roi.w, roi.h, 2);
    feather_gaussian_u8(mask.data(), roi.w, roi.h, 11);
    feather_gaussian_u8(mask.data(), roi.w, roi.h, 9);
    if (!mask_any(mask.data(), roi.w * roi.h)) return;

    std::vector<std::uint8_t> crop;
    copy_bgr_roi(frame, roi, crop);
    box_blur_bgr(crop.data(), roi.w, roi.h, 2);

    for (int y = 0; y < roi.h; ++y) {
        std::uint8_t* drow =
            frame.data.data() + static_cast<std::size_t>(roi.y + y) * stride +
            static_cast<std::size_t>(roi.x) * 3u;
        const std::uint8_t* brow =
            crop.data() + static_cast<std::size_t>(y) * roi.w * 3;
        const std::uint8_t* mrow = mask.data() + static_cast<std::size_t>(y) * roi.w;
        for (int x = 0; x < roi.w; ++x) {
            const float wgt = (static_cast<float>(mrow[x]) / 255.f) * strength;
            if (wgt <= 1e-4f) continue;
            std::uint8_t* p = drow + x * 3;
            const std::uint8_t* b = brow + x * 3;
            float L, A, B;
            bgr_to_lab_u8(p[0], p[1], p[2], L, A, B);
            L = L + 22.f * wgt;
            std::uint8_t lb, lg, lr;
            lab_to_bgr_u8(L, A, B, lb, lg, lr);
            const float mix = 0.35f * wgt;
            p[0] = static_cast<std::uint8_t>(std::clamp(
                static_cast<float>(lb) * (1.f - mix) + static_cast<float>(b[0]) * mix + 0.5f,
                0.f, 255.f));
            p[1] = static_cast<std::uint8_t>(std::clamp(
                static_cast<float>(lg) * (1.f - mix) + static_cast<float>(b[1]) * mix + 0.5f,
                0.f, 255.f));
            p[2] = static_cast<std::uint8_t>(std::clamp(
                static_cast<float>(lr) * (1.f - mix) + static_cast<float>(b[2]) * mix + 0.5f,
                0.f, 255.f));
        }
    }
}

void apply_eye_sharpen_bgr(VideoFrame& frame, const LandmarkSet& set,
                           const BeautyConfig& cfg) {
    const float strength = std::clamp(cfg.eye_sharpen_blend, 0.f, 1.f);
    if (!cfg.eye_sharpen || strength <= 0.f || set.empty() || frame.data.empty())
        return;
    if (frame.width <= 0 || frame.height <= 0) return;

    const int w = frame.width;
    const int h = frame.height;
    const int stride = frame.stride > 0 ? frame.stride : w * 3;

    std::vector<std::vector<IVec2>> eyes;
    for (std::uint32_t fi = 0; fi < set.face_count; ++fi) {
        const Landmark* face = face_begin(set, fi);
        if (!face) continue;
        std::vector<IVec2> left;
        std::vector<IVec2> right;
        if (lip_arc_px(face, set.landmarks_per_face, w, h, kLeftEye, left))
            eyes.push_back(std::move(left));
        if (lip_arc_px(face, set.landmarks_per_face, w, h, kRightEye, right))
            eyes.push_back(std::move(right));
    }
    const IRect roi = bounds_of_polys(eyes, 6, w, h);
    if (roi.empty()) return;

    std::vector<std::uint8_t> mask(static_cast<std::size_t>(roi.w) * roi.h, 0);
    for (const auto& eye : eyes)
        fill_roi(mask.data(), roi, eye, 255);
    feather_gaussian_u8(mask.data(), roi.w, roi.h, 5);
    if (!mask_any(mask.data(), roi.w * roi.h)) return;

    std::vector<std::uint8_t> blur;
    copy_bgr_roi(frame, roi, blur);
    box_blur_bgr(blur.data(), roi.w, roi.h, 1);

    for (int y = 0; y < roi.h; ++y) {
        std::uint8_t* drow =
            frame.data.data() + static_cast<std::size_t>(roi.y + y) * stride +
            static_cast<std::size_t>(roi.x) * 3u;
        const std::uint8_t* brow =
            blur.data() + static_cast<std::size_t>(y) * roi.w * 3;
        const std::uint8_t* mrow = mask.data() + static_cast<std::size_t>(y) * roi.w;
        for (int x = 0; x < roi.w; ++x) {
            const float wgt = (static_cast<float>(mrow[x]) / 255.f) * strength;
            if (wgt <= 1e-4f) continue;
            std::uint8_t* p = drow + x * 3;
            const std::uint8_t* b = brow + x * 3;
            for (int c = 0; c < 3; ++c) {
                const float sharp = static_cast<float>(p[c]) +
                                    1.15f * (static_cast<float>(p[c]) - static_cast<float>(b[c]));
                const float o = static_cast<float>(p[c]) * (1.f - wgt) + sharp * wgt;
                p[c] = static_cast<std::uint8_t>(std::clamp(o + 0.5f, 0.f, 255.f));
            }
        }
    }
}

} // namespace pm::video
