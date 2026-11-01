#pragma once

#include "pm/video/filters/beauty_config.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace pm::video {

enum class SegCapability {
    Available,
    DllNotFound,
    LoadFailed,
    ExportMissing,
    AbiMismatch,
    ModelNotFound,
};

struct SegStatus {
    bool compiled = true;
    SegCapability capability = SegCapability::DllNotFound;
    std::string dll_path;
    std::string model_path;
    std::uint32_t abi_expected = 1;
    std::uint32_t abi_actual = 0;
    std::vector<std::string> attempted_dlls;
    std::vector<std::string> attempted_models;
    std::string error;

    bool available() const noexcept {
        return capability == SegCapability::Available;
    }
};

struct PersonMask {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> data;

    bool empty() const noexcept {
        return width <= 0 || height <= 0 || data.empty();
    }
};

class ImageSegmenter {
public:
    ImageSegmenter() = default;
    ImageSegmenter(const ImageSegmenter&) = delete;
    ImageSegmenter& operator=(const ImageSegmenter&) = delete;
    ImageSegmenter(ImageSegmenter&& other) noexcept;
    ImageSegmenter& operator=(ImageSegmenter&& other) noexcept;
    ~ImageSegmenter();

    bool create(const BeautyConfig& cfg, std::string& error);
    bool segment_rgb(const std::uint8_t* rgb, int width, int height, int stride,
                     std::int64_t timestamp_ms, PersonMask& out,
                     std::string& error);
    void reset() noexcept;
    bool valid() const noexcept { return handle_ != nullptr; }

private:
    void* handle_ = nullptr;
};

SegStatus probe_image_segmenter(const BeautyConfig& cfg);

} // namespace pm::video
