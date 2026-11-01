#pragma once

#include "pm/video/frame.hpp"

#include <cstdint>
#include <vector>

namespace pm::vcam {

void scale_bgr24(const pm::video::VideoFrame& src, int dst_w, int dst_h,
                 std::vector<std::uint8_t>& dst_bgr);

void bgr24_to_nv12(const std::uint8_t* bgr, int width, int height, int stride,
                   std::uint8_t* nv12);

} // namespace pm::vcam
