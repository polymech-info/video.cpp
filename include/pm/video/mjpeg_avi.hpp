#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pm::video {

// True when `file_bytes - 8` fits in the RIFF chunk-size field.
inline bool avi_riff_size_fits(std::uint64_t file_bytes) noexcept
{
    return file_bytes >= 8 && (file_bytes - 8) <= 0xFFFFFFFFull;
}

// Minimal RIFF/AVI container of MJPEG frames. Header counts are patched in finalize().
//
// write_frame returns false on a bad buffer, a short write, or when another frame
// would push the file past the 32-bit RIFF limit. A rejected frame is not added
// to the index. finalize() is idempotent. The destructor finalizes if the caller
// did not. The writer is not safe to use from two threads.
class MjpegAviWriter {
public:
    MjpegAviWriter(const std::string& path, int width, int height, int fps);
    ~MjpegAviWriter();

    MjpegAviWriter(const MjpegAviWriter&) = delete;
    MjpegAviWriter& operator=(const MjpegAviWriter&) = delete;

    bool write_frame(const std::uint8_t* jpeg_data, std::size_t jpeg_size);
    bool finalize();

private:
    struct FrameEntry {
        std::uint32_t movi_offset = 0;
        std::uint32_t data_size = 0;
    };

    bool write_raw(const void* data, std::size_t size);
    bool write_u32(std::uint32_t v);
    bool patch_u32(std::uint64_t offset, std::uint32_t value);
    bool write_headers();
    bool room_for(std::uint64_t more) const;

    FILE* fp_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int fps_ = 30;
    int frame_count_ = 0;
    bool finalized_ = false;
    bool failed_ = false;
    std::uint64_t bytes_ = 0;
    std::size_t max_frame_bytes_ = 1;
    std::uint64_t off_riff_size_ = 0;
    std::uint64_t off_avih_total_frames_ = 0;
    std::uint64_t off_avih_max_bps_ = 0;
    std::uint64_t off_strh_length_ = 0;
    std::uint64_t off_movi_size_ = 0;
    std::uint64_t off_movi_data_ = 0;
    std::vector<FrameEntry> index_;
};

} // namespace pm::video
