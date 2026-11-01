#include "pm/video/capture.hpp"

#include <algorithm>
#include <sstream>
#include <string>

namespace pm::video {
namespace {

bool mode_less(const DeviceMode& a, const DeviceMode& b)
{
    const int area_a = a.width * a.height;
    const int area_b = b.width * b.height;
    if (area_a != area_b)
        return area_a > area_b;
    const double fa = a.fps_den > 0 ? static_cast<double>(a.fps_num) / a.fps_den : 0;
    const double fb = b.fps_den > 0 ? static_cast<double>(b.fps_num) / b.fps_den : 0;
    if (fa != fb)
        return fa > fb;
    return a.format < b.format;
}

std::string lower_copy(std::string value)
{
    for (char& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

} // namespace

void sort_device_modes(std::vector<DeviceMode>& modes)
{
    std::sort(modes.begin(), modes.end(), mode_less);
}

std::vector<DeviceMode> sorted_device_modes(const std::string& device_name)
{
    auto modes = enumerate_device_modes(device_name);
    sort_device_modes(modes);
    return modes;
}

bool resolve_device_mode(const std::string& device_name,
                         int mode_idx,
                         int& width,
                         int& height,
                         int& fps,
                         std::string& err)
{
    if (mode_idx < 0)
        return true;
    const auto modes = sorted_device_modes(device_name);
    if (static_cast<std::size_t>(mode_idx) >= modes.size()) {
        std::ostringstream o;
        o << "video: --mode " << mode_idx << " is out of range (device has "
          << modes.size() << " mode(s); use `video info --modes` to list them)";
        err = o.str();
        return false;
    }
    const auto& m = modes[static_cast<std::size_t>(mode_idx)];
    width = m.width;
    height = m.height;
    fps = m.fps_den > 0 ? static_cast<int>(m.fps_num / m.fps_den) : m.fps_num;
    return true;
}

bool capture_device_present(const std::string& needle)
{
    if (needle.empty())
        return true;
    const std::string want = lower_copy(needle);
    for (const auto& device : enumerate_capture_devices()) {
        if (lower_copy(device.name).find(want) != std::string::npos)
            return true;
    }
    return false;
}

std::string friendly_capture_name(const std::string& camera_input)
{
    const auto videos = enumerate_capture_devices();
    if (!camera_input.empty()) {
        const std::string want = lower_copy(camera_input);
        for (const auto& device : videos) {
            if (lower_copy(device.name).find(want) != std::string::npos)
                return device.name;
        }
        return camera_input;
    }
    for (const auto& device : videos) {
        if (device.is_default && !device.name.empty())
            return device.name;
    }
    if (!videos.empty())
        return videos.front().name;
    return {};
}

} // namespace pm::video
