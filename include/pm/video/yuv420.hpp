#pragma once

// Android Camera2 YUV_420_888 is three planes. Row stride and pixel stride are
// independent, and the chroma planes are not a packed NV12 buffer.
// Width and height must be positive and even.

#include <cstddef>
#include <cstdint>

namespace pm::video {

struct YuvPlane {
    const std::uint8_t* data = nullptr;
    int row_stride = 0;
    int pixel_stride = 0;
    std::size_t capacity = 0;
};

inline bool yuv420888_to_nv12(const YuvPlane& y, const YuvPlane& u, const YuvPlane& v,
                              int width, int height,
                              std::uint8_t* nv12, std::size_t nv12_bytes)
{
    if (!y.data || !u.data || !v.data || !nv12) return false;
    if (width <= 0 || height <= 0 || (width & 1) || (height & 1)) return false;
    if (y.row_stride < width || y.pixel_stride <= 0) return false;
    if (u.row_stride <= 0 || v.row_stride <= 0 || u.pixel_stride <= 0 || v.pixel_stride <= 0)
        return false;

    const std::size_t y_bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (y_bytes > (static_cast<std::size_t>(-1) / 3)) return false;
    const std::size_t need = y_bytes + y_bytes / 2;
    if (nv12_bytes < need) return false;

    auto fits = [](const YuvPlane& plane, int rows, int cols) {
        if (rows <= 0 || cols <= 0) return false;
        const std::size_t last_row = static_cast<std::size_t>(rows - 1) * static_cast<std::size_t>(plane.row_stride);
        const std::size_t last_col = static_cast<std::size_t>(cols - 1) * static_cast<std::size_t>(plane.pixel_stride);
        if (last_row > static_cast<std::size_t>(-1) - last_col - 1) return false;
        return last_row + last_col + 1 <= plane.capacity;
    };
    if (!fits(y, height, width)) return false;
    if (!fits(u, height / 2, width / 2)) return false;
    if (!fits(v, height / 2, width / 2)) return false;

    for (int row = 0; row < height; ++row) {
        const std::uint8_t* src = y.data + static_cast<std::size_t>(row) * static_cast<std::size_t>(y.row_stride);
        std::uint8_t* dst = nv12 + static_cast<std::size_t>(row) * static_cast<std::size_t>(width);
        for (int col = 0; col < width; ++col)
            dst[col] = src[static_cast<std::size_t>(col) * static_cast<std::size_t>(y.pixel_stride)];
    }
    std::uint8_t* uv = nv12 + y_bytes;
    for (int row = 0; row < height / 2; ++row) {
        const std::uint8_t* su = u.data + static_cast<std::size_t>(row) * static_cast<std::size_t>(u.row_stride);
        const std::uint8_t* sv = v.data + static_cast<std::size_t>(row) * static_cast<std::size_t>(v.row_stride);
        std::uint8_t* dst = uv + static_cast<std::size_t>(row) * static_cast<std::size_t>(width);
        for (int col = 0; col < width / 2; ++col) {
            dst[col * 2] = su[static_cast<std::size_t>(col) * static_cast<std::size_t>(u.pixel_stride)];
            dst[col * 2 + 1] = sv[static_cast<std::size_t>(col) * static_cast<std::size_t>(v.pixel_stride)];
        }
    }
    return true;
}

} // namespace pm::video
