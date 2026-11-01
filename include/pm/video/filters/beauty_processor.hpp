#pragma once

#include "pm/video/filters/beauty_config.hpp"
#include "pm/video/filters/face_landmarker.hpp"
#include "pm/video/filters/image_segmenter.hpp"
#include "pm/video/filters/landmarks.hpp"
#include "pm/video/filters/skin_gpu.hpp"

#include "pm/video/capture.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace pm::video {

class BeautyProcessor {
public:
    bool create(const BeautyConfig& cfg, std::string& error);
    bool process(VideoFrame& frame, std::int64_t timestamp_ms, std::string& error);
    const LandmarkSet& last_landmarks() const noexcept { return last_; }
    const PersonMask& last_person() const noexcept { return person_; }
    const BeautyConfig& config() const noexcept { return cfg_; }
    bool skin_gpu() const noexcept { return gpu_.valid(); }
    void reset() noexcept;

private:
    BeautyConfig cfg_{};
    FaceLandmarker landmarker_;
    ImageSegmenter segmenter_;
    LandmarkSet last_{};
    PersonMask person_{};
    VideoFrame bg_image_{};
    std::vector<std::uint8_t> rgb_;
    SkinGpu gpu_;
};

void draw_landmark_overlay_bgr(VideoFrame& frame, const LandmarkSet& set);

bool probe_beauty_runtime(const BeautyConfig& cfg, std::string& error);

} // namespace pm::video
