#include "pm/video/filters/image_segmenter.hpp"
#include "pm/video/text.hpp"

#include "pm/video/hooks.hpp"
#include "pm/video/dynlib.hpp"

#include "pixlwiz_image_segmenter.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <utility>

namespace pm::video {
namespace {

namespace fs = std::filesystem;

constexpr const char* kDllName = "pixlwiz_face_landmarker.dll";
constexpr const char* kModelName = "selfie_segmenter.task";

struct SegApi {
    using AbiVersion = std::uint32_t (*)();
    using Create = int (*)(const char*, int, pm_image_segmenter**, char*, std::size_t);
    using Segment = int (*)(pm_image_segmenter*, const std::uint8_t*, int, int, int,
                            std::int64_t, pm_seg_result*, char*, std::size_t);
    using Destroy = void (*)(pm_image_segmenter*);

    void* module = nullptr;
    AbiVersion abi_version = nullptr;
    Create create = nullptr;
    Segment segment = nullptr;
    Destroy destroy = nullptr;
    std::string loaded_path;
    std::string error;
    SegCapability capability = SegCapability::DllNotFound;
    std::uint32_t abi_actual = 0;
    int live_handles = 0;
};

SegApi& api() noexcept {
    static SegApi state;
    return state;
}

std::mutex& api_mutex() {
    static std::mutex m;
    return m;
}

void remember(std::vector<std::string>& attempted, const fs::path& path) {
    if (path.empty()) return;
    attempted.push_back(pm::video::path_to_utf8(path));
}

std::vector<fs::path> dll_search_dirs(const std::string& dll_override) {
    std::vector<fs::path> dirs;
    if (!dll_override.empty()) {
        const fs::path override_path(dll_override);
        if (override_path.has_parent_path())
            dirs.push_back(override_path.parent_path());
        else
            dirs.push_back(fs::current_path());
    }
    const fs::path exe_dir = pm::video::exe_dir();
    if (!exe_dir.empty()) {
        dirs.push_back(exe_dir);
        dirs.push_back(exe_dir / ".." / "vendor" / "mediapipe-tasks");
        dirs.push_back(exe_dir / ".." / ".." / "packages" / "mediapipe-tasks" /
                       "prebuilt" / "win-x64" / "bin");
    }
    return dirs;
}

std::vector<fs::path> model_candidates(const std::string& model_override) {
    std::vector<fs::path> out;
    if (!model_override.empty())
        out.emplace_back(model_override);
    const fs::path exe_dir = pm::video::exe_dir();
    if (!exe_dir.empty()) {
        out.push_back(exe_dir / ".." / "models" / "vision" / kModelName);
        out.push_back(exe_dir / ".." / "models" / "vision" / "selfie_segmenter.tflite");
        out.push_back(pm::video::embedding_models_dir() / "vision" / kModelName);
        out.push_back(exe_dir / ".." / ".." / "packages" / "mediapipe-tasks" /
                      "prebuilt" / "win-x64" / "models" / kModelName);
        out.push_back(exe_dir / ".." / ".." / "packages" / "mediapipe-tasks" /
                      "prebuilt" / "win-x64" / "models" / "selfie_segmenter.tflite");
        out.push_back(exe_dir / ".." / "vendor" / "mediapipe-tasks" / "models" /
                      kModelName);
    }
    out.push_back(pm::video::models_dir() / "vision" / kModelName);
    return out;
}

std::string resolve_existing(const std::vector<fs::path>& candidates,
                             std::vector<std::string>& attempted) {
    for (const auto& path : candidates) {
        remember(attempted, path);
        std::error_code ec;
        if (fs::is_regular_file(path, ec))
            return pm::video::path_to_utf8(fs::weakly_canonical(path, ec));
    }
    return {};
}

bool load_dll(const std::string& dll_override, SegApi& state,
              std::vector<std::string>& attempted) {
    if (state.module)
        return true;

    std::string name = kDllName;
    if (!dll_override.empty()) {
        const fs::path override_path(dll_override);
        if (override_path.has_filename())
            name = pm::video::path_to_utf8(override_path.filename());
        remember(attempted, override_path);
        const std::string search = override_path.has_parent_path()
                                       ? pm::video::path_to_utf8(override_path.parent_path())
                                       : std::string{};
        state.module = pm::video::dlib::load(name, search);
        if (state.module)
            state.loaded_path = pm::video::path_to_utf8(override_path);
    }

    auto bind_seg = [&](void* module) -> bool {
        state.abi_version = reinterpret_cast<SegApi::AbiVersion>(
            pm::video::dlib::sym(module, "pm_seg_abi_version"));
        state.create = reinterpret_cast<SegApi::Create>(
            pm::video::dlib::sym(module, "pm_seg_create"));
        state.segment = reinterpret_cast<SegApi::Segment>(
            pm::video::dlib::sym(module, "pm_seg_segment_rgb"));
        state.destroy = reinterpret_cast<SegApi::Destroy>(
            pm::video::dlib::sym(module, "pm_seg_destroy"));
        if (state.abi_version && state.create && state.segment && state.destroy)
            return true;
        pm::video::dlib::unload(module);
        state.abi_version = nullptr;
        state.create = nullptr;
        state.segment = nullptr;
        state.destroy = nullptr;
        return false;
    };

    if (state.module && !bind_seg(state.module))
        state.module = nullptr;

    if (!state.module) {
        for (const auto& dir : dll_search_dirs(dll_override)) {
            const fs::path full = dir / kDllName;
            remember(attempted, full);
            void* module = pm::video::dlib::load(kDllName, pm::video::path_to_utf8(dir));
            if (!module)
                continue;
            if (!bind_seg(module))
                continue;
            state.module = module;
            state.loaded_path = pm::video::path_to_utf8(full);
            break;
        }
    }

    if (!state.module) {
        remember(attempted, pm::video::path_from_utf8(kDllName));
        if (pm::video::dlib::probe(kDllName)) {
            void* module = pm::video::dlib::load(kDllName);
            if (module && bind_seg(module)) {
                state.module = module;
                state.loaded_path = kDllName;
            }
        }
    }

    if (!state.module) {
        for (const auto& path : attempted) {
            std::error_code ec;
            if (fs::is_regular_file(path, ec)) {
                state.capability = SegCapability::ExportMissing;
                state.error = "pixlwiz_face_landmarker.dll is missing pm_seg_* "
                              "(rebuild packages/mediapipe-tasks)";
                return false;
            }
        }
        state.capability = SegCapability::DllNotFound;
        state.error = "pixlwiz_face_landmarker.dll not found";
        return false;
    }

    state.abi_actual = state.abi_version();
    if (state.abi_actual != PM_SEG_ABI_VERSION) {
        state.capability = SegCapability::AbiMismatch;
        state.error = "pixlwiz_face_landmarker.dll segmenter ABI mismatch";
        return false;
    }

    state.capability = SegCapability::Available;
    state.error.clear();
    return true;
}

int running_mode_from_config(const BeautyConfig& cfg) {
    if (cfg.running_mode == "image")
        return PM_SEG_MODE_IMAGE;
    return PM_SEG_MODE_VIDEO;
}

} // namespace

ImageSegmenter::ImageSegmenter(ImageSegmenter&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)) {}

ImageSegmenter& ImageSegmenter::operator=(ImageSegmenter&& other) noexcept {
    if (this != &other) {
        reset();
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

ImageSegmenter::~ImageSegmenter() { reset(); }

void ImageSegmenter::reset() noexcept {
    if (!handle_) return;
    std::lock_guard<std::mutex> lock(api_mutex());
    auto& state = api();
    if (state.destroy)
        state.destroy(static_cast<pm_image_segmenter*>(handle_));
    handle_ = nullptr;
    if (state.live_handles > 0)
        --state.live_handles;
}

static SegStatus probe_image_segmenter_locked(const BeautyConfig& cfg) {
    SegStatus status;
    status.abi_expected = PM_SEG_ABI_VERSION;
    auto& state = api();
    const bool dll_ok = load_dll(cfg.dll_path, state, status.attempted_dlls);
    status.dll_path = state.loaded_path;
    status.abi_actual = state.abi_actual;
    status.model_path = resolve_existing(model_candidates(cfg.model_seg),
                                         status.attempted_models);
    if (!dll_ok) {
        status.capability = state.capability;
        status.error = state.error;
        return status;
    }
    if (status.model_path.empty()) {
        status.capability = SegCapability::ModelNotFound;
        status.error = "selfie_segmenter.task not found";
        return status;
    }

    status.capability = SegCapability::Available;
    status.error.clear();
    return status;
}

bool ImageSegmenter::create(const BeautyConfig& cfg, std::string& error) {
    error.clear();
    reset();

    std::lock_guard<std::mutex> lock(api_mutex());
    const SegStatus status = probe_image_segmenter_locked(cfg);
    if (!status.available()) {
        error = status.error;
        return false;
    }

    auto& state = api();
    std::array<char, 1024> message{};
    pm_image_segmenter* created = nullptr;
    if (state.create(status.model_path.c_str(), running_mode_from_config(cfg),
                     &created, message.data(), message.size()) != 0 ||
        !created) {
        error = message[0] ? message.data() : "pm_seg_create failed";
        return false;
    }
    handle_ = created;
    ++state.live_handles;
    return true;
}

bool ImageSegmenter::segment_rgb(const std::uint8_t* rgb, int width, int height,
                                 int stride, std::int64_t timestamp_ms,
                                 PersonMask& out, std::string& error) {
    error.clear();
    out = {};
    std::lock_guard<std::mutex> lock(api_mutex());
    auto& state = api();
    if (!handle_ || !state.segment) {
        error = "image segmenter is not created";
        return false;
    }
    std::array<char, 1024> message{};
    pm_seg_result result{};
    if (state.segment(static_cast<pm_image_segmenter*>(handle_), rgb, width, height,
                      stride, timestamp_ms, &result, message.data(),
                      message.size()) != 0) {
        error = message[0] ? message.data() : "pm_seg_segment_rgb failed";
        return false;
    }
    if (result.width <= 0 || result.height <= 0 || !result.mask) {
        error = "pm_seg_segment_rgb returned an empty mask";
        return false;
    }
    out.width = result.width;
    out.height = result.height;
    const int src_stride = result.stride > 0 ? result.stride : result.width;
    out.data.resize(static_cast<std::size_t>(out.width) * out.height);
    for (int y = 0; y < out.height; ++y) {
        std::memcpy(out.data.data() + static_cast<std::size_t>(y) * out.width,
                    result.mask + static_cast<std::size_t>(y) * src_stride,
                    static_cast<std::size_t>(out.width));
    }
    return true;
}

SegStatus probe_image_segmenter(const BeautyConfig& cfg) {
    std::lock_guard<std::mutex> lock(api_mutex());
    return probe_image_segmenter_locked(cfg);
}

} // namespace pm::video
