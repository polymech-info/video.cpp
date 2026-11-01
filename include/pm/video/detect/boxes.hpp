#pragma once
//
// video/detect/boxes — axis-aligned detection head decoder (YOLOv8/v11 detect).
//
// Output layout: float32[1, 4+nc, A]  (attrs-major)
//   ch 0-3   : cx, cy, w, h  (model-input pixels)
//   ch 4..   : per-class score (raw logit or post-sigmoid)
//
#include "pm/video/detect/types.hpp"

#include <string>
#include <vector>

namespace media::onnx {
struct RunResult;
struct LetterboxInfo;
}

namespace media::vdetect {

/// Decode a detect head into NMS'd, original-pixel-space detections.
///
/// @param run         Inference result (first output = detect head).
/// @param lb          Letterbox info from preprocessing (for coordinate unmap).
/// @param orig_w/h    Original frame dimensions (fallback unmap when lb absent).
/// @param input_size  Model input size in px (fallback unmap; 0 → 640).
/// @param conf_thresh Min class confidence.
/// @param nms_thresh  IoU threshold for NMS.
/// @param class_names index → label (empty → no labels).
DecodeResult decode_detect(const media::onnx::RunResult&     run,
                           const media::onnx::LetterboxInfo& lb,
                           int                               orig_w,
                           int                               orig_h,
                           int                               input_size,
                           float                             conf_thresh,
                           float                             nms_thresh,
                           const std::vector<std::string>&   class_names);

} // namespace media::vdetect
