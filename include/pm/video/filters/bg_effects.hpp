#pragma once

#include "pm/video/filters/beauty_config.hpp"
#include "pm/video/filters/image_segmenter.hpp"

#include "pm/video/capture.hpp"

namespace pm::video {

class SkinGpu;

void apply_mirror_bgr(VideoFrame& frame);

void apply_bg_blur_bgr(VideoFrame& frame, const PersonMask& person,
                       const BeautyConfig& cfg, SkinGpu* gpu = nullptr);

void apply_bg_replace_bgr(VideoFrame& frame, const PersonMask& person,
                          const BeautyConfig& cfg, const VideoFrame* bg_image);

} // namespace pm::video
