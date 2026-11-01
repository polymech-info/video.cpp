#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pm::video {

// Minimal RIFF/AVI container of MJPEG frames. Header counts are patched in finalize().
class MjpegAviWriter {
public:
    MjpegAviWriter(const std::string& path, int width, int height, int fps);
    ~MjpegAviWriter();

    MjpegAviWriter(const MjpegAviWriter&) = delete;
    MjpegAviWriter& operator=(const MjpegAviWriter&) = delete;

    void write_frame(const std::uint8_t* jpeg_data, std::size_t jpeg_size);
    void finalize();

private:
    struct FrameEntry {
        std::uint32_t movi_offset = 0;
        std::uint32_t data_size = 0;
    };

    void write_u32(std::uint32_t v);
    void patch_u32(long offset, std::uint32_t value);
    void write_headers();

    FILE* fp_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int fps_ = 30;
    int frame_count_ = 0;
    std::size_t max_frame_bytes_ = 1;
    long off_riff_size_ = 0;
    long off_avih_total_frames_ = 0;
    long off_avih_max_bps_ = 0;
    long off_strh_length_ = 0;
    long off_movi_size_ = 0;
    long off_movi_data_ = 0;
    std::vector<FrameEntry> index_;
};

} // namespace pm::video
