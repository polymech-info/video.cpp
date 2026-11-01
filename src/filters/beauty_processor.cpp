#include "pm/video/filters/beauty_processor.hpp"

#include "pm/video/filters/bg_effects.hpp"
#include "pm/video/filters/face_effects.hpp"

#include <algorithm>
#include <cmath>

namespace pm::video {
namespace {

void fill_rgb_from_bgr(const VideoFrame& frame, std::vector<std::uint8_t>& rgb) {
    const int w = frame.width;
    const int h = frame.height;
    const int src_stride = frame.stride > 0 ? frame.stride : w * 3;
    rgb.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3u);
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* src = frame.data.data() + static_cast<std::size_t>(y) * src_stride;
        std::uint8_t* dst = rgb.data() + static_cast<std::size_t>(y) * w * 3;
        for (int x = 0; x < w; ++x) {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            src += 3;
            dst += 3;
        }
    }
}

void put_bgr(VideoFrame& frame, int x, int y, std::uint8_t b, std::uint8_t g,
             std::uint8_t r) {
    if (x < 0 || y < 0 || x >= frame.width || y >= frame.height) return;
    const int stride = frame.stride > 0 ? frame.stride : frame.width * 3;
    std::uint8_t* p =
        frame.data.data() + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 3u;
    p[0] = b;
    p[1] = g;
    p[2] = r;
}

int pixel_coord(float v, int extent) {
    if (extent <= 0) return 0;
    // Tasks NormalizedLandmark is in image fractions; allow a little overshoot.
    if (v >= -0.25f && v <= 1.25f)
        return static_cast<int>(std::lround(v * static_cast<float>(extent - 1)));
    return static_cast<int>(std::lround(v));
}

} // namespace

void draw_landmark_overlay_bgr(VideoFrame& frame, const LandmarkSet& set) {
    if (set.empty() || frame.data.empty() || frame.width <= 0 || frame.height <= 0)
        return;
    const int radius = frame.width >= 256 ? 2 : 1;
    for (const auto& pt : set.points) {
        const int cx = pixel_coord(pt.x, frame.width);
        const int cy = pixel_coord(pt.y, frame.height);
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (dx * dx + dy * dy > radius * radius) continue;
                put_bgr(frame, cx + dx, cy + dy, 0, 220, 80);
            }
        }
    }
}

bool BeautyProcessor::create(const BeautyConfig& cfg, std::string& error) {
    error.clear();
    reset();
    cfg_ = cfg;
    if (cfg_.skin || cfg_.blur)
        gpu_.create();
    if (cfg_.background && !cfg_.bg_image.empty()) {
        if (!load_frame(cfg_.bg_image, bg_image_, error))
            return false;
        if (!is_packed_bgr24(bg_image_)) {
            VideoFrame packed;
            if (!to_bgr24(bg_image_, packed)) {
                error = "background image is not BGR24";
                return false;
            }
            bg_image_ = std::move(packed);
        }
    }
    if (cfg_.needs_mesh() && !landmarker_.create(cfg_, error))
        return false;
    if (cfg_.needs_seg() && !segmenter_.create(cfg_, error))
        return false;
    return true;
}

bool BeautyProcessor::process(VideoFrame& frame, std::int64_t timestamp_ms,
                              std::string& error) {
    error.clear();
    last_ = {};
    person_ = {};
    if (frame.width <= 0 || frame.height <= 0 || frame.data.empty()) {
        error = "empty frame";
        return false;
    }
    if (frame.format != PixelFormat::BGR24) {
        error = "BeautyProcessor expects packed BGR24";
        return false;
    }

    if (cfg_.mirror)
        apply_mirror_bgr(frame);

    const bool need_rgb = cfg_.needs_mesh() || cfg_.needs_seg();
    if (need_rgb)
        fill_rgb_from_bgr(frame, rgb_);

    if (cfg_.needs_mesh()) {
        if (!landmarker_.valid()) {
            error = "face landmarker is not created";
            return false;
        }
        if (!landmarker_.detect_rgb(rgb_.data(), frame.width, frame.height,
                                    frame.width * 3, timestamp_ms, last_, error))
            return false;
    }

    if (cfg_.skin)
        apply_skin_smooth_bgr(frame, last_, cfg_, &gpu_);
    if (cfg_.redness)
        apply_redness_mute_bgr(frame, last_, cfg_);
    if (cfg_.face_gain > 0.f || cfg_.warmth > 0.f)
        apply_face_grade_bgr(frame, last_, cfg_);
    if (cfg_.undereye)
        apply_undereye_lift_bgr(frame, last_, cfg_);
    if (cfg_.teeth)
        apply_teeth_whiten_bgr(frame, last_, cfg_);
    if (cfg_.lips)
        apply_lip_tint_bgr(frame, last_, cfg_);
    if (cfg_.eye_sharpen)
        apply_eye_sharpen_bgr(frame, last_, cfg_);

    if (cfg_.needs_seg()) {
        if (!segmenter_.valid()) {
            error = "image segmenter is not created";
            return false;
        }
        if (!segmenter_.segment_rgb(rgb_.data(), frame.width, frame.height,
                                    frame.width * 3, timestamp_ms, person_, error))
            return false;
        if (cfg_.blur)
            apply_bg_blur_bgr(frame, person_, cfg_, &gpu_);
        if (cfg_.background)
            apply_bg_replace_bgr(frame, person_, cfg_,
                                 bg_image_.data.empty() ? nullptr : &bg_image_);
    }

    if (cfg_.show_landmarks)
        draw_landmark_overlay_bgr(frame, last_);

    return true;
}

void BeautyProcessor::reset() noexcept {
    landmarker_.reset();
    segmenter_.reset();
    last_ = {};
    person_ = {};
    bg_image_ = {};
    rgb_.clear();
    gpu_.reset();
}

bool probe_beauty_runtime(const BeautyConfig& cfg, std::string& error) {
    error.clear();
    if (cfg.needs_mesh()) {
        const FaceStatus status = probe_face_landmarker(cfg);
        if (!status.available()) {
            error = status.error.empty() ? "face bridge not available" : status.error;
            return false;
        }
    }
    if (cfg.needs_seg()) {
        const SegStatus status = probe_image_segmenter(cfg);
        if (!status.available()) {
            error = status.error.empty() ? "image segmenter not available" : status.error;
            return false;
        }
    }
    return true;
}

} // namespace pm::video
