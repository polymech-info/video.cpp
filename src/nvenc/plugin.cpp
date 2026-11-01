#include "pm/video/nvenc_api.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>

#include "NvEncoder/NvEncoderD3D11.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace {

void set_error(char* dst, std::size_t capacity, const std::string& message) noexcept
{
    if (!dst || capacity == 0) return;
    const std::size_t size = std::min(capacity - 1, message.size());
    if (size) std::memcpy(dst, message.data(), size);
    dst[size] = '\0';
}

struct NvencSession {
    ID3D11Device*                     device = nullptr;
    ID3D11DeviceContext*              context = nullptr;
    std::unique_ptr<NvEncoderD3D11>   encoder;
    std::uint32_t                     width = 0;
    std::uint32_t                     height = 0;
    std::vector<std::uint8_t>         sequence;
    std::vector<NvEncOutputFrame>      encoded;
    std::vector<PmNvencPacket>         packet_views;
    std::deque<std::int64_t>           pending_timestamps;
    std::uint64_t                      submitted_frames = 0;
    bool                               flushed = false;

    ~NvencSession()
    {
        if (encoder) {
            try {
                if (!flushed) {
                    std::vector<NvEncOutputFrame> ignored;
                    encoder->EndEncode(ignored);
                }
                encoder->DestroyEncoder();
            } catch (...) {
            }
            encoder.reset();
        }
        if (context) context->Release();
        if (device) device->Release();
    }

    void publish_packets()
    {
        packet_views.clear();
        packet_views.reserve(encoded.size());
        for (const auto& packet : encoded) {
            const std::int64_t timestamp = pending_timestamps.empty()
                ? 0 : pending_timestamps.front();
            if (!pending_timestamps.empty()) pending_timestamps.pop_front();
            std::uint32_t flags = 0;
            if (packet.pictureType == NV_ENC_PIC_TYPE_IDR ||
                packet.pictureType == NV_ENC_PIC_TYPE_I) {
                flags |= PM_NVENC_PACKET_KEYFRAME;
            }
            packet_views.push_back({
                packet.frame.data(), packet.frame.size(), timestamp, flags
            });
        }
    }
};

NvencSession* session(PmNvencHandle handle) noexcept
{
    return static_cast<NvencSession*>(handle);
}

} // namespace

PM_NVENC_API std::uint32_t pm_nvenc_abi_version()
{
    return PM_NVENC_ABI_VERSION;
}

PM_NVENC_API int pm_nvenc_create(const PmNvencConfig* config,
                                 PmNvencHandle* out_handle,
                                 char* error,
                                 std::size_t error_capacity)
{
    if (out_handle) *out_handle = nullptr;
    if (!config || !out_handle || config->abi_version != PM_NVENC_ABI_VERSION ||
        !config->d3d11_device || config->width < 2 || config->height < 2 ||
        config->fps == 0) {
        set_error(error, error_capacity, "invalid NVENC configuration");
        return 0;
    }

    try {
        auto state = std::make_unique<NvencSession>();
        state->device = static_cast<ID3D11Device*>(config->d3d11_device);
        state->device->AddRef();
        state->device->GetImmediateContext(&state->context);
        state->width = config->width;
        state->height = config->height;

        state->encoder = std::make_unique<NvEncoderD3D11>(
            state->device, state->width, state->height,
            NV_ENC_BUFFER_FORMAT_ARGB, 0);

        NV_ENC_INITIALIZE_PARAMS initialize = {NV_ENC_INITIALIZE_PARAMS_VER};
        NV_ENC_CONFIG encode = {NV_ENC_CONFIG_VER};
        initialize.encodeConfig = &encode;
        state->encoder->CreateDefaultEncoderParams(
            &initialize, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P4_GUID,
            NV_ENC_TUNING_INFO_HIGH_QUALITY);

        const std::uint32_t bitrate = config->bitrate_kbps > 0
            ? config->bitrate_kbps * 1000U
            : static_cast<std::uint32_t>(std::clamp<std::uint64_t>(
                static_cast<std::uint64_t>(state->width) * state->height *
                    config->fps / 8U,
                1'000'000ULL, 24'000'000ULL));
        initialize.frameRateNum = config->fps;
        initialize.frameRateDen = 1;
        initialize.enablePTD = 1;
        encode.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
        encode.gopLength = std::max<std::uint32_t>(config->fps * 2U, 1U);
        encode.frameIntervalP = 1;
        encode.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
        encode.rcParams.averageBitRate = bitrate;
        encode.rcParams.maxBitRate = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(
                static_cast<std::uint64_t>(bitrate) * 3U / 2U,
                100'000'000ULL));
        encode.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
        encode.encodeCodecConfig.h264Config.idrPeriod = encode.gopLength;
        encode.encodeCodecConfig.h264Config.disableSPSPPS = 0;
        encode.encodeCodecConfig.h264Config.outputAUD = 0;

        state->encoder->CreateEncoder(&initialize);
        state->encoder->GetSequenceParams(state->sequence);
        *out_handle = state.release();
        return 1;
    } catch (const std::exception& ex) {
        set_error(error, error_capacity, ex.what() ? ex.what() : "NVENC create failed");
        return 0;
    } catch (...) {
        set_error(error, error_capacity, "NVENC create failed");
        return 0;
    }
}

PM_NVENC_API int pm_nvenc_get_sequence(PmNvencHandle handle,
                                       const std::uint8_t** data,
                                       std::size_t* size)
{
    auto* state = session(handle);
    if (!state || !data || !size) return 0;
    *data = state->sequence.data();
    *size = state->sequence.size();
    return 1;
}

PM_NVENC_API int pm_nvenc_encode(PmNvencHandle handle,
                                 void* d3d11_texture,
                                 std::uint32_t source_left,
                                 std::uint32_t source_top,
                                 std::int64_t timestamp_hns,
                                 const PmNvencPacket** packets,
                                 std::size_t* packet_count,
                                 char* error,
                                 std::size_t error_capacity)
{
    auto* state = session(handle);
    if (packets) *packets = nullptr;
    if (packet_count) *packet_count = 0;
    if (!state || !d3d11_texture || !packets || !packet_count || state->flushed) {
        set_error(error, error_capacity, "invalid NVENC encode request");
        return 0;
    }

    try {
        auto* source = static_cast<ID3D11Texture2D*>(d3d11_texture);
        D3D11_TEXTURE2D_DESC source_desc{};
        source->GetDesc(&source_desc);
        if (source_left + state->width > source_desc.Width ||
            source_top + state->height > source_desc.Height) {
            set_error(error, error_capacity, "NVENC source region is outside texture bounds");
            return 0;
        }

        const NvEncInputFrame* input = state->encoder->GetNextInputFrame();
        auto* destination = static_cast<ID3D11Texture2D*>(input->inputPtr);
        const D3D11_BOX box{
            source_left, source_top, 0,
            source_left + state->width, source_top + state->height, 1
        };
        state->context->CopySubresourceRegion(
            destination, 0, 0, 0, 0, source, 0, &box);

        state->pending_timestamps.push_back(timestamp_hns);
        state->encoded.clear();
        NV_ENC_PIC_PARAMS picture{};
        if (state->submitted_frames++ == 0)
            picture.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;
        state->encoder->EncodeFrame(state->encoded, &picture);
        state->publish_packets();
        *packets = state->packet_views.data();
        *packet_count = state->packet_views.size();
        return 1;
    } catch (const std::exception& ex) {
        set_error(error, error_capacity, ex.what() ? ex.what() : "NVENC encode failed");
        return 0;
    } catch (...) {
        set_error(error, error_capacity, "NVENC encode failed");
        return 0;
    }
}

PM_NVENC_API int pm_nvenc_flush(PmNvencHandle handle,
                                const PmNvencPacket** packets,
                                std::size_t* packet_count,
                                char* error,
                                std::size_t error_capacity)
{
    auto* state = session(handle);
    if (packets) *packets = nullptr;
    if (packet_count) *packet_count = 0;
    if (!state || !packets || !packet_count) {
        set_error(error, error_capacity, "invalid NVENC flush request");
        return 0;
    }
    if (state->flushed) return 1;

    try {
        state->encoded.clear();
        state->encoder->EndEncode(state->encoded);
        state->flushed = true;
        state->publish_packets();
        *packets = state->packet_views.data();
        *packet_count = state->packet_views.size();
        return 1;
    } catch (const std::exception& ex) {
        set_error(error, error_capacity, ex.what() ? ex.what() : "NVENC flush failed");
        return 0;
    } catch (...) {
        set_error(error, error_capacity, "NVENC flush failed");
        return 0;
    }
}

PM_NVENC_API void pm_nvenc_destroy(PmNvencHandle handle)
{
    delete session(handle);
}

