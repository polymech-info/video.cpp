#include "pm/video/backends.hpp"

namespace pm::video {

nlohmann::json backend_capabilities_json()
{
    nlohmann::json body = {
        {"ok", true},
        {"capture", {
#if defined(_WIN32)
            {"mediaFoundation", "compiled"},
            {"v4l2", "unsupported"},
            {"avfoundation", "unsupported"},
#elif defined(__APPLE__)
            {"mediaFoundation", "unsupported"},
            {"v4l2", "unsupported"},
            {"avfoundation", "compiled"},
#elif defined(__linux__)
            {"mediaFoundation", "unsupported"},
            {"v4l2", "compiled"},
            {"avfoundation", "unsupported"},
#else
            {"mediaFoundation", "unsupported"},
            {"v4l2", "unsupported"},
            {"avfoundation", "unsupported"},
#endif
        }},
        {"encode", {
            {"mjpegAvi", "compiled"},
#if defined(_WIN32) && defined(PM_VIDEO_RECORD_ENGINES) && PM_VIDEO_RECORD_ENGINES
            {"mediaFoundation", "compiled"},
#else
            {"mediaFoundation", "unsupported"},
#endif
#if defined(_WIN32)
            {"nvenc", "plugin"},
#else
            {"nvenc", "unsupported"},
#endif
        }},
        {"webrtc", {
#if defined(PM_VIDEO_HAS_WEBRTC) && PM_VIDEO_HAS_WEBRTC
            {"signaling", "compiled"},
#else
            {"signaling", "unsupported"},
#endif
            {"transport", "unsupported"},
        }},
        {"note", "nvenc plugin means the loader is compiled; the DLL may still be absent"},
    };
    return body;
}

} // namespace pm::video
