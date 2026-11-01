#include "pm/video/detect/pose.hpp"
#include "pm/video/detect/commons.hpp"

#include "lib/onnx/OnnxSession.hpp"
#include "lib/onnx/OnnxImagePrep.hpp"

namespace media::vdetect {

DecodeResult decode_pose(const media::onnx::RunResult&     run,
                         const media::onnx::LetterboxInfo& lb,
                         int                               orig_w,
                         int                               orig_h,
                         int                               input_size,
                         float                             conf_thresh,
                         float                             nms_thresh,
                         const std::vector<std::string>&   class_names)
{
    DecodeResult result;
    result.task = Task::Pose;

    const DetectHead head = parse_head(run);
    if (!head.ok || head.attrs < 6) {
        result.error = "pose: unexpected output shape";
        return result;
    }
    const auto& data = run.outputs_f32[0];
    result.num_classes = 1;
    if (input_size <= 0) input_size = 640;

    const std::string person_label =
        class_names.empty() ? std::string("person") : class_names[0];

    // Classic: [cx,cy,w,h, obj, K*(x,y,score)]   → conf@4, kpts@5, n=(attrs-5)/3
    // EndToEnd: [x1,y1,x2,y2, score, cls, K*(x,y,score)] → conf@4, kpts@6, n=(attrs-6)/3
    const bool e2e   = (head.layout == HeadLayout::EndToEnd);
    const int  kbase = e2e ? 6 : 5;
    const int  n_kpt = (head.attrs - kbase) / 3;
    if (n_kpt <= 0) {
        result.error = "pose: no keypoint channels";
        return result;
    }

    const auto read_kpts = [&](Detection& d, int i) {
        d.keypoints.reserve(n_kpt);
        for (int k = 0; k < n_kpt; ++k) {
            const int a = kbase + k * 3;
            Keypoint kp;
            unmap_point(lb, orig_w, orig_h, input_size,
                        head.at(data, a, i), head.at(data, a + 1, i), kp.x, kp.y);
            kp.score = maybe_sigmoid(head.at(data, a + 2, i));
            d.keypoints.push_back(kp);
        }
    };

    if (e2e) {
        result.detections.reserve(head.count);
        for (int i = 0; i < head.count; ++i) {
            const float conf = head.at(data, 4, i);
            if (conf < conf_thresh) continue;
            float x1, y1, x2, y2;
            unmap_point(lb, orig_w, orig_h, input_size, head.at(data, 0, i), head.at(data, 1, i), x1, y1);
            unmap_point(lb, orig_w, orig_h, input_size, head.at(data, 2, i), head.at(data, 3, i), x2, y2);
            Detection d;
            d.x = x1; d.y = y1; d.w = x2 - x1; d.h = y2 - y1;
            d.conf = conf; d.cls = 0; d.label = person_label;
            read_kpts(d, i);
            result.detections.push_back(std::move(d));
        }
        result.ok = true;
        return result;
    }

    std::vector<Detection> candidates;
    candidates.reserve(128);

    for (int i = 0; i < head.count; ++i) {
        const float conf = maybe_sigmoid(head.at(data, 4, i));
        if (conf < conf_thresh) continue;

        const float cx = head.at(data, 0, i);
        const float cy = head.at(data, 1, i);
        const float bw = head.at(data, 2, i);
        const float bh = head.at(data, 3, i);

        float x1, y1, x2, y2;
        unmap_point(lb, orig_w, orig_h, input_size, cx - bw * 0.5f, cy - bh * 0.5f, x1, y1);
        unmap_point(lb, orig_w, orig_h, input_size, cx + bw * 0.5f, cy + bh * 0.5f, x2, y2);

        Detection d;
        d.x = x1; d.y = y1; d.w = x2 - x1; d.h = y2 - y1;
        d.conf = conf;
        d.cls  = 0;
        d.label = person_label;
        read_kpts(d, i);
        candidates.push_back(std::move(d));
    }

    result.detections = nms(std::move(candidates), nms_thresh, /*rotated=*/false);
    result.ok = true;
    return result;
}

} // namespace media::vdetect
