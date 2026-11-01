#pragma once

#include <cstdint>

namespace pm::video {

/// Independent pause owners. Recording writes encoder samples only when
/// `pause_reasons == PauseNone`. Auto-idle never toggles user pause.
enum PauseReason : std::uint32_t {
    PauseNone   = 0,
    PauseUser   = 1u << 0,
    PauseIdle   = 1u << 1,
    PauseSystem = 1u << 2,
};

inline constexpr bool pause_recording_active(std::uint32_t reasons) noexcept
{
    return reasons == PauseNone;
}

inline constexpr bool pause_has(std::uint32_t reasons, PauseReason bit) noexcept
{
    return (reasons & static_cast<std::uint32_t>(bit)) != 0;
}

inline constexpr std::uint32_t pause_set(
    std::uint32_t reasons, PauseReason bit, bool on) noexcept
{
    const auto b = static_cast<std::uint32_t>(bit);
    return on ? (reasons | b) : (reasons & ~b);
}

} // namespace pm::video
