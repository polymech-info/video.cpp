#include "pm/video/detect/serialize.hpp"

namespace media::vdetect {

nlohmann::json detection_to_json(const Detection& d)
{
    nlohmann::json j;
    j["x"]    = round_dp(d.x, 1);
    j["y"]    = round_dp(d.y, 1);
    j["w"]    = round_dp(d.w, 1);
    j["h"]    = round_dp(d.h, 1);
    j["conf"] = round_dp(d.conf, 3);
    j["cls"]  = d.cls;
    if (!d.label.empty()) j["label"] = d.label;
    if (d.oriented)       j["angle"] = round_dp(d.angle, 4);
    if (!d.keypoints.empty()) {
        auto kp = nlohmann::json::array();
        for (const auto& k : d.keypoints)
            kp.push_back({ {"x", round_dp(k.x, 1)},
                           {"y", round_dp(k.y, 1)},
                           {"score", round_dp(k.score, 3)} });
        j["keypoints"] = std::move(kp);
    }
    if (!d.contour.empty()) {
        auto poly = nlohmann::json::array();
        for (const auto& p : d.contour)
            poly.push_back({ round_dp(p.first, 1), round_dp(p.second, 1) });
        j["contour"] = std::move(poly);
    }
    return j;
}

nlohmann::json detections_to_json(const std::vector<Detection>& dets)
{
    auto arr = nlohmann::json::array();
    for (const auto& d : dets) arr.push_back(detection_to_json(d));
    return arr;
}

nlohmann::json classes_to_json(const std::vector<ClassScore>& classes)
{
    auto arr = nlohmann::json::array();
    for (const auto& c : classes)
        arr.push_back({ {"cls", c.cls},
                        {"score", round_dp(c.score, 4)},
                        {"label", c.label} });
    return arr;
}

nlohmann::json result_to_json(const DecodeResult& r)
{
    nlohmann::json j;
    j["task"] = task_to_string(r.task);
    if (!r.ok) j["error"] = r.error;
    if (r.task == Task::Classify)
        j["classes"] = classes_to_json(r.classes);
    else
        j["boxes"] = detections_to_json(r.detections);
    return j;
}

} // namespace media::vdetect
