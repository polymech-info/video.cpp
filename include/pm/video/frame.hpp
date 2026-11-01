#pragma once

#include <cstdint>
#include <vector>

namespace pm::video {

enum class PixelFormat {
    Unknown,
    RGB24,   // R8 G8 B8, top-down, tightly packed rows
    BGR24,   // B8 G8 R8, top-down, tightly packed rows
    NV12,
    MJPEG,   // compressed JPEG bytes
};

struct VideoFrame {
    int         width        = 0;
    int         height       = 0;
    int         stride       = 0;    // bytes per row (>= width * bytes_per_pixel)
    PixelFormat format       = PixelFormat::Unknown;
    int64_t     timestamp_ms = 0;
    std::vector<std::uint8_t> data;
};

} // namespace pm::video
