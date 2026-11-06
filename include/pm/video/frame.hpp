#pragma once

#include <cstdint>
#include <limits>
#include <vector>

namespace pm::video {

enum class PixelFormat {
    Unknown,
    RGB24,   // R8 G8 B8, top-down, tightly packed rows
    BGR24,   // B8 G8 R8, top-down, tightly packed rows
    NV12,
    MJPEG,   // compressed JPEG bytes
};

// Owning CPU frame. `data` is the only storage. Callers may read it on the
// capture thread only for the duration of the callback, then copy or move it.
// stride <= 0 on a packed RGB/BGR frame means width * 3. A negative stride is
// not a valid frame. There is no GPU memory in this struct.
struct VideoFrame {
    int         width        = 0;
    int         height       = 0;
    int         stride       = 0;    // bytes per row (>= width * bytes_per_pixel)
    PixelFormat format       = PixelFormat::Unknown;
    int64_t     timestamp_ms = 0;
    std::vector<std::uint8_t> data;
};

// True when a BGR24 or RGB24 frame's last pixel lies inside `data`.
inline bool rgb_frame_fits(const VideoFrame& frame) noexcept
{
    if (frame.format != PixelFormat::BGR24 && frame.format != PixelFormat::RGB24)
        return false;
    if (frame.width <= 0 || frame.height <= 0 || frame.stride < 0) return false;
    if (frame.width > (std::numeric_limits<int>::max() / 3)) return false;
    const int row = frame.width * 3;
    const int stride = frame.stride > 0 ? frame.stride : row;
    if (stride < row) return false;
    const auto pitch = static_cast<std::size_t>(stride);
    const auto rows = static_cast<std::size_t>(frame.height);
    if (rows > 1 && pitch > (std::numeric_limits<std::size_t>::max() / (rows - 1)))
        return false;
    const std::size_t lead = (rows - 1) * pitch;
    const auto tail = static_cast<std::size_t>(row);
    if (tail > std::numeric_limits<std::size_t>::max() - lead) return false;
    return lead + tail <= frame.data.size();
}

} // namespace pm::video
