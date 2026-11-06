#pragma once

// Opaque hardware-buffer reference. The generic frame API does not expose a
// D3D11 texture or an Android AHardwareBuffer. A platform adapter fills `map`,
// which copies or wraps the buffer into a CPU VideoFrame. Unset map means the
// buffer cannot be read on the CPU.

#include "pm/video/frame.hpp"

namespace pm::video {

struct HardwareBufferRef {
    void* handle = nullptr;
    const char* api = "";
    bool (*map)(void* handle, VideoFrame& out, void* user) = nullptr;
    void* user = nullptr;
};

inline bool map_hardware_buffer(const HardwareBufferRef& buffer, VideoFrame& out)
{
    if (!buffer.handle || !buffer.map || !buffer.api || buffer.api[0] == '\0')
        return false;
    return buffer.map(buffer.handle, out, buffer.user);
}

} // namespace pm::video
