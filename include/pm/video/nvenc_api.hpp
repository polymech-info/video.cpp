#pragma once

#include <cstddef>
#include <cstdint>

#define PM_NVENC_ABI_VERSION 1U

#if defined(_WIN32)
#  if defined(PM_NVENC_PLUGIN_BUILD)
#    define PM_NVENC_API extern "C" __declspec(dllexport)
#  else
#    define PM_NVENC_API extern "C"
#  endif
#else
#  define PM_NVENC_API extern "C"
#endif

enum PmNvencPacketFlags : std::uint32_t {
    PM_NVENC_PACKET_KEYFRAME = 1U << 0,
};

struct PmNvencConfig {
    std::uint32_t abi_version;
    void*         d3d11_device;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t fps;
    std::uint32_t bitrate_kbps;
};

struct PmNvencPacket {
    const std::uint8_t* data;
    std::size_t         size;
    std::int64_t        timestamp_hns;
    std::uint32_t       flags;
};

using PmNvencHandle = void*;

PM_NVENC_API std::uint32_t pm_nvenc_abi_version();
PM_NVENC_API int pm_nvenc_create(const PmNvencConfig* config,
                                 PmNvencHandle* out_handle,
                                 char* error,
                                 std::size_t error_capacity);
PM_NVENC_API int pm_nvenc_get_sequence(PmNvencHandle handle,
                                       const std::uint8_t** data,
                                       std::size_t* size);
PM_NVENC_API int pm_nvenc_encode(PmNvencHandle handle,
                                 void* d3d11_texture,
                                 std::uint32_t source_left,
                                 std::uint32_t source_top,
                                 std::int64_t timestamp_hns,
                                 const PmNvencPacket** packets,
                                 std::size_t* packet_count,
                                 char* error,
                                 std::size_t error_capacity);
PM_NVENC_API int pm_nvenc_flush(PmNvencHandle handle,
                                const PmNvencPacket** packets,
                                std::size_t* packet_count,
                                char* error,
                                std::size_t error_capacity);
PM_NVENC_API void pm_nvenc_destroy(PmNvencHandle handle);

