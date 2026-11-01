#include "pm/video/detect/boxes.hpp"
#include "pm/video/detect/commons.hpp"

#include "lib/onnx/OnnxSession.hpp"
#include "lib/onnx/OnnxImagePrep.hpp"

namespace media::vdetect {

DecodeResult decode_detect(const media::onnx::RunResult&     run,
                           const media::onnx::LetterboxInfo& lb,
                           int                               orig_w,
                           int                               orig_h,
                           int                               input_size,
                           float                             conf_thresh,
                           float                             nms_thresh,
                           const std::vector<std::string>&   class_names)
{
    DecodeResult result;
    result.task = Task::Detect;

    const DetectHead head = parse_head(run);
    if (!head.ok) {
        result.error = "detect: unexpected output shape";
        return result;
    }
    const auto& data = run.outputs_f32[0];
    if (input_size <= 0) input_size = 640;

    const auto set_label = [&](Detection& d) {
        if (d.cls >= 0 && d.cls < static_cast<int>(class_names.size()))
            d.label = class_names[d.cls];
    };

    // ── yolo26 NMS-free head: [x1,y1,x2,y2, score, cls], already filtered/sorted ─
    if (head.layout == HeadLayout::EndToEnd) {
        result.num_classes = static_cast<int>(class_names.size());
        result.detections.reserve(head.count);
        for (int i = 0; i < head.count; ++i) {
            const float score = head.at(data, 4, i);
            if (score < conf_thresh) continue;  // tail rows are zero-padded
            float x1, y1, x2, y2;
            unmap_point(lb, orig_w, orig_h, input_size, head.at(data, 0, i), head.at(data, 1, i), x1, y1);
            unmap_point(lb, orig_w, orig_h, input_size, head.at(data, 2, i), head.at(data, 3, i), x2, y2);
            Detection d;
            d.x = x1; d.y = y1; d.w = x2 - x1; d.h = y2 - y1;
            d.conf = score;
            d.cls  = static_cast<int>(head.at(data, 5, i) + 0.5f);
            set_label(d);
            result.detections.push_back(std::move(d));
        }
        result.ok = true;
        return result;
    }

    // ── classic v8/v11 head: [cx,cy,w,h, nc class scores], needs NMS ────────────
    const int nc = head.attrs - 4;
    result.num_classes = nc;

    std::vector<Detection> candidates;
    candidates.reserve(256);

    for (int i = 0; i < head.count; ++i) {
        int   best_cls  = 0;
        float best_conf = 0.f;
        for (int c = 0; c < nc; ++c) {
            const float v = maybe_sigmoid(head.at(data, 4 + c, i));
            if (v > best_conf) { best_conf = v; best_cls = c; }
        }
        if (best_conf < conf_thresh) continue;

        const float cx = head.at(data, 0, i);
        const float cy = head.at(data, 1, i);
        const float bw = head.at(data, 2, i);
        const float bh = head.at(data, 3, i);

        float x1, y1, x2, y2;
        unmap_point(lb, orig_w, orig_h, input_size, cx - bw * 0.5f, cy - bh * 0.5f, x1, y1);
        unmap_point(lb, orig_w, orig_h, input_size, cx + bw * 0.5f, cy + bh * 0.5f, x2, y2);

        Detection d;
        d.x = x1; d.y = y1; d.w = x2 - x1; d.h = y2 - y1;
        d.conf = best_conf;
        d.cls  = best_cls;
        set_label(d);
        candidates.push_back(std::move(d));
    }

    result.detections = nms(std::move(candidates), nms_thresh, /*rotated=*/false);
    result.ok = true;
    return result;
}

} // namespace media::vdetect
