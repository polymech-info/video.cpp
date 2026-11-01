#pragma once

#include "pm/video/filters/beauty_config.hpp"
#include "pm/video/filters/landmarks.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace pm::video {

enum class FaceCapability {
    Available,
    DllNotFound,
    LoadFailed,
    ExportMissing,
    AbiMismatch,
    ModelNotFound,
};

struct FaceStatus {
    bool compiled = true;
    FaceCapability capability = FaceCapability::DllNotFound;
    std::string dll_path;
    std::string model_path;
    std::uint32_t abi_expected = 1;
    std::uint32_t abi_actual = 0;
    std::vector<std::string> attempted_dlls;
    std::vector<std::string> attempted_models;
    std::string error;

    bool available() const noexcept {
        return capability == FaceCapability::Available;
    }
};

class FaceLandmarker {
public:
    FaceLandmarker() = default;
    FaceLandmarker(const FaceLandmarker&) = delete;
    FaceLandmarker& operator=(const FaceLandmarker&) = delete;
    FaceLandmarker(FaceLandmarker&& other) noexcept;
    FaceLandmarker& operator=(FaceLandmarker&& other) noexcept;
    ~FaceLandmarker();

    bool create(const BeautyConfig& cfg, std::string& error);
    bool detect_rgb(const std::uint8_t* rgb, int width, int height, int stride,
                    std::int64_t timestamp_ms, LandmarkSet& out,
                    std::string& error);
    void reset() noexcept;
    bool valid() const noexcept { return handle_ != nullptr; }

private:
    void* handle_ = nullptr;
};

FaceStatus probe_face_landmarker(const BeautyConfig& cfg);

} // namespace pm::video
