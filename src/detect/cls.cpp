#include "pm/video/detect/cls.hpp"
#include "pm/video/detect/commons.hpp"

#include "lib/onnx/OnnxSession.hpp"

#include <algorithm>
#include <numeric>

namespace media::vdetect {

DecodeResult decode_classify(const media::onnx::RunResult&   run,
                             int                             top_k,
                             const std::vector<std::string>& class_names)
{
    DecodeResult result;
    result.task = Task::Classify;

    if (run.outputs_f32.empty() || run.outputs_f32[0].empty()) {
        result.error = "classify: model produced no output";
        return result;
    }
    std::vector<float> scores = run.outputs_f32[0];
    const int nc = static_cast<int>(scores.size());
    result.num_classes = nc;

    // If the vector is not already a probability distribution (sums ≈ 1, all in
    // [0,1]), treat it as logits and softmax it.
    float sum = 0.f; bool in_unit = true;
    for (float v : scores) { sum += v; if (v < 0.f || v > 1.f) in_unit = false; }
    if (!in_unit || std::abs(sum - 1.f) > 0.05f)
        softmax_inplace(scores);

    std::vector<int> idx(nc);
    std::iota(idx.begin(), idx.end(), 0);
    if (top_k < 1) top_k = 1;
    const int k = std::min(top_k, nc);
    std::partial_sort(idx.begin(), idx.begin() + k, idx.end(),
                      [&](int a, int b) { return scores[a] > scores[b]; });

    result.classes.reserve(k);
    for (int i = 0; i < k; ++i) {
        ClassScore cs;
        cs.cls   = idx[i];
        cs.score = scores[idx[i]];
        if (idx[i] >= 0 && idx[i] < static_cast<int>(class_names.size()))
            cs.label = class_names[idx[i]];
        result.classes.push_back(std::move(cs));
    }
    result.ok = true;
    return result;
}

} // namespace media::vdetect
