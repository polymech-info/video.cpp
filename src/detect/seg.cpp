#include "pm/video/detect/seg.hpp"
#include "pm/video/detect/commons.hpp"

#include "lib/onnx/OnnxSession.hpp"
#include "lib/onnx/OnnxImagePrep.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace media::vdetect {
namespace {

// ── Moore-neighbor boundary trace ─────────────────────────────────────────────
// Ordered outer boundary of the foreground blob in a w×h binary grid (1 = fg),
// clockwise, y-down. Returns the start pixel first. No deps.
std::vector<std::pair<int,int>> trace_boundary(const std::vector<uint8_t>& g, int w, int h)
{
    std::vector<std::pair<int,int>> out;
    const auto at = [&](int x, int y) {
        return (x >= 0 && x < w && y >= 0 && y < h) && g[static_cast<size_t>(y) * w + x];
    };
    int sx = -1, sy = -1;
    for (int y = 0; y < h && sy < 0; ++y)
        for (int x = 0; x < w; ++x)
            if (at(x, y)) { sx = x; sy = y; break; }
    if (sx < 0) return out;

    // Clockwise Moore neighborhood (start at West).
    static const int ox[8] = {-1, -1,  0,  1, 1, 1, 0, -1};
    static const int oy[8] = { 0, -1, -1, -1, 0, 1, 1,  1};

    int px = sx, py = sy;      // current boundary pixel
    int bx = sx - 1, by = sy;  // backtrack (background we came from)
    out.emplace_back(sx, sy);

    const int max_iter = w * h * 8 + 32;
    for (int it = 0; it < max_iter; ++it) {
        int d = 0;
        for (int k = 0; k < 8; ++k)
            if (px + ox[k] == bx && py + oy[k] == by) { d = k; break; }
        int found = -1;
        for (int s = 1; s <= 8; ++s) {
            const int k  = (d + s) & 7;
            const int nx = px + ox[k], ny = py + oy[k];
            if (at(nx, ny)) { found = k; break; }
            bx = nx; by = ny;  // last background before fg becomes backtrack
        }
        if (found < 0) break;  // isolated pixel
        px += ox[found]; py += oy[found];
        if (px == sx && py == sy) break;  // returned to start
        out.emplace_back(px, py);
        if (static_cast<int>(out.size()) > w * h + 8) break;  // safety
    }
    return out;
}

// ── Douglas-Peucker simplification (integer points) ───────────────────────────
double perp_dist2(const std::pair<int,int>& p,
                  const std::pair<int,int>& a, const std::pair<int,int>& b)
{
    const double dx = b.first - a.first, dy = b.second - a.second;
    const double len2 = dx * dx + dy * dy;
    if (len2 < 1e-9) {
        const double ex = p.first - a.first, ey = p.second - a.second;
        return ex * ex + ey * ey;
    }
    const double t = ((p.first - a.first) * dx + (p.second - a.second) * dy) / len2;
    const double cx = a.first + t * dx, cy = a.second + t * dy;
    const double ex = p.first - cx, ey = p.second - cy;
    return ex * ex + ey * ey;
}

void dp_simplify(const std::vector<std::pair<int,int>>& pts, int lo, int hi,
                 double eps2, std::vector<char>& keep)
{
    if (hi <= lo + 1) return;
    double max_d = 0.0; int max_i = -1;
    for (int i = lo + 1; i < hi; ++i) {
        const double d = perp_dist2(pts[i], pts[lo], pts[hi]);
        if (d > max_d) { max_d = d; max_i = i; }
    }
    if (max_d > eps2 && max_i > 0) {
        keep[max_i] = 1;
        dp_simplify(pts, lo, max_i, eps2, keep);
        dp_simplify(pts, max_i, hi, eps2, keep);
    }
}

std::vector<std::pair<int,int>> simplify(const std::vector<std::pair<int,int>>& pts, double eps)
{
    if (pts.size() <= 2) return pts;
    std::vector<char> keep(pts.size(), 0);
    keep.front() = keep.back() = 1;
    dp_simplify(pts, 0, static_cast<int>(pts.size()) - 1, eps * eps, keep);
    std::vector<std::pair<int,int>> out;
    for (size_t i = 0; i < pts.size(); ++i) if (keep[i]) out.push_back(pts[i]);
    return out;
}

struct Cand { Detection det; std::vector<float> coeffs; };

} // namespace

DecodeResult decode_segment(const media::onnx::RunResult&     run,
                            const media::onnx::LetterboxInfo& lb,
                            int                               orig_w,
                            int                               orig_h,
                            int                               input_size,
                            float                             conf_thresh,
                            float                             nms_thresh,
                            const std::vector<std::string>&   class_names)
{
    DecodeResult result;
    result.task = Task::Segment;

    const DetectHead head = parse_head(run);
    if (!head.ok || head.attrs < 6) {
        result.error = "segment: unexpected detection-head shape";
        return result;
    }
    // Prototype masks: output1 = [1, C, ph, pw].
    if (run.outputs_f32.size() < 2 || run.output_info.size() < 2) {
        result.error = "segment: missing prototype output";
        return result;
    }
    const auto& proto       = run.outputs_f32[1];
    const auto& proto_shape = run.output_info[1].shape;
    if (proto_shape.size() < 4) {
        result.error = "segment: unexpected prototype shape";
        return result;
    }
    const int pc = static_cast<int>(proto_shape[proto_shape.size() - 3]);
    const int ph = static_cast<int>(proto_shape[proto_shape.size() - 2]);
    const int pw = static_cast<int>(proto_shape[proto_shape.size() - 1]);
    if (pc <= 0 || ph <= 0 || pw <= 0 ||
        static_cast<size_t>(pc) * ph * pw > proto.size()) {
        result.error = "segment: bad prototype dims";
        return result;
    }
    if (input_size <= 0) input_size = 640;

    const auto& data = run.outputs_f32[0];
    const bool e2e   = (head.layout == HeadLayout::EndToEnd);
    const int  ncoef = pc;                              // mask coeffs == prototype count
    const int  coeff0 = e2e ? 6 : (head.attrs - ncoef); // first coeff attribute index
    const int  nc     = e2e ? 0 : (head.attrs - 4 - ncoef);
    if (coeff0 < (e2e ? 6 : 4) || coeff0 + ncoef > head.attrs) {
        result.error = "segment: coefficient layout mismatch";
        return result;
    }
    result.num_classes = e2e ? static_cast<int>(class_names.size()) : nc;

    const auto set_label = [&](Detection& d) {
        if (d.cls >= 0 && d.cls < static_cast<int>(class_names.size()))
            d.label = class_names[d.cls];
    };
    const auto read_coeffs = [&](int i) {
        std::vector<float> c(ncoef);
        for (int k = 0; k < ncoef; ++k) c[k] = head.at(data, coeff0 + k, i);
        return c;
    };

    // ── Decode boxes (+ coeffs) ────────────────────────────────────────────────
    std::vector<Cand> cands;
    cands.reserve(256);
    if (e2e) {
        for (int i = 0; i < head.count; ++i) {
            const float score = head.at(data, 4, i);
            if (score < conf_thresh) continue;
            float x1, y1, x2, y2;
            unmap_point(lb, orig_w, orig_h, input_size, head.at(data, 0, i), head.at(data, 1, i), x1, y1);
            unmap_point(lb, orig_w, orig_h, input_size, head.at(data, 2, i), head.at(data, 3, i), x2, y2);
            Detection d;
            d.x = x1; d.y = y1; d.w = x2 - x1; d.h = y2 - y1;
            d.conf = score; d.cls = static_cast<int>(head.at(data, 5, i) + 0.5f);
            set_label(d);
            cands.push_back({ std::move(d), read_coeffs(i) });
        }
    } else {
        for (int i = 0; i < head.count; ++i) {
            int best_cls = 0; float best = 0.f;
            for (int c = 0; c < nc; ++c) {
                const float v = maybe_sigmoid(head.at(data, 4 + c, i));
                if (v > best) { best = v; best_cls = c; }
            }
            if (best < conf_thresh) continue;
            const float cx = head.at(data, 0, i), cy = head.at(data, 1, i);
            const float bw = head.at(data, 2, i), bh = head.at(data, 3, i);
            float x1, y1, x2, y2;
            unmap_point(lb, orig_w, orig_h, input_size, cx - bw * 0.5f, cy - bh * 0.5f, x1, y1);
            unmap_point(lb, orig_w, orig_h, input_size, cx + bw * 0.5f, cy + bh * 0.5f, x2, y2);
            Detection d;
            d.x = x1; d.y = y1; d.w = x2 - x1; d.h = y2 - y1;
            d.conf = best; d.cls = best_cls;
            set_label(d);
            cands.push_back({ std::move(d), read_coeffs(i) });
        }
        // Class-aware axis-aligned NMS (carry coefficients with the kept boxes).
        std::sort(cands.begin(), cands.end(),
                  [](const Cand& a, const Cand& b) { return a.det.conf > b.det.conf; });
        std::vector<char> dead(cands.size(), 0);
        std::vector<Cand> kept;
        for (size_t i = 0; i < cands.size(); ++i) {
            if (dead[i]) continue;
            kept.push_back(std::move(cands[i]));
            const Detection& a = kept.back().det;
            for (size_t j = i + 1; j < cands.size(); ++j) {
                if (dead[j] || cands[j].det.cls != a.cls) continue;
                const Detection& b = cands[j].det;
                if (iou_aabb(a.x, a.y, a.x + a.w, a.y + a.h,
                             b.x, b.y, b.x + b.w, b.y + b.h) > nms_thresh)
                    dead[j] = 1;
            }
        }
        cands = std::move(kept);
    }

    // ── Per-box mask → contour (proto space → original pixels) ─────────────────
    const float proto_sx = static_cast<float>(pw) / input_size;  // input px → proto px
    const float proto_sy = static_cast<float>(ph) / input_size;
    // To crop the prototype we need the box in INPUT space — the inverse of
    // unmap (orig × scale + pad). Mirrors unmap_point's lb-vs-fallback choice.
    const bool  lb_ok    = (lb.scale > 0.f && lb.orig_w > 0);
    const float in_scale = lb_ok ? lb.scale
                                 : 1.f / unmap_scale(lb, orig_w, orig_h, input_size);
    const float pad_x = lb_ok ? static_cast<float>(lb.pad_left) : 0.f;
    const float pad_y = lb_ok ? static_cast<float>(lb.pad_top)  : 0.f;
    const auto orig_to_proto_x = [&](float ox) { return (ox * in_scale + pad_x) * proto_sx; };
    const auto orig_to_proto_y = [&](float oy) { return (oy * in_scale + pad_y) * proto_sy; };

    result.detections.reserve(cands.size());
    for (auto& cd : cands) {
        Detection d = std::move(cd.det);

        int px1 = static_cast<int>(std::floor(orig_to_proto_x(d.x)));
        int py1 = static_cast<int>(std::floor(orig_to_proto_y(d.y)));
        int px2 = static_cast<int>(std::ceil (orig_to_proto_x(d.x + d.w)));
        int py2 = static_cast<int>(std::ceil (orig_to_proto_y(d.y + d.h)));
        px1 = std::max(0, std::min(px1, pw)); px2 = std::max(0, std::min(px2, pw));
        py1 = std::max(0, std::min(py1, ph)); py2 = std::max(0, std::min(py2, ph));
        const int bw = px2 - px1, bh = py2 - py1;
        if (bw >= 2 && bh >= 2) {
            std::vector<uint8_t> mask(static_cast<size_t>(bw) * bh, 0);
            for (int yy = 0; yy < bh; ++yy) {
                const int gy = py1 + yy;
                for (int xx = 0; xx < bw; ++xx) {
                    const int gx = px1 + xx;
                    float acc = 0.f;
                    const size_t off = static_cast<size_t>(gy) * pw + gx;
                    for (int c = 0; c < ncoef; ++c)
                        acc += cd.coeffs[c] * proto[static_cast<size_t>(c) * ph * pw + off];
                    if (sigmoid(acc) > 0.5f) mask[static_cast<size_t>(yy) * bw + xx] = 1;
                }
            }
            auto poly = trace_boundary(mask, bw, bh);
            if (poly.size() >= 3) {
                const double eps = std::max(1.0, 0.012 * std::sqrt(double(bw) * bw + double(bh) * bh));
                poly = simplify(poly, eps);
                const float ux = 1.f / proto_sx, uy = 1.f / proto_sy;  // proto px → input px
                d.contour.reserve(poly.size());
                for (const auto& p : poly) {
                    float ox, oy;
                    unmap_point(lb, orig_w, orig_h, input_size,
                                (px1 + p.first) * ux, (py1 + p.second) * uy, ox, oy);
                    d.contour.emplace_back(ox, oy);
                }
            }
        }
        result.detections.push_back(std::move(d));
    }

    result.ok = true;
    return result;
}

} // namespace media::vdetect
