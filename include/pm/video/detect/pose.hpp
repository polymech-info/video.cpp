#pragma once
//
// video/detect/pose — keypoint head decoder (YOLOv8/v11 -pose).
//
// Output layout: float32[1, 4+1+K*3, A]  (attrs-major)
//   ch 0-3              : cx, cy, w, h  (model-input pixels)
//   ch 4                : object/person score
//   ch 5..5+K*3-1       : K keypoints, each (x, y, score)
//
// nc is implicitly 1 (person). Detections carry the person box plus `keypoints`
// in original pixel space. K is derived from the channel count (COCO → 17).
//
#include "pm/video/detect/types.hpp"

#include <string>
#include <vector>

namespace media::onnx {
struct RunResult;
struct LetterboxInfo;
}

namespace media::vdetect {

DecodeResult decode_pose(const media::onnx::RunResult&     run,
                         const media::onnx::LetterboxInfo& lb,
                         int                               orig_w,
                         int                               orig_h,
                         int                               input_size,
                         float                             conf_thresh,
                         float                             nms_thresh,
                         const std::vector<std::string>&   class_names);

} // namespace media::vdetect
