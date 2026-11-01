#pragma once
//
// video/detect/obb — oriented bounding box head decoder (YOLOv8/v11 -obb).
//
// Output layout: float32[1, 4+nc+1, A]  (attrs-major)
//   ch 0-3        : cx, cy, w, h  (model-input pixels)
//   ch 4..4+nc-1  : per-class score
//   ch 4+nc       : angle θ (radians, ~[-π/4, 3π/4])
//
// Decoded detections carry oriented=true + `angle`; (x,y,w,h) are the
// axis-aligned center/size mapped to original pixels (corners are derived from
// center+size+angle by the renderer). NMS uses rotated probIoU.
//
#include "pm/video/detect/types.hpp"

#include <string>
#include <vector>

namespace media::onnx {
struct RunResult;
struct LetterboxInfo;
}

namespace media::vdetect {

DecodeResult decode_obb(const media::onnx::RunResult&     run,
                        const media::onnx::LetterboxInfo& lb,
                        int                               orig_w,
                        int                               orig_h,
                        int                               input_size,
                        float                             conf_thresh,
                        float                             nms_thresh,
                        const std::vector<std::string>&   class_names);

} // namespace media::vdetect
