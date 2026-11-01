#include "pm/video/vcam/convert.hpp"

#include <algorithm>

namespace pm::vcam {

void scale_bgr24(const pm::video::VideoFrame& src, int dst_w, int dst_h,
                 std::vector<std::uint8_t>& dst_bgr) {
    dst_bgr.assign(static_cast<std::size_t>(dst_w) * dst_h * 3u, 0);
    if (src.width <= 0 || src.height <= 0 || src.data.empty() || dst_w <= 0 || dst_h <= 0)
        return;
    const int src_stride = src.stride > 0 ? src.stride : src.width * 3;
    if (src.width == dst_w && src.height == dst_h && src_stride == dst_w * 3 &&
        src.format == pm::video::PixelFormat::BGR24) {
        dst_bgr.assign(src.data.begin(), src.data.begin() + static_cast<std::ptrdiff_t>(dst_w) * dst_h * 3);
        return;
    }
    for (int y = 0; y < dst_h; ++y) {
        const int sy = std::min(src.height - 1, y * src.height / dst_h);
        const std::uint8_t* srow =
            src.data.data() + static_cast<std::size_t>(sy) * src_stride;
        std::uint8_t* drow = dst_bgr.data() + static_cast<std::size_t>(y) * dst_w * 3;
        for (int x = 0; x < dst_w; ++x) {
            const int sx = std::min(src.width - 1, x * src.width / dst_w);
            const std::uint8_t* p = srow + sx * 3;
            drow[x * 3 + 0] = p[0];
            drow[x * 3 + 1] = p[1];
            drow[x * 3 + 2] = p[2];
        }
    }
}

void bgr24_to_nv12(const std::uint8_t* bgr, int width, int height, int stride,
                   std::uint8_t* nv12) {
    if (!bgr || !nv12 || width <= 0 || height <= 0) return;
    std::uint8_t* y_plane = nv12;
    std::uint8_t* uv_plane = nv12 + static_cast<std::size_t>(width) * height;
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = bgr + static_cast<std::size_t>(y) * stride;
        std::uint8_t* yrow = y_plane + static_cast<std::size_t>(y) * width;
        for (int x = 0; x < width; ++x) {
            const int b = row[x * 3 + 0];
            const int g = row[x * 3 + 1];
            const int r = row[x * 3 + 2];
            const int Y = (66 * r + 129 * g + 25 * b + 128) >> 8;
            yrow[x] = static_cast<std::uint8_t>(std::clamp(Y + 16, 0, 255));
        }
    }
    for (int y = 0; y < height; y += 2) {
        const std::uint8_t* row0 = bgr + static_cast<std::size_t>(y) * stride;
        const std::uint8_t* row1 = bgr + static_cast<std::size_t>(std::min(y + 1, height - 1)) * stride;
        std::uint8_t* uv = uv_plane + static_cast<std::size_t>(y / 2) * width;
        for (int x = 0; x < width; x += 2) {
            int b = 0, g = 0, r = 0;
            const int x1 = std::min(x + 1, width - 1);
            const std::uint8_t* p00 = row0 + x * 3;
            const std::uint8_t* p10 = row0 + x1 * 3;
            const std::uint8_t* p01 = row1 + x * 3;
            const std::uint8_t* p11 = row1 + x1 * 3;
            b = p00[0] + p10[0] + p01[0] + p11[0];
            g = p00[1] + p10[1] + p01[1] + p11[1];
            r = p00[2] + p10[2] + p01[2] + p11[2];
            b /= 4;
            g /= 4;
            r /= 4;
            const int U = (-38 * r - 74 * g + 112 * b + 128) >> 8;
            const int V = (112 * r - 94 * g - 18 * b + 128) >> 8;
            uv[x] = static_cast<std::uint8_t>(std::clamp(U + 128, 0, 255));
            uv[x + 1] = static_cast<std::uint8_t>(std::clamp(V + 128, 0, 255));
        }
    }
}

} // namespace pm::vcam
