#include "pm/video/filters/bg_effects.hpp"

#include "pm/video/filters/image_ops.hpp"
#include "pm/video/filters/landmarks.hpp"
#include "pm/video/filters/skin_gpu.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace pm::video {
namespace {

int frame_stride(const VideoFrame& frame) {
    return frame.stride > 0 ? frame.stride : frame.width * 3;
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

void downsample2_bgr(const std::uint8_t* src, int sw, int sh, int src_stride,
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
                const std::uint8_t* row = src + static_cast<std::size_t>(yy) * src_stride;
                for (int xx = x0; xx <= x1; ++xx) {
                    const std::uint8_t* p = row + static_cast<std::size_t>(xx) * 3u;
                    acc[0] += p[0];
                    acc[1] += p[1];
                    acc[2] += p[2];
                    ++n;
                }
            }
            std::uint8_t* d =
                dst.data() + (static_cast<std::size_t>(y) * dw + x) * 3u;
            d[0] = static_cast<std::uint8_t>(acc[0] / std::max(1, n));
            d[1] = static_cast<std::uint8_t>(acc[1] / std::max(1, n));
            d[2] = static_cast<std::uint8_t>(acc[2] / std::max(1, n));
        }
    }
}

void upsample2_bgr(const std::uint8_t* src, int sw, int sh, std::uint8_t* dst,
                   int dw, int dh, int dst_stride) {
    for (int y = 0; y < dh; ++y) {
        const int sy = std::min(y / 2, sh - 1);
        std::uint8_t* row = dst + static_cast<std::size_t>(y) * dst_stride;
        const std::uint8_t* srow = src + static_cast<std::size_t>(sy) * sw * 3;
        for (int x = 0; x < dw; ++x) {
            const int sx = std::min(x / 2, sw - 1);
            const std::uint8_t* p = srow + static_cast<std::size_t>(sx) * 3u;
            std::uint8_t* d = row + static_cast<std::size_t>(x) * 3u;
            d[0] = p[0];
            d[1] = p[1];
            d[2] = p[2];
        }
    }
}

std::uint8_t sample_person(const PersonMask& person, int x, int y, int fw, int fh) {
    if (person.empty() || fw <= 0 || fh <= 0) return 0;
    if (person.width == fw && person.height == fh)
        return person.data[static_cast<std::size_t>(y) * person.width + x];
    const int sx = std::clamp(x * person.width / fw, 0, person.width - 1);
    const int sy = std::clamp(y * person.height / fh, 0, person.height - 1);
    return person.data[static_cast<std::size_t>(sy) * person.width + sx];
}

void resize_feather_mask(const PersonMask& person, int w, int h, int feather_px,
                         std::vector<std::uint8_t>& out) {
    out.resize(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x)
            out[static_cast<std::size_t>(y) * w + x] = sample_person(person, x, y, w, h);
    }
    if (feather_px > 0)
        feather_gaussian_u8(out.data(), w, h, feather_px * 2 + 1);
}

void sample_bg_pixel(const VideoFrame& bg, int x, int y, int fw, int fh,
                     std::uint8_t& b, std::uint8_t& g, std::uint8_t& r) {
    const int sx = std::clamp(x * bg.width / std::max(1, fw), 0, bg.width - 1);
    const int sy = std::clamp(y * bg.height / std::max(1, fh), 0, bg.height - 1);
    const int stride = frame_stride(bg);
    const std::uint8_t* p =
        bg.data.data() + static_cast<std::size_t>(sy) * stride + static_cast<std::size_t>(sx) * 3u;
    b = p[0];
    g = p[1];
    r = p[2];
}

} // namespace

void apply_mirror_bgr(VideoFrame& frame) {
    if (frame.width <= 1 || frame.height <= 0 || frame.data.empty()) return;
    const int w = frame.width;
    const int stride = frame_stride(frame);
    for (int y = 0; y < frame.height; ++y) {
        std::uint8_t* row = frame.data.data() + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < w / 2; ++x) {
            std::uint8_t* a = row + static_cast<std::size_t>(x) * 3u;
            std::uint8_t* b = row + static_cast<std::size_t>(w - 1 - x) * 3u;
            std::swap(a[0], b[0]);
            std::swap(a[1], b[1]);
            std::swap(a[2], b[2]);
        }
    }
}

void apply_bg_blur_bgr(VideoFrame& frame, const PersonMask& person,
                       const BeautyConfig& cfg, SkinGpu* gpu) {
    if (frame.width <= 0 || frame.height <= 0 || frame.data.empty()) return;
    const int w = frame.width;
    const int h = frame.height;
    const int stride = frame_stride(frame);
    const float blend = std::clamp(cfg.blur_strength > 0.f ? cfg.blur_strength : 0.65f,
                                   0.f, 1.f);
    if (blend <= 1e-4f) return;

    std::vector<std::uint8_t> mask;
    resize_feather_mask(person, w, h, std::max(0, cfg.feather_px), mask);

    if (gpu && gpu->valid()) {
        std::vector<std::uint8_t> inv(mask.size());
        for (std::size_t i = 0; i < mask.size(); ++i)
            inv[i] = static_cast<std::uint8_t>(255 - mask[i]);
        IRect roi{0, 0, w, h};
        if (gpu->smooth_roi(frame, roi, inv.data(), blend))
            return;
    }

    const int scale = blend > 0.7f ? 4 : 2;
    std::vector<std::uint8_t> small;
    int sw = w;
    int sh = h;
    if (scale >= 4) {
        std::vector<std::uint8_t> mid;
        int mw = 0, mh = 0;
        downsample2_bgr(frame.data.data(), w, h, stride, mid, mw, mh);
        downsample2_bgr(mid.data(), mw, mh, mw * 3, small, sw, sh);
    } else {
        downsample2_bgr(frame.data.data(), w, h, stride, small, sw, sh);
    }
    const int radius = std::max(1, static_cast<int>(blend * 16.f + 0.5f));
    box_blur_bgr(small.data(), sw, sh, radius);

    std::vector<std::uint8_t> blurred(static_cast<std::size_t>(w) * h * 3u);
    if (scale >= 4) {
        std::vector<std::uint8_t> mid(static_cast<std::size_t>((w + 1) / 2) *
                                      ((h + 1) / 2) * 3u);
        const int mw = std::max(1, w / 2);
        const int mh = std::max(1, h / 2);
        upsample2_bgr(small.data(), sw, sh, mid.data(), mw, mh, mw * 3);
        upsample2_bgr(mid.data(), mw, mh, blurred.data(), w, h, w * 3);
    } else {
        upsample2_bgr(small.data(), sw, sh, blurred.data(), w, h, w * 3);
    }

    for (int y = 0; y < h; ++y) {
        std::uint8_t* dst = frame.data.data() + static_cast<std::size_t>(y) * stride;
        const std::uint8_t* blur = blurred.data() + static_cast<std::size_t>(y) * w * 3;
        const std::uint8_t* mrow = mask.data() + static_cast<std::size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            const int keep = mrow[x];
            const int wet = static_cast<int>((255 - keep) * blend + 0.5f);
            if (wet <= 0) {
                dst += 3;
                blur += 3;
                continue;
            }
            dst[0] = static_cast<std::uint8_t>(
                (dst[0] * (255 - wet) + blur[0] * wet) / 255);
            dst[1] = static_cast<std::uint8_t>(
                (dst[1] * (255 - wet) + blur[1] * wet) / 255);
            dst[2] = static_cast<std::uint8_t>(
                (dst[2] * (255 - wet) + blur[2] * wet) / 255);
            dst += 3;
            blur += 3;
        }
    }
}

void apply_bg_replace_bgr(VideoFrame& frame, const PersonMask& person,
                          const BeautyConfig& cfg, const VideoFrame* bg_image) {
    if (frame.width <= 0 || frame.height <= 0 || frame.data.empty()) return;
    const int w = frame.width;
    const int h = frame.height;
    const int stride = frame_stride(frame);

    std::uint8_t cr = 26, cg = 26, cb = 26;
    if (!cfg.bg_color.empty())
        parse_hex_rgb(cfg.bg_color, cr, cg, cb);
    const bool use_image = bg_image && bg_image->width > 0 && bg_image->height > 0 &&
                           !bg_image->data.empty() &&
                           bg_image->format == PixelFormat::BGR24;

    std::vector<std::uint8_t> mask;
    resize_feather_mask(person, w, h, std::max(0, cfg.feather_px), mask);

    for (int y = 0; y < h; ++y) {
        std::uint8_t* dst = frame.data.data() + static_cast<std::size_t>(y) * stride;
        const std::uint8_t* mrow = mask.data() + static_cast<std::size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            const int keep = mrow[x];
            if (keep >= 255) {
                dst += 3;
                continue;
            }
            std::uint8_t bb = cb, gg = cg, rr = cr;
            if (use_image)
                sample_bg_pixel(*bg_image, x, y, w, h, bb, gg, rr);
            if (keep <= 0) {
                dst[0] = bb;
                dst[1] = gg;
                dst[2] = rr;
            } else {
                dst[0] = static_cast<std::uint8_t>((dst[0] * keep + bb * (255 - keep)) / 255);
                dst[1] = static_cast<std::uint8_t>((dst[1] * keep + gg * (255 - keep)) / 255);
                dst[2] = static_cast<std::uint8_t>((dst[2] * keep + rr * (255 - keep)) / 255);
            }
            dst += 3;
        }
    }
}

} // namespace pm::video
