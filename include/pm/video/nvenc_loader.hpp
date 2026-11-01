#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11Texture2D;

namespace media::win::recorder {

struct NvencEncodedPacket {
    std::vector<std::uint8_t> bytes;
    std::int64_t              timestamp_hns = 0;
    bool                      keyframe = false;
};

class NvencPlugin {
public:
    NvencPlugin();
    ~NvencPlugin();
    NvencPlugin(const NvencPlugin&) = delete;
    NvencPlugin& operator=(const NvencPlugin&) = delete;

    bool create(ID3D11Device* device,
                int width,
                int height,
                int fps,
                int bitrate_kbps,
                std::string& error);
    bool encode(ID3D11Texture2D* texture,
                std::uint32_t source_left,
                std::uint32_t source_top,
                std::int64_t timestamp_hns,
                std::vector<NvencEncodedPacket>& packets,
                std::string& error);
    bool flush(std::vector<NvencEncodedPacket>& packets, std::string& error);

    bool active() const noexcept;
    const std::vector<std::uint8_t>& sequence_header() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_;
};

} // namespace media::win::recorder

