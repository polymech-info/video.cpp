#pragma once

#include "pm/video/frame.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pm::vcam {

// Nearest-neighbor scale into tightly packed BGR24. The destination stride is
// dst_w * 3; source row padding is not preserved.
//
// Source must be BGR24. stride <= 0 means tightly packed (width * 3). A negative
// stride is rejected. Returns false when the format, size, stride, or buffer
// capacity is unusable. dst_bgr is cleared on failure. Allocation failure
// returns false and does not throw.
bool scale_bgr24(const pm::video::VideoFrame& src, int dst_w, int dst_h,
                 std::vector<std::uint8_t>& dst_bgr);

// BT.601 limited-range (studio swing) BGR24 to NV12. Coefficients are the
// integer form already used by the virtual camera:
//   Y = ((66 R + 129 G + 25 B + 128) >> 8) + 16
//   U = ((-38 R - 74 G + 112 B + 128) >> 8) + 128
//   V = ((112 R - 94 G - 18 B + 128) >> 8) + 128
// Chroma is that matrix applied to the average of each 2x2 block. Width and
// height must be positive and even. stride is the source row pitch and must be
// >= width * 3. bgr_bytes and nv12_bytes are the accessible capacities.
// Returns false without writing nv12 when the arguments are unusable.
bool bgr24_to_nv12(const std::uint8_t* bgr, std::size_t bgr_bytes,
                   int width, int height, int stride,
                   std::uint8_t* nv12, std::size_t nv12_bytes);

} // namespace pm::vcam
