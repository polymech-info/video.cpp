#pragma once

#include "pm/video/detect/types.hpp"

#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace media::onnx {
struct RunResult;
struct LetterboxInfo;
class OnnxSession;
}

namespace media::vdetect {

inline float sigmoid(float v) noexcept { return 1.f / (1.f + std::exp(-v)); }

/// Apply sigmoid only if the value is outside [0,1] (models that already
/// bake the activation into the export).
inline float maybe_sigmoid(float v) noexcept {
    return (v < 0.f || v > 1.f) ? sigmoid(v) : v;
}

void softmax_inplace(std::vector<float>& v);

float iou_aabb(float ax1, float ay1, float ax2, float ay2,
               float bx1, float by1, float bx2, float by2) noexcept;

float probiou(float cx1, float cy1, float w1, float h1, float a1,
              float cx2, float cy2, float w2, float h2, float a2) noexcept;

std::vector<Detection> nms(std::vector<Detection> dets, float iou_thresh, bool rotated);

std::vector<std::string> load_label_lines(const std::string& path);

const std::vector<std::string>& dota15_labels() noexcept;

const std::vector<std::pair<int, int>>& coco_pose_skeleton() noexcept;

bool parse_detect_head(const media::onnx::RunResult& run,
                       int& num_attrs, int& num_anchors);

enum class HeadLayout { Transposed, EndToEnd };

struct DetectHead {
    bool       ok     = false;
    HeadLayout layout = HeadLayout::Transposed;
    int        attrs  = 0;
    int        count  = 0;

    float at(const std::vector<float>& data, int a, int i) const {
        return layout == HeadLayout::Transposed
                   ? data[static_cast<std::size_t>(a) * static_cast<std::size_t>(count) + static_cast<std::size_t>(i)]
                   : data[static_cast<std::size_t>(i) * static_cast<std::size_t>(attrs) + static_cast<std::size_t>(a)];
    }
};

DetectHead parse_head(const media::onnx::RunResult& run);
Task autodetect_task(const media::onnx::OnnxSession& session);
void unmap_point(const media::onnx::LetterboxInfo& lb, int orig_w, int orig_h,
                 int input_size, float mx, float my, float& ox, float& oy);
float unmap_scale(const media::onnx::LetterboxInfo& lb, int orig_w, int orig_h,
                  int input_size);

} // namespace media::vdetect
