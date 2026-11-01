#include "pm/video/detect/commons.hpp"

#include "lib/onnx/OnnxSession.hpp"
#include "lib/onnx/OnnxImagePrep.hpp"

#include <algorithm>

namespace media::vdetect {

// ── Output-shape parsing ──────────────────────────────────────────────────────

bool parse_detect_head(const media::onnx::RunResult& run,
                       int& num_attrs, int& num_anchors)
{
    if (run.outputs_f32.empty() || run.outputs_f32[0].empty()) return false;
    const auto& data = run.outputs_f32[0];
    const auto& shape = run.output_info.empty() ? std::vector<int64_t>{}
                                                : run.output_info[0].shape;
    num_attrs = num_anchors = 0;
    if (shape.size() >= 3) {
        num_attrs   = static_cast<int>(shape[shape.size() - 2]);
        num_anchors = static_cast<int>(shape[shape.size() - 1]);
    }
    if (num_attrs <= 4 || num_anchors <= 0) {
        num_anchors = 8400;  // 640-input v8/v11 default
        num_attrs   = static_cast<int>(data.size()) / num_anchors;
    }
    if (num_attrs <= 4 || num_anchors <= 0 ||
        static_cast<int>(data.size()) != num_attrs * num_anchors)
        return false;
    return true;
}

DetectHead parse_head(const media::onnx::RunResult& run)
{
    DetectHead h;
    if (run.outputs_f32.empty() || run.outputs_f32[0].empty()) return h;
    const auto& data  = run.outputs_f32[0];
    const auto& shape = run.output_info.empty() ? std::vector<int64_t>{}
                                                : run.output_info[0].shape;
    if (shape.size() >= 3) {
        const int d1 = static_cast<int>(shape[shape.size() - 2]);
        const int d2 = static_cast<int>(shape[shape.size() - 1]);
        if (d1 > 0 && d2 > 0) {
            // attrs = smaller dim, count = larger dim. d1<d2 → attrs-major
            // (classic, transposed); d1>d2 → detection-major (yolo26 end-to-end).
            if (d1 <= d2) { h.layout = HeadLayout::Transposed; h.attrs = d1; h.count = d2; }
            else          { h.layout = HeadLayout::EndToEnd;   h.attrs = d2; h.count = d1; }
            if (h.attrs > 4 &&
                static_cast<size_t>(h.attrs) * h.count == data.size()) {
                h.ok = true;
                return h;
            }
        }
    }
    // Fallback: dynamic/missing shape → assume classic 8400-anchor transposed.
    h.layout = HeadLayout::Transposed;
    h.count  = 8400;
    h.attrs  = static_cast<int>(data.size()) / h.count;
    if (h.attrs > 4 && static_cast<size_t>(h.attrs) * h.count == data.size())
        h.ok = true;
    return h;
}

Task autodetect_task(const media::onnx::OnnxSession& session)
{
    const auto outs = session.output_info();
    if (outs.empty()) return Task::Detect;

    // Classify: single output, rank ≤ 2 effective dims ([1, nc] or [1, nc,1,1]).
    if (outs.size() == 1) {
        const auto& sh = outs[0].shape;
        int non_unit = 0;
        for (size_t i = 1; i < sh.size(); ++i) if (sh[i] != 1) ++non_unit;
        if (sh.size() <= 2 || non_unit <= 1) return Task::Classify;
    }
    // Two outputs (det head + proto) → segmentation.
    if (outs.size() >= 2) return Task::Segment;
    return Task::Detect;
}

// ── Coordinate unmap ──────────────────────────────────────────────────────────

float unmap_scale(const media::onnx::LetterboxInfo& lb, int orig_w, int orig_h,
                  int input_size)
{
    if (lb.scale > 0.f) return 1.f / lb.scale;
    const int dim = std::max(orig_w, orig_h);
    if (input_size > 0 && dim > 0) return static_cast<float>(dim) / input_size;
    return 1.f;
}

void unmap_point(const media::onnx::LetterboxInfo& lb, int orig_w, int orig_h,
                 int input_size, float mx, float my, float& ox, float& oy)
{
    if (lb.scale > 0.f && lb.orig_w > 0) {
        ox = lb.unmap_x(mx);
        oy = lb.unmap_y(my);
        return;
    }
    const float s = unmap_scale(lb, orig_w, orig_h, input_size);
    ox = mx * s;
    oy = my * s;
}

} // namespace media::vdetect
