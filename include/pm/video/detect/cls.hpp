#pragma once
//
// video/detect/cls — classification head decoder (YOLOv8/v11 -cls).
//
// Output layout: float32[1, nc] (or [1, nc, 1, 1]). No boxes. Whole-image class
// scores → top-k. Models train on 224×224, center-crop, /255 (no mean/std):
// the caller must preprocess accordingly (force_size, normalize=false).
//
#include "pm/video/detect/types.hpp"

#include <string>
#include <vector>

namespace media::onnx { struct RunResult; }

namespace media::vdetect {

/// Decode a classification head into the top-@p top_k predictions (desc by score).
/// Applies softmax when the raw scores do not already look like a distribution.
DecodeResult decode_classify(const media::onnx::RunResult&   run,
                             int                             top_k,
                             const std::vector<std::string>& class_names);

} // namespace media::vdetect
