#pragma once
//
// video/detect/serialize — JSON boundary for decode results.
//
// Keeps nlohmann::json out of the decoders (types/commons/boxes/cls/obb/pose);
// the CLI and the xblox block both serialize through here so the wire schema
// stays in one place.
//
//   detect / obb / pose → { "task", "boxes": [ {x,y,w,h,conf,cls,label,
//                            angle?, keypoints?:[{x,y,score}]} ] }
//   classify            → { "task", "classes": [ {cls,score,label} ] }
//
#include "pm/video/detect/types.hpp"

#include <nlohmann/json.hpp>

namespace media::vdetect {

/// Round to @p dp decimal places (compact NDJSON output).
inline double round_dp(double v, int dp) {
    double m = 1.0; while (dp-- > 0) m *= 10.0;
    return std::round(v * m) / m;
}

nlohmann::json detection_to_json(const Detection& d);

/// Array of box objects (detect/obb/pose). Empty for classify.
nlohmann::json detections_to_json(const std::vector<Detection>& dets);

/// Array of {cls,score,label} (classify). Empty otherwise.
nlohmann::json classes_to_json(const std::vector<ClassScore>& classes);

/// Full result object with "task" + the task-appropriate payload.
nlohmann::json result_to_json(const DecodeResult& r);

} // namespace media::vdetect
