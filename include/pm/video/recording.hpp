#pragma once

#include <cstdint>
#include <string>

namespace pm::video {

enum class RecordingState {
    idle,
    starting,
    recording,
    failed,
    stopped,
};

struct RecordingStatus {
    RecordingState state = RecordingState::idle;
    std::string capture_backend;
    std::string effective_encoder;
    std::string error;
    /// Non-fatal soft warning (e.g. interactive zoom/HUD unavailable).
    /// Does not mark the session failed.
    std::string warning;
    int width = 0;
    int height = 0;
    int fps = 0;
    int bitrate_kbps = 0;
    double zoom = 1.0;
    std::string follow;
    int viewport_left = 0;
    int viewport_top = 0;
    int viewport_width = 0;
    int viewport_height = 0;
    bool paused = false;
    std::uint32_t pause_reasons = 0;
    std::uint64_t frames_written = 0;
    std::uint64_t frames_dropped = 0;

    bool active() const noexcept
    {
        return state == RecordingState::starting ||
               state == RecordingState::recording;
    }

    bool failed() const noexcept { return state == RecordingState::failed; }
};

inline const char* recording_state_name(RecordingState state) noexcept
{
    switch (state) {
    case RecordingState::idle: return "idle";
    case RecordingState::starting: return "starting";
    case RecordingState::recording: return "recording";
    case RecordingState::failed: return "failed";
    case RecordingState::stopped: return "stopped";
    }
    return "unknown";
}

} // namespace pm::video
