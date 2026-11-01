#include "pm/video/filters/face_landmarker.hpp"

#include "pm/video/hooks.hpp"
#include "pm/video/dynlib.hpp"

#include "pixlwiz_face_landmarker.h"

#include <array>
#include <filesystem>
#include <mutex>
#include <utility>

namespace pm::video {
namespace {

namespace fs = std::filesystem;

constexpr const char* kDllName = "pixlwiz_face_landmarker.dll";
constexpr const char* kModelName = "face_landmarker_v2.task";

struct FaceApi {
    using AbiVersion = std::uint32_t (*)();
    using Create = int (*)(const char*, int, pm_face_landmarker**, char*, std::size_t);
    using Detect = int (*)(pm_face_landmarker*, const std::uint8_t*, int, int, int,
                           std::int64_t, pm_face_result*, char*, std::size_t);
    using Destroy = void (*)(pm_face_landmarker*);

    void* module = nullptr;
    AbiVersion abi_version = nullptr;
    Create create = nullptr;
    Detect detect = nullptr;
    Destroy destroy = nullptr;
    std::string loaded_path;
    std::string error;
    FaceCapability capability = FaceCapability::DllNotFound;
    std::uint32_t abi_actual = 0;
    int live_handles = 0;
};

FaceApi& api() noexcept {
    static FaceApi state;
    return state;
}

std::mutex& api_mutex() {
    static std::mutex m;
    return m;
}

void remember(std::vector<std::string>& attempted, const fs::path& path) {
    if (path.empty()) return;
    attempted.push_back(path.string());
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
        out.push_back(pm::video::embedding_models_dir() / "vision" / kModelName);
        // Same layout as dll_search_dirs: staged package, then vendor copy.
        out.push_back(exe_dir / ".." / ".." / "packages" / "mediapipe-tasks" /
                      "prebuilt" / "win-x64" / "models" / kModelName);
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
            return fs::weakly_canonical(path, ec).string();
    }
    return {};
}

bool load_dll(const std::string& dll_override, FaceApi& state,
              std::vector<std::string>& attempted) {
    if (state.module)
        return true;

    std::string name = kDllName;
    if (!dll_override.empty()) {
        const fs::path override_path(dll_override);
        if (override_path.has_filename())
            name = override_path.filename().string();
        remember(attempted, override_path);
        const std::string search = override_path.has_parent_path()
                                       ? override_path.parent_path().string()
                                       : std::string{};
        state.module = pm::video::dlib::load(name, search);
        if (state.module)
            state.loaded_path = override_path.string();
    }

    auto bind_face = [&](void* module) -> bool {
        state.abi_version = reinterpret_cast<FaceApi::AbiVersion>(
            pm::video::dlib::sym(module, "pm_face_abi_version"));
        state.create = reinterpret_cast<FaceApi::Create>(
            pm::video::dlib::sym(module, "pm_face_create"));
        state.detect = reinterpret_cast<FaceApi::Detect>(
            pm::video::dlib::sym(module, "pm_face_detect_rgb"));
        state.destroy = reinterpret_cast<FaceApi::Destroy>(
            pm::video::dlib::sym(module, "pm_face_destroy"));
        if (state.abi_version && state.create && state.detect && state.destroy)
            return true;
        pm::video::dlib::unload(module);
        state.abi_version = nullptr;
        state.create = nullptr;
        state.detect = nullptr;
        state.destroy = nullptr;
        return false;
    };

    if (state.module && !bind_face(state.module))
        state.module = nullptr;

    if (!state.module) {
        for (const auto& dir : dll_search_dirs(dll_override)) {
            const fs::path full = dir / kDllName;
            remember(attempted, full);
            void* module = pm::video::dlib::load(kDllName, dir.string());
            if (!module)
                continue;
            if (!bind_face(module))
                continue;
            state.module = module;
            state.loaded_path = full.string();
            break;
        }
    }

    if (!state.module) {
        remember(attempted, fs::path(kDllName));
        if (pm::video::dlib::probe(kDllName)) {
            void* module = pm::video::dlib::load(kDllName);
            if (module && bind_face(module)) {
                state.module = module;
                state.loaded_path = kDllName;
            }
        }
    }

    if (!state.module) {
        for (const auto& path : attempted) {
            std::error_code ec;
            if (fs::is_regular_file(path, ec)) {
                state.capability = FaceCapability::LoadFailed;
                state.error = "pixlwiz_face_landmarker.dll exists but could not be loaded";
                return false;
            }
        }
        state.capability = FaceCapability::DllNotFound;
        state.error = "pixlwiz_face_landmarker.dll not found";
        return false;
    }

    state.abi_actual = state.abi_version();
    if (state.abi_actual != PM_FACE_ABI_VERSION) {
        state.capability = FaceCapability::AbiMismatch;
        state.error = "pixlwiz_face_landmarker.dll ABI mismatch";
        pm::video::dlib::unload(state.module);
        state.module = nullptr;
        return false;
    }

    state.capability = FaceCapability::Available;
    state.error.clear();
    return true;
}

int running_mode_from_config(const BeautyConfig& cfg) {
    if (cfg.running_mode == "image")
        return PM_FACE_MODE_IMAGE;
    return PM_FACE_MODE_VIDEO;
}

} // namespace

FaceLandmarker::FaceLandmarker(FaceLandmarker&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)) {}

FaceLandmarker& FaceLandmarker::operator=(FaceLandmarker&& other) noexcept {
    if (this != &other) {
        reset();
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

FaceLandmarker::~FaceLandmarker() { reset(); }

void FaceLandmarker::reset() noexcept {
    if (!handle_) return;
    std::lock_guard<std::mutex> lock(api_mutex());
    auto& state = api();
    if (state.destroy)
        state.destroy(static_cast<pm_face_landmarker*>(handle_));
    handle_ = nullptr;
    if (state.live_handles > 0)
        --state.live_handles;
}

static FaceStatus probe_face_landmarker_locked(const BeautyConfig& cfg) {
    FaceStatus status;
    status.abi_expected = PM_FACE_ABI_VERSION;
    auto& state = api();
    if (!load_dll(cfg.dll_path, state, status.attempted_dlls)) {
        status.capability = state.capability;
        status.dll_path = state.loaded_path;
        status.abi_actual = state.abi_actual;
        status.error = state.error;
        return status;
    }

    status.dll_path = state.loaded_path;
    status.abi_actual = state.abi_actual;
    status.model_path = resolve_existing(model_candidates(cfg.model_face),
                                         status.attempted_models);
    if (status.model_path.empty()) {
        status.capability = FaceCapability::ModelNotFound;
        status.error = "face_landmarker_v2.task not found";
        return status;
    }

    status.capability = FaceCapability::Available;
    status.error.clear();
    return status;
}

bool FaceLandmarker::create(const BeautyConfig& cfg, std::string& error) {
    error.clear();
    reset();

    std::lock_guard<std::mutex> lock(api_mutex());
    const FaceStatus status = probe_face_landmarker_locked(cfg);
    if (!status.available()) {
        error = status.error;
        return false;
    }

    auto& state = api();
    std::array<char, 1024> message{};
    pm_face_landmarker* created = nullptr;
    if (state.create(status.model_path.c_str(), running_mode_from_config(cfg),
                     &created, message.data(), message.size()) != 0 ||
        !created) {
        error = message[0] ? message.data() : "pm_face_create failed";
        return false;
    }
    handle_ = created;
    ++state.live_handles;
    return true;
}

bool FaceLandmarker::detect_rgb(const std::uint8_t* rgb, int width, int height,
                                int stride, std::int64_t timestamp_ms,
                                LandmarkSet& out, std::string& error) {
    error.clear();
    out = {};
    std::lock_guard<std::mutex> lock(api_mutex());
    auto& state = api();
    if (!handle_ || !state.detect) {
        error = "face landmarker is not created";
        return false;
    }
    std::array<char, 1024> message{};
    pm_face_result result{};
    if (state.detect(static_cast<pm_face_landmarker*>(handle_), rgb, width, height,
                     stride, timestamp_ms, &result, message.data(),
                     message.size()) != 0) {
        error = message[0] ? message.data() : "pm_face_detect_rgb failed";
        return false;
    }
    out.face_count = result.face_count;
    out.landmarks_per_face = result.landmarks_per_face;
    const std::size_t n =
        static_cast<std::size_t>(result.face_count) * result.landmarks_per_face;
    if (n == 0)
        return true;
    if (!result.landmarks) {
        error = "pm_face_detect_rgb returned faces without landmark data";
        return false;
    }
    out.points.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.points[i].x = result.landmarks[i].x;
        out.points[i].y = result.landmarks[i].y;
        out.points[i].z = result.landmarks[i].z;
    }
    return true;
}

FaceStatus probe_face_landmarker(const BeautyConfig& cfg) {
    std::lock_guard<std::mutex> lock(api_mutex());
    return probe_face_landmarker_locked(cfg);
}

} // namespace pm::video
