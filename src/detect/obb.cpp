#include "pm/video/detect/obb.hpp"
#include "pm/video/detect/commons.hpp"

#include "lib/onnx/OnnxSession.hpp"
#include "lib/onnx/OnnxImagePrep.hpp"

namespace media::vdetect {

DecodeResult decode_obb(const media::onnx::RunResult&     run,
                        const media::onnx::LetterboxInfo& lb,
                        int                               orig_w,
                        int                               orig_h,
                        int                               input_size,
                        float                             conf_thresh,
                        float                             nms_thresh,
                        const std::vector<std::string>&   class_names)
{
    DecodeResult result;
    result.task = Task::OBB;

    const DetectHead head = parse_head(run);
    if (!head.ok || head.attrs < 6) {
        result.error = "obb: unexpected output shape";
        return result;
    }
    const auto& data = run.outputs_f32[0];
    if (input_size <= 0) input_size = 640;
    const float s = unmap_scale(lb, orig_w, orig_h, input_size);

    const auto set_label = [&](Detection& d) {
        if (d.cls >= 0 && d.cls < static_cast<int>(class_names.size()))
            d.label = class_names[d.cls];
    };
    const auto push_obb = [&](std::vector<Detection>& out, float cx, float cy,
                              float bw, float bh, float angle, float conf, int cls) {
        float ocx, ocy;
        unmap_point(lb, orig_w, orig_h, input_size, cx, cy, ocx, ocy);
        const float ow = bw * s, oh = bh * s;
        Detection d;
        d.x = ocx - ow * 0.5f; d.y = ocy - oh * 0.5f;
        d.w = ow; d.h = oh;
        d.conf = conf; d.cls = cls; d.oriented = true; d.angle = angle;
        set_label(d);
        out.push_back(std::move(d));
    };

    // ── yolo26 NMS-free OBB: row [cx,cy,w,h, score, cls, angle], already sorted ──
    // (confirmed empirically: attr4=score∈[0,1], attr5=class idx, attr6=angle rad).
    if (head.layout == HeadLayout::EndToEnd) {
        result.num_classes = static_cast<int>(class_names.size());
        result.detections.reserve(head.count);
        for (int i = 0; i < head.count; ++i) {
            const float score = head.at(data, 4, i);
            if (score < conf_thresh) continue;
            push_obb(result.detections,
                     head.at(data, 0, i), head.at(data, 1, i),
                     head.at(data, 2, i), head.at(data, 3, i),
                     head.at(data, 6, i), score,
                     static_cast<int>(head.at(data, 5, i) + 0.5f));
        }
        result.ok = true;
        return result;
    }

    // ── classic v8/v11 OBB: [cx,cy,w,h, nc scores, angle]; angle is LAST channel ─
    const int angle_ch = head.attrs - 1;
    const int nc       = head.attrs - 5;
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
        push_obb(candidates,
                 head.at(data, 0, i), head.at(data, 1, i),
                 head.at(data, 2, i), head.at(data, 3, i),
                 head.at(data, angle_ch, i), best_conf, best_cls);
    }

    result.detections = nms(std::move(candidates), nms_thresh, /*rotated=*/true);
    result.ok = true;
    return result;
}

} // namespace media::vdetect
