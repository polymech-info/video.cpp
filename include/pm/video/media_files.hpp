#pragma once

#include <algorithm>
#include "fs/path_sanitizer.hpp"
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include "string/string_utils.hpp"

namespace pm::video {

inline std::string lower_extension(std::string_view path)
{
    const auto dot = path.find_last_of('.');
    if (dot == std::string_view::npos || dot + 1 >= path.size())
        return {};
    std::string ext(path.substr(dot + 1));
    ext = media::strutil::lower_ascii(std::move(ext));
    return ext;
}

inline bool is_still_image_path(std::string_view path)
{
    const std::string ext = lower_extension(path);
    return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "bmp";
}

inline bool is_native_container_path(const std::filesystem::path& path)
{
    std::string ext = media::path::path_to_utf8(path.extension());
    ext = media::strutil::lower_ascii(std::move(ext));
    return ext == ".mp4" || ext == ".mov";
}

// Frame time that never goes backwards. fps <= 0 keeps the source timestamp.
inline std::int64_t monotonic_media_timestamp(std::int64_t frame_ts,
                                              long long index,
                                              double fps,
                                              std::int64_t& prev)
{
    std::int64_t cand = frame_ts;
    if (fps > 1e-3)
        cand = std::max(cand, static_cast<std::int64_t>(index * 1000.0 / fps));
    if (cand <= prev)
        cand = prev + 1;
    prev = cand;
    return cand;
}

// True when an encoder error is a missing or unloadable NVENC plugin.
inline bool nvenc_plugin_absent(const std::string& message)
{
    std::string m = message;
    m = media::strutil::lower_ascii(std::move(m));
    const bool mentions_nvenc = m.find("nvenc") != std::string::npos;
    const bool missing =
        m.find("not installed") != std::string::npos ||
        m.find("unavailable") != std::string::npos ||
        m.find("could not") != std::string::npos ||
        m.find("does not include") != std::string::npos ||
        m.find("incomplete api") != std::string::npos ||
        m.find("abi version") != std::string::npos ||
        m.find("failed to load") != std::string::npos;
    return mentions_nvenc && missing;
}

} // namespace pm::video
