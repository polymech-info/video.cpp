#pragma once
//
// video/detect/seg — instance segmentation head decoder (YOLOv8/v11/yolo26 -seg).
//
// Two outputs:
//   output0 — detection head + 32 mask coefficients per box.
//             classic  : float32[1, 4+nc+32, A]            (attrs-major, needs NMS)
//             yolo26   : float32[1, N, 4+1+1+32]           (end-to-end, pre-NMS'd)
//   output1 — mask prototypes: float32[1, 32, ph, pw]      (typically 160×160)
//
// Per kept box the 32 coefficients are combined with the prototype maps
// (mask = sigmoid(Σ coeff·proto)), thresholded at 0.5, cropped to the box, then
// the outer boundary is traced (Moore-neighbor) and simplified (Douglas-Peucker)
// into Detection::contour — original image pixels. No OpenCV dependency.
//
#include "pm/video/detect/types.hpp"

#include <string>
#include <vector>

namespace media::onnx {
struct RunResult;
struct LetterboxInfo;
}

namespace media::vdetect {

DecodeResult decode_segment(const media::onnx::RunResult&     run,
                            const media::onnx::LetterboxInfo& lb,
                            int                               orig_w,
                            int                               orig_h,
                            int                               input_size,
                            float                             conf_thresh,
                            float                             nms_thresh,
                            const std::vector<std::string>&   class_names);

} // namespace media::vdetect
