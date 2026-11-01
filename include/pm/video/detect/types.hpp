#pragma once
//
// video/detect — shared result types for YOLO-family ONNX decoding.
//
// Task-agnostic, struct-based (no nlohmann::json in the hot path — JSON is only
// produced at the CLI / block boundary). One DecodeResult carries whichever
// fields the task populates: detect/obb/pose fill `detections`, classify fills
// `classes`.
//
// Coordinate convention: all box coordinates are in ORIGINAL image pixel space
// (decoders already unmap through the letterbox). Angles are radians.
//
#include <string>
#include <utility>
#include <vector>

namespace media::vdetect {

/// Which YOLO head a model exposes. `Auto` asks the decoder/dispatcher to infer
/// it from the output tensor shapes (only reliable for detect vs classify — obb
/// and pose share the detect layout and must be requested explicitly).
enum class Task {
    Auto = 0,
    Detect,      ///< axis-aligned boxes — [1, 4+nc, A]
    Classify,    ///< whole-image class vector — [1, nc]
    Segment,     ///< instance masks — detect head + proto (not yet implemented)
    OBB,         ///< oriented boxes — [1, 4+nc+1, A] (+1 = angle)
    Pose,        ///< person + keypoints — [1, 4+1+K*3, A]
    Semantic,    ///< per-pixel class map (yolo26 -sem, not yet implemented)
};

/// Parse a task name (cli / block param). Unknown → Auto.
inline Task task_from_string(const std::string& s) noexcept {
    if (s == "detect")   return Task::Detect;
    if (s == "classify" || s == "cls") return Task::Classify;
    if (s == "segment"  || s == "seg") return Task::Segment;
    if (s == "obb")      return Task::OBB;
    if (s == "pose")     return Task::Pose;
    if (s == "semantic" || s == "sem") return Task::Semantic;
    return Task::Auto;
}

inline const char* task_to_string(Task t) noexcept {
    switch (t) {
        case Task::Detect:   return "detect";
        case Task::Classify: return "classify";
        case Task::Segment:  return "segment";
        case Task::OBB:      return "obb";
        case Task::Pose:     return "pose";
        case Task::Semantic: return "semantic";
        default:             return "auto";
    }
}

/// A single pose keypoint in original image pixel space.
struct Keypoint {
    float x = 0.f;
    float y = 0.f;
    float score = 0.f;  ///< Visibility / confidence in [0,1].
};

/// One detection. Axis-aligned by default; `oriented` flips on the angle for OBB.
/// `keypoints` is populated for pose. Top-left corner + size in original pixels.
struct Detection {
    float x = 0.f, y = 0.f;   ///< Top-left corner (original image pixels).
    float w = 0.f, h = 0.f;   ///< Width / height (original image pixels).
    float conf = 0.f;
    int   cls = 0;
    std::string label;        ///< Empty unless class_names supplied.

    bool  oriented = false;   ///< true → `angle` is meaningful (OBB).
    float angle = 0.f;        ///< Rotation in radians (OBB only).

    std::vector<Keypoint> keypoints;  ///< Pose only.

    /// Segmentation only: outer mask boundary as a simplified polygon in original
    /// image pixels (x,y). Empty for other tasks. Traced from the prototype mask
    /// (no OpenCV — see decode_segment).
    std::vector<std::pair<float, float>> contour;

    /// Box center — convenience for OBB corner reconstruction / smoothing.
    float cx() const { return x + w * 0.5f; }
    float cy() const { return y + h * 0.5f; }
};

/// One classification prediction (classify task).
struct ClassScore {
    int   cls = 0;
    float score = 0.f;
    std::string label;
};

/// Unified decode output. `task` records what was actually decoded.
struct DecodeResult {
    bool        ok = false;
    std::string error;
    Task        task = Task::Detect;
    int         num_classes = 0;

    std::vector<Detection>  detections;  ///< detect / obb / pose
    std::vector<ClassScore> classes;     ///< classify (sorted desc by score)
};

} // namespace media::vdetect
