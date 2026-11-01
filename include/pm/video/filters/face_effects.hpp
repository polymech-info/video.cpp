#pragma once

#include "pm/video/filters/beauty_config.hpp"
#include "pm/video/filters/landmarks.hpp"

#include "pm/video/capture.hpp"

namespace pm::video {

class SkinGpu;

void apply_lip_tint_bgr(VideoFrame& frame, const LandmarkSet& set,
                        const BeautyConfig& cfg);
void apply_teeth_whiten_bgr(VideoFrame& frame, const LandmarkSet& set,
                            const BeautyConfig& cfg);
void apply_skin_smooth_bgr(VideoFrame& frame, const LandmarkSet& set,
                           const BeautyConfig& cfg, SkinGpu* gpu = nullptr);
void apply_face_grade_bgr(VideoFrame& frame, const LandmarkSet& set,
                          const BeautyConfig& cfg);
void apply_redness_mute_bgr(VideoFrame& frame, const LandmarkSet& set,
                            const BeautyConfig& cfg);
void apply_undereye_lift_bgr(VideoFrame& frame, const LandmarkSet& set,
                             const BeautyConfig& cfg);
void apply_eye_sharpen_bgr(VideoFrame& frame, const LandmarkSet& set,
                           const BeautyConfig& cfg);

} // namespace pm::video
