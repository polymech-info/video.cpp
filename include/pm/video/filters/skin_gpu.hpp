#pragma once

// D3D11 skin smooth for the face-oval ROI. Hardware only; caller keeps CPU
// fallback. MediaPipe stays on CPU.

#include "pm/video/filters/landmarks.hpp"

#include "pm/video/capture.hpp"

#include <cstdint>
#include <memory>

namespace pm::video {

class SkinGpu {
public:
    SkinGpu();
    ~SkinGpu();

    SkinGpu(const SkinGpu&) = delete;
    SkinGpu& operator=(const SkinGpu&) = delete;

    bool create();
    bool valid() const noexcept;
    /// Downsample + blur + upsample + mask blend into `frame`'s ROI.
    /// `mask` is roi.w * roi.h, 8-bit. Returns false → use CPU.
    bool smooth_roi(VideoFrame& frame, const IRect& roi, const std::uint8_t* mask,
                    float strength);
    void reset() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pm::video
