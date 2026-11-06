#include "pm/video/vcam/convert.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>

namespace pm::vcam {
namespace {

bool row_bytes_for(int width, int channels, int& row_bytes)
{
    if (width <= 0 || channels <= 0) return false;
    if (width > std::numeric_limits<int>::max() / channels) return false;
    row_bytes = width * channels;
    return true;
}

bool packed_size(int width, int height, int channels, std::size_t& bytes)
{
    int row = 0;
    if (!row_bytes_for(width, height > 0 ? channels : 0, row)) return false;
    if (static_cast<std::size_t>(height) >
        std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(row))
        return false;
    bytes = static_cast<std::size_t>(row) * static_cast<std::size_t>(height);
    return true;
}

// Bytes from the start of the buffer through the last used pixel of the last row.
bool span_fits(int stride, int height, int used_bytes, std::size_t capacity)
{
    if (stride <= 0 || height <= 0 || used_bytes <= 0 || used_bytes > stride) return false;
    const auto pitch = static_cast<std::size_t>(stride);
    const auto rows = static_cast<std::size_t>(height);
    if (rows > 1 && pitch > std::numeric_limits<std::size_t>::max() / (rows - 1)) return false;
    const std::size_t tail = static_cast<std::size_t>(used_bytes);
    const std::size_t lead = (rows - 1) * pitch;
    if (tail > std::numeric_limits<std::size_t>::max() - lead) return false;
    return lead + tail <= capacity;
}

int limited_bt601(int value)
{
    return std::clamp(value, 0, 255);
}

} // namespace

bool scale_bgr24(const pm::video::VideoFrame& src, int dst_w, int dst_h,
                 std::vector<std::uint8_t>& dst_bgr)
{
    auto reject = [&] {
        dst_bgr.clear();
        return false;
    };
    if (src.format != pm::video::PixelFormat::BGR24) return reject();
    if (src.width <= 0 || src.height <= 0 || dst_w <= 0 || dst_h <= 0) return reject();
    if (src.stride < 0) return reject();

    int src_row = 0;
    int dst_row = 0;
    if (!row_bytes_for(src.width, 3, src_row)) return reject();
    if (!row_bytes_for(dst_w, 3, dst_row)) return reject();
    const int src_stride = src.stride > 0 ? src.stride : src_row;
    if (src_stride < src_row) return reject();
    if (!span_fits(src_stride, src.height, src_row, src.data.size())) return reject();

    std::size_t dst_bytes = 0;
    if (!packed_size(dst_w, dst_h, 3, dst_bytes)) return reject();
    if (dst_bgr.size() != dst_bytes) {
        try {
            dst_bgr.assign(dst_bytes, 0);
        } catch (const std::bad_alloc&) {
            dst_bgr.clear();
            return false;
        }
    }

    if (src.width == dst_w && src.height == dst_h && src_stride == dst_row) {
        std::memcpy(dst_bgr.data(), src.data.data(), dst_bytes);
        return true;
    }

    for (int y = 0; y < dst_h; ++y) {
        const int sy = static_cast<int>(
            (static_cast<std::int64_t>(y) * src.height) / dst_h);
        const auto srow_off = static_cast<std::size_t>(sy) * static_cast<std::size_t>(src_stride);
        std::uint8_t* drow = dst_bgr.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(dst_row);
        for (int x = 0; x < dst_w; ++x) {
            const int sx = static_cast<int>(
                (static_cast<std::int64_t>(x) * src.width) / dst_w);
            const auto soff = srow_off + static_cast<std::size_t>(sx) * 3u;
            if (soff + 3 > src.data.size()) {
                dst_bgr.clear();
                return false;
            }
            const std::uint8_t* p = src.data.data() + soff;
            drow[x * 3 + 0] = p[0];
            drow[x * 3 + 1] = p[1];
            drow[x * 3 + 2] = p[2];
        }
    }
    return true;
}

bool bgr24_to_nv12(const std::uint8_t* bgr, std::size_t bgr_bytes,
                   int width, int height, int stride,
                   std::uint8_t* nv12, std::size_t nv12_bytes)
{
    if (!bgr || !nv12 || width <= 0 || height <= 0) return false;
    if ((width & 1) != 0 || (height & 1) != 0) return false;
    int row = 0;
    if (!row_bytes_for(width, 3, row)) return false;
    if (stride < row) return false;
    if (!span_fits(stride, height, row, bgr_bytes)) return false;

    std::size_t y_bytes = 0;
    if (!packed_size(width, height, 1, y_bytes)) return false;
    if (y_bytes > std::numeric_limits<std::size_t>::max() / 3) return false;
    const std::size_t nv12_need = y_bytes + y_bytes / 2;
    if (nv12_bytes < nv12_need) return false;

    std::uint8_t* y_plane = nv12;
    std::uint8_t* uv_plane = nv12 + y_bytes;
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* src = bgr + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
        std::uint8_t* yrow = y_plane + static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
        for (int x = 0; x < width; ++x) {
            const int b = src[x * 3 + 0];
            const int g = src[x * 3 + 1];
            const int r = src[x * 3 + 2];
            const int Y = (66 * r + 129 * g + 25 * b + 128) >> 8;
            yrow[x] = static_cast<std::uint8_t>(limited_bt601(Y + 16));
        }
    }
    for (int y = 0; y < height; y += 2) {
        const std::uint8_t* row0 = bgr + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
        const std::uint8_t* row1 = bgr + static_cast<std::size_t>(y + 1) * static_cast<std::size_t>(stride);
        std::uint8_t* uv = uv_plane + static_cast<std::size_t>(y / 2) * static_cast<std::size_t>(width);
        for (int x = 0; x < width; x += 2) {
            const std::uint8_t* p00 = row0 + x * 3;
            const std::uint8_t* p10 = row0 + (x + 1) * 3;
            const std::uint8_t* p01 = row1 + x * 3;
            const std::uint8_t* p11 = row1 + (x + 1) * 3;
            const int b = (p00[0] + p10[0] + p01[0] + p11[0]) / 4;
            const int g = (p00[1] + p10[1] + p01[1] + p11[1]) / 4;
            const int r = (p00[2] + p10[2] + p01[2] + p11[2]) / 4;
            const int U = (-38 * r - 74 * g + 112 * b + 128) >> 8;
            const int V = (112 * r - 94 * g - 18 * b + 128) >> 8;
            uv[x] = static_cast<std::uint8_t>(limited_bt601(U + 128));
            uv[x + 1] = static_cast<std::uint8_t>(limited_bt601(V + 128));
        }
    }
    return true;
}

} // namespace pm::vcam
