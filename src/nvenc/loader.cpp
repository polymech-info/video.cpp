#include "pm/video/nvenc_loader.hpp"
#include "pm/video/text.hpp"

#include "pm/video/nvenc_api.hpp"

#include "pm/video/hooks.hpp"
#include "pm/video/dynlib.hpp"

#include <array>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace media::win::recorder {
namespace {

struct NvencApi {
    using AbiVersion = std::uint32_t(*)();
    using Create = int(*)(const PmNvencConfig*, PmNvencHandle*, char*, std::size_t);
    using GetSequence = int(*)(PmNvencHandle, const std::uint8_t**, std::size_t*);
    using Encode = int(*)(PmNvencHandle, void*, std::uint32_t, std::uint32_t,
                          std::int64_t, const PmNvencPacket**, std::size_t*,
                          char*, std::size_t);
    using Flush = int(*)(PmNvencHandle, const PmNvencPacket**, std::size_t*,
                         char*, std::size_t);
    using Destroy = void(*)(PmNvencHandle);

    void*       module = nullptr;
    AbiVersion  abi_version = nullptr;
    Create      create = nullptr;
    GetSequence get_sequence = nullptr;
    Encode      encode = nullptr;
    Flush       flush = nullptr;
    Destroy     destroy = nullptr;
    std::string error;
};

const NvencApi& api() noexcept
{
    static NvencApi state;
    static std::once_flag once;
    std::call_once(once, [] {
        const auto plugin_path =
            pm::video::exe_dir() / "pm-video-nvenc.dll";
        state.module = pm::video::dlib::load(
            "pm-video-nvenc.dll",
            pm::video::path_to_utf8(pm::video::exe_dir()));
        if (!state.module) {
#if defined(_WIN32)
            const DWORD load_error = ::GetLastError();
#endif
            std::error_code file_error;
            const bool installed = std::filesystem::is_regular_file(
                plugin_path, file_error);
            if (!installed) {
                state.error = "NVENC plugin is not installed at " +
                              pm::video::path_to_utf8(plugin_path) +
                              " (install it and restart the process)";
            } else {
#if defined(_WIN32)
                state.error = "NVENC plugin exists but Windows could not load it"
                              " (error " + std::to_string(load_error) + ": " +
                              std::system_category().message(
                                  static_cast<int>(load_error)) +
                              "); check DLL architecture and dependencies";
#else
                state.error = "NVENC plugin exists but could not be loaded; "
                              "check its dependencies";
#endif
            }
            return;
        }

        state.abi_version = reinterpret_cast<NvencApi::AbiVersion>(
            pm::video::dlib::sym(state.module, "pm_nvenc_abi_version"));
        state.create = reinterpret_cast<NvencApi::Create>(
            pm::video::dlib::sym(state.module, "pm_nvenc_create"));
        state.get_sequence = reinterpret_cast<NvencApi::GetSequence>(
            pm::video::dlib::sym(state.module, "pm_nvenc_get_sequence"));
        state.encode = reinterpret_cast<NvencApi::Encode>(
            pm::video::dlib::sym(state.module, "pm_nvenc_encode"));
        state.flush = reinterpret_cast<NvencApi::Flush>(
            pm::video::dlib::sym(state.module, "pm_nvenc_flush"));
        state.destroy = reinterpret_cast<NvencApi::Destroy>(
            pm::video::dlib::sym(state.module, "pm_nvenc_destroy"));
        if (!state.abi_version || !state.create || !state.get_sequence ||
            !state.encode || !state.flush || !state.destroy) {
            state.error = "pm-video-nvenc.dll has an incomplete API";
            pm::video::dlib::unload(state.module);
            state.module = nullptr;
            return;
        }
        if (state.abi_version() != PM_NVENC_ABI_VERSION) {
            state.error = "pm-video-nvenc.dll ABI version mismatch";
            pm::video::dlib::unload(state.module);
            state.module = nullptr;
        }
        // Keep a valid plugin mapped for process lifetime. Encoder sessions can
        // then be created and destroyed without racing DLL unload.
    });
    return state;
}

void copy_packets(const PmNvencPacket* source,
                  std::size_t count,
                  std::vector<NvencEncodedPacket>& destination)
{
    destination.clear();
    destination.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        NvencEncodedPacket packet;
        if (source[i].data && source[i].size) {
            packet.bytes.assign(source[i].data, source[i].data + source[i].size);
        }
        packet.timestamp_hns = source[i].timestamp_hns;
        packet.keyframe = (source[i].flags & PM_NVENC_PACKET_KEYFRAME) != 0;
        destination.push_back(std::move(packet));
    }
}

} // namespace

struct NvencPlugin::Impl {
    PmNvencHandle handle = nullptr;
    std::vector<std::uint8_t> sequence;
};

NvencPlugin::NvencPlugin() : m_(std::make_unique<Impl>()) {}

NvencPlugin::~NvencPlugin()
{
    if (m_->handle) {
        const auto& loaded = api();
        if (loaded.destroy) loaded.destroy(m_->handle);
    }
}

bool NvencPlugin::create(ID3D11Device* device,
                         int width,
                         int height,
                         int fps,
                         int bitrate_kbps,
                         std::string& error)
{
    error.clear();
    const auto& loaded = api();
    if (!loaded.module) {
        error = loaded.error;
        return false;
    }

    PmNvencConfig config{
        PM_NVENC_ABI_VERSION,
        device,
        static_cast<std::uint32_t>(width),
        static_cast<std::uint32_t>(height),
        static_cast<std::uint32_t>(fps),
        static_cast<std::uint32_t>(bitrate_kbps),
    };
    std::array<char, 1024> message{};
    if (!loaded.create(&config, &m_->handle, message.data(), message.size()) ||
        !m_->handle) {
        error = message[0] ? message.data() : "NVENC session creation failed";
        return false;
    }

    const std::uint8_t* sequence = nullptr;
    std::size_t sequence_size = 0;
    if (!loaded.get_sequence(m_->handle, &sequence, &sequence_size) ||
        !sequence || sequence_size == 0) {
        loaded.destroy(m_->handle);
        m_->handle = nullptr;
        error = "NVENC did not return an H.264 sequence header";
        return false;
    }
    m_->sequence.assign(sequence, sequence + sequence_size);
    return true;
}

bool NvencPlugin::encode(ID3D11Texture2D* texture,
                         std::uint32_t source_left,
                         std::uint32_t source_top,
                         std::int64_t timestamp_hns,
                         std::vector<NvencEncodedPacket>& packets,
                         std::string& error)
{
    error.clear();
    const auto& loaded = api();
    if (!m_->handle || !loaded.encode) {
        error = "NVENC session is not active";
        return false;
    }
    const PmNvencPacket* views = nullptr;
    std::size_t count = 0;
    std::array<char, 1024> message{};
    if (!loaded.encode(m_->handle, texture, source_left, source_top,
                       timestamp_hns, &views, &count,
                       message.data(), message.size())) {
        error = message[0] ? message.data() : "NVENC encode failed";
        return false;
    }
    copy_packets(views, count, packets);
    return true;
}

bool NvencPlugin::flush(std::vector<NvencEncodedPacket>& packets,
                        std::string& error)
{
    error.clear();
    const auto& loaded = api();
    if (!m_->handle || !loaded.flush) {
        packets.clear();
        return true;
    }
    const PmNvencPacket* views = nullptr;
    std::size_t count = 0;
    std::array<char, 1024> message{};
    if (!loaded.flush(m_->handle, &views, &count,
                      message.data(), message.size())) {
        error = message[0] ? message.data() : "NVENC flush failed";
        return false;
    }
    copy_packets(views, count, packets);
    return true;
}

bool NvencPlugin::active() const noexcept
{
    return m_->handle != nullptr;
}

const std::vector<std::uint8_t>& NvencPlugin::sequence_header() const noexcept
{
    return m_->sequence;
}

} // namespace media::win::recorder

