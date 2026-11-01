#include "pm/video/detect/commons.hpp"

#include <algorithm>
#include <fstream>

namespace media::vdetect {

void softmax_inplace(std::vector<float>& v)
{
    if (v.empty()) return;
    const float mx = *std::max_element(v.begin(), v.end());
    float sum = 0.f;
    for (float& x : v) { x = std::exp(x - mx); sum += x; }
    if (sum > 0.f) for (float& x : v) x /= sum;
}

float iou_aabb(float ax1, float ay1, float ax2, float ay2,
               float bx1, float by1, float bx2, float by2) noexcept
{
    const float ix1 = std::max(ax1, bx1), iy1 = std::max(ay1, by1);
    const float ix2 = std::min(ax2, bx2), iy2 = std::min(ay2, by2);
    if (ix2 <= ix1 || iy2 <= iy1) return 0.f;
    const float inter = (ix2 - ix1) * (iy2 - iy1);
    const float uni   = (ax2 - ax1) * (ay2 - ay1) + (bx2 - bx1) * (by2 - by1) - inter;
    return (uni > 0.f) ? inter / uni : 0.f;
}

namespace {
struct Cov { double a, b, c; };
inline Cov covariance(float w, float h, float angle) {
    const double a = (double)w * w / 12.0;
    const double b = (double)h * h / 12.0;
    const double cs = std::cos(angle), sn = std::sin(angle);
    return { a * cs * cs + b * sn * sn,
             a * sn * sn + b * cs * cs,
             (a - b) * cs * sn };
}
} // namespace

float probiou(float cx1, float cy1, float w1, float h1, float a1,
              float cx2, float cy2, float w2, float h2, float a2) noexcept
{
    constexpr double eps = 1e-7;
    const Cov A = covariance(w1, h1, a1);
    const Cov B = covariance(w2, h2, a2);
    const double dx = (double)cx1 - cx2, dy = (double)cy1 - cy2;

    const double sA = A.a + B.a, sB = A.b + B.b, sC = A.c + B.c;
    const double denom = sA * sB - sC * sC + eps;

    const double t1 = ((sA * dy * dy + sB * dx * dx) / denom) * 0.25;
    const double t2 = ((sC * dx * dy) / denom) * 0.5;

    const double d1 = A.a * A.b - A.c * A.c;
    const double d2 = B.a * B.b - B.c * B.c;
    const double t3 = std::log(denom / (4.0 * std::sqrt(std::max(d1, 0.0) * std::max(d2, 0.0)) + eps) + eps) * 0.5;

    double bd = t1 + t2 + t3;
    bd = std::min(std::max(bd, eps), 100.0);
    const double hd = std::sqrt(1.0 - std::exp(-bd));
    return static_cast<float>(1.0 - hd);
}

std::vector<Detection> nms(std::vector<Detection> dets, float iou_thresh, bool rotated)
{
    std::sort(dets.begin(), dets.end(),
              [](const Detection& a, const Detection& b) { return a.conf > b.conf; });

    std::vector<bool> suppressed(dets.size(), false);
    std::vector<Detection> kept;
    kept.reserve(dets.size());

    for (size_t i = 0; i < dets.size(); ++i) {
        if (suppressed[i]) continue;
        const Detection& a = dets[i];
        kept.push_back(a);
        for (size_t j = i + 1; j < dets.size(); ++j) {
            if (suppressed[j]) continue;
            const Detection& b = dets[j];
            if (a.cls != b.cls) continue;
            float ov;
            if (rotated) {
                ov = probiou(a.cx(), a.cy(), a.w, a.h, a.angle,
                             b.cx(), b.cy(), b.w, b.h, b.angle);
            } else {
                ov = iou_aabb(a.x, a.y, a.x + a.w, a.y + a.h,
                              b.x, b.y, b.x + b.w, b.y + b.h);
            }
            if (ov > iou_thresh) suppressed[j] = true;
        }
    }
    return kept;
}

std::vector<std::string> load_label_lines(const std::string& path)
{
    std::vector<std::string> out;
    std::ifstream f(path);
    if (!f) return out;
    std::string line;
    while (std::getline(f, line)) {
        const auto a = line.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) continue;
        const auto b = line.find_last_not_of(" \t\r\n");
        out.push_back(line.substr(a, b - a + 1));
    }
    return out;
}

const std::vector<std::string>& dota15_labels() noexcept
{
    static const std::vector<std::string> k = {
        "plane", "ship", "storage tank", "baseball diamond", "tennis court",
        "basketball court", "ground track field", "harbor", "bridge",
        "large vehicle", "small vehicle", "helicopter", "roundabout",
        "soccer ball field", "swimming pool"
    };
    return k;
}

const std::vector<std::pair<int, int>>& coco_pose_skeleton() noexcept
{
    static const std::vector<std::pair<int, int>> k = {
        {15, 13}, {13, 11}, {16, 14}, {14, 12}, {11, 12}, {5, 11}, {6, 12}, {5, 6},
        {5, 7}, {6, 8}, {7, 9}, {8, 10}, {1, 2}, {0, 1}, {0, 2}, {1, 3}, {2, 4}, {3, 5}, {4, 6}
    };
    return k;
}

} // namespace media::vdetect
