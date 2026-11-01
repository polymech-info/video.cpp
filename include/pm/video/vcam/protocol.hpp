#pragma once

// Shared-memory contract between pixlwiz (producer) and tanit_vcam_source.dll
// (Frame Server media source). POD only — the DLL must not include pixlwiz headers.
//
// Frames live in %ProgramData%\Pixlwiz\vcam.frames (file-backed mapping).
// Frame Server is session 0 and cannot see Local\ named objects.
//
// VCM2 is two NV12 slots. The writer fills the unpublished slot, then publishes
// `slot`. Never clear `ready` after the first good frame — Zoom samples often
// and treats ready=0 as “no camera” (color bars).

#include <cstdint>

namespace pm::vcam {

inline constexpr wchar_t kFramesFileLeaf[] = L"vcam.frames";

inline constexpr std::uint32_t kMagicVcm1 = 0x56434D31u; // 'VCM1' single buffer
inline constexpr std::uint32_t kMagic = 0x56434D32u;     // 'VCM2' double buffer
inline constexpr int kWidth = 1280;
inline constexpr int kHeight = 720;
inline constexpr int kFps = 30;
inline constexpr int kNv12Bytes = kWidth * kHeight * 3 / 2;
inline constexpr int kSlots = 2;
inline constexpr int kShmBytes = 256 + kSlots * kNv12Bytes;

// {8F3C2A11-4B6E-4D91-9C2A-7E1B0C4D5E6F}
inline constexpr wchar_t kSourceClsidW[] = L"{8F3C2A11-4B6E-4D91-9C2A-7E1B0C4D5E6F}";

#pragma pack(push, 8)
struct ShmHeader {
    std::uint32_t magic;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t fps;
    std::uint32_t frame_id;
    std::int64_t timestamp_ms;
    std::uint32_t nv12_bytes;
    std::uint32_t ready;
    std::uint32_t slot; // published slot 0 or 1; writer fills 1-slot
};
#pragma pack(pop)

inline std::uint32_t shm_slot(const ShmHeader* h) {
    if (!h) return 0;
    if (h->magic == kMagic)
        return h->slot & 1u;
    return 0; // VCM1: pixels always at slot 0
}

inline bool shm_magic_ok(const ShmHeader* h) {
    return h && (h->magic == kMagic || h->magic == kMagicVcm1);
}

inline std::uint8_t* shm_nv12(ShmHeader* h, std::uint32_t slot) {
    return reinterpret_cast<std::uint8_t*>(h) + 256 +
           static_cast<std::uint32_t>(kNv12Bytes) * (slot & 1u);
}
inline const std::uint8_t* shm_nv12(const ShmHeader* h, std::uint32_t slot) {
    return reinterpret_cast<const std::uint8_t*>(h) + 256 +
           static_cast<std::uint32_t>(kNv12Bytes) * (slot & 1u);
}

} // namespace pm::vcam
