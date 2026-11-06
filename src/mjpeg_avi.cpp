#include "pm/video/mjpeg_avi.hpp"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace pm::video {
namespace {

bool checked_image_bytes(int width, int height, std::uint32_t& out)
{
    if (width <= 0 || height <= 0) return false;
    if (width > (std::numeric_limits<int>::max() / 3)) return false;
    const int row = width * 3;
    if (static_cast<std::uint64_t>(height) >
        (static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) /
         static_cast<std::uint64_t>(row)))
        return false;
    out = static_cast<std::uint32_t>(row) * static_cast<std::uint32_t>(height);
    return true;
}

bool seek_bytes(FILE* fp, std::uint64_t offset)
{
#if defined(_WIN32)
    return _fseeki64(fp, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
    return fseeko(fp, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

} // namespace

MjpegAviWriter::MjpegAviWriter(const std::string& path, int width, int height, int fps)
    : width_(width), height_(height), fps_(fps > 0 ? fps : 30)
{
    std::uint32_t image_bytes = 0;
    if (!checked_image_bytes(width_, height_, image_bytes))
        throw std::runtime_error("video: invalid AVI dimensions");
#if defined(_WIN32)
    const int n = ::MultiByteToWideChar(
        CP_UTF8, 0, path.data(), static_cast<int>(path.size()), nullptr, 0);
    std::wstring wpath(n > 0 ? static_cast<std::size_t>(n) : 0, L'\0');
    if (n > 0)
        ::MultiByteToWideChar(
            CP_UTF8, 0, path.data(), static_cast<int>(path.size()), wpath.data(), n);
    fp_ = n > 0 ? _wfopen(wpath.c_str(), L"wb") : nullptr;
#else
    fp_ = std::fopen(path.c_str(), "wb");
#endif
    if (!fp_)
        throw std::runtime_error("video: cannot create file: " + path);
    if (!write_headers()) {
        std::fclose(fp_);
        fp_ = nullptr;
        throw std::runtime_error("video: cannot write AVI header");
    }
}

MjpegAviWriter::~MjpegAviWriter()
{
    if (fp_)
        finalize();
}

bool MjpegAviWriter::room_for(std::uint64_t more) const
{
    constexpr std::uint64_t kMaxFile = 0xFFFFFFFFull;
    return more <= kMaxFile && bytes_ <= kMaxFile - more && avi_riff_size_fits(bytes_ + more);
}

bool MjpegAviWriter::write_frame(const std::uint8_t* jpeg_data, std::size_t jpeg_size)
{
    if (finalized_ || failed_ || !fp_) return false;
    if (!jpeg_data || jpeg_size == 0 || jpeg_size > 0xFFFFFFFFull) return false;
    if (frame_count_ == std::numeric_limits<int>::max()) return false;

    const std::uint64_t chunk = 8ull + static_cast<std::uint64_t>(jpeg_size) + (jpeg_size & 1u);
    const std::uint64_t index_bytes = 8ull + (static_cast<std::uint64_t>(index_.size()) + 1ull) * 16ull;
    if (!room_for(chunk + index_bytes)) return false;

    const std::uint64_t movi_offset = bytes_ - off_movi_data_;
    if (movi_offset > 0xFFFFFFFFull) return false;

    if (!write_raw("00dc", 4)) return false;
    if (!write_u32(static_cast<std::uint32_t>(jpeg_size))) return false;
    if (!write_raw(jpeg_data, jpeg_size)) return false;
    if (jpeg_size & 1u) {
        const unsigned char pad = 0;
        if (!write_raw(&pad, 1)) return false;
    }
    if (jpeg_size > max_frame_bytes_)
        max_frame_bytes_ = jpeg_size;
    index_.push_back({static_cast<std::uint32_t>(movi_offset), static_cast<std::uint32_t>(jpeg_size)});
    ++frame_count_;
    return true;
}

bool MjpegAviWriter::finalize()
{
    if (finalized_) return !failed_;
    if (!fp_) return false;
    if (failed_) {
        std::fclose(fp_);
        fp_ = nullptr;
        finalized_ = true;
        return false;
    }

    const std::uint64_t idx1_start = bytes_;
    const auto idx_size = static_cast<std::uint32_t>(index_.size() * 16);
    if (!write_raw("idx1", 4) || !write_u32(idx_size)) {
        failed_ = true;
        std::fclose(fp_);
        fp_ = nullptr;
        finalized_ = true;
        return false;
    }
    for (const auto& entry : index_) {
        if (!write_raw("00dc", 4) || !write_u32(0x10u) ||
            !write_u32(entry.movi_offset) || !write_u32(entry.data_size)) {
            failed_ = true;
            std::fclose(fp_);
            fp_ = nullptr;
            finalized_ = true;
            return false;
        }
    }

    const std::uint64_t file_end = bytes_;
    if (!avi_riff_size_fits(file_end)) {
        failed_ = true;
        std::fclose(fp_);
        fp_ = nullptr;
        finalized_ = true;
        return false;
    }
    const std::uint64_t bps = static_cast<std::uint64_t>(max_frame_bytes_) * static_cast<std::uint32_t>(fps_);
    const auto movi_sz = static_cast<std::uint32_t>(idx1_start - off_movi_data_ + 4);
    const bool patched =
        patch_u32(off_riff_size_, static_cast<std::uint32_t>(file_end - 8)) &&
        patch_u32(off_movi_size_, movi_sz) &&
        patch_u32(off_avih_total_frames_, static_cast<std::uint32_t>(frame_count_)) &&
        patch_u32(off_avih_max_bps_, bps > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(bps)) &&
        patch_u32(off_strh_length_, static_cast<std::uint32_t>(frame_count_));
    std::fclose(fp_);
    fp_ = nullptr;
    finalized_ = true;
    if (!patched) failed_ = true;
    return patched;
}

bool MjpegAviWriter::write_raw(const void* data, std::size_t size)
{
    if (!fp_ || failed_) return false;
    if (size == 0) return true;
    if (!data) {
        failed_ = true;
        return false;
    }
    const std::size_t wrote = std::fwrite(data, 1, size, fp_);
    if (wrote != size) {
        failed_ = true;
        return false;
    }
    bytes_ += static_cast<std::uint64_t>(size);
    return true;
}

bool MjpegAviWriter::write_u32(std::uint32_t v)
{
    return write_raw(&v, 4);
}

bool MjpegAviWriter::patch_u32(std::uint64_t offset, std::uint32_t value)
{
    if (!fp_) return false;
    if (!seek_bytes(fp_, offset)) return false;
    if (std::fwrite(&value, 4, 1, fp_) != 1) return false;
    return seek_bytes(fp_, bytes_);
}

bool MjpegAviWriter::write_headers()
{
    std::uint32_t image_bytes = 0;
    if (!checked_image_bytes(width_, height_, image_bytes)) return false;
    const std::uint32_t strl_data_sz = 4 + 64 + 48;
    const std::uint32_t hdrl_data_sz = 4 + 64 + (8 + strl_data_sz);

    if (!write_raw("RIFF", 4)) return false;
    off_riff_size_ = bytes_;
    if (!write_u32(0) || !write_raw("AVI ", 4)) return false;

    if (!write_raw("LIST", 4) || !write_u32(hdrl_data_sz) || !write_raw("hdrl", 4)) return false;

    if (!write_raw("avih", 4) || !write_u32(56)) return false;
    if (!write_u32(fps_ > 0 ? (1000000u / static_cast<std::uint32_t>(fps_)) : 33333u)) return false;
    off_avih_max_bps_ = bytes_;
    if (!write_u32(0) || !write_u32(0) || !write_u32(0x10u)) return false;
    off_avih_total_frames_ = bytes_;
    if (!write_u32(0) || !write_u32(0) || !write_u32(1) || !write_u32(0)) return false;
    if (!write_u32(static_cast<std::uint32_t>(width_)) || !write_u32(static_cast<std::uint32_t>(height_)))
        return false;
    if (!write_u32(0) || !write_u32(0) || !write_u32(0) || !write_u32(0)) return false;

    if (!write_raw("LIST", 4) || !write_u32(strl_data_sz) || !write_raw("strl", 4)) return false;

    if (!write_raw("strh", 4) || !write_u32(56) || !write_raw("vids", 4) || !write_raw("MJPG", 4))
        return false;
    if (!write_u32(0) || !write_u32(0) || !write_u32(0) || !write_u32(1)) return false;
    if (!write_u32(static_cast<std::uint32_t>(fps_)) || !write_u32(0)) return false;
    off_strh_length_ = bytes_;
    if (!write_u32(0) || !write_u32(0) || !write_u32(0xFFFFFFFFu) || !write_u32(0) || !write_u32(0))
        return false;
    if (!write_u32((static_cast<std::uint32_t>(width_) & 0xFFFFu) |
                   ((static_cast<std::uint32_t>(height_) & 0xFFFFu) << 16)))
        return false;

    if (!write_raw("strf", 4) || !write_u32(40) || !write_u32(40)) return false;
    if (!write_u32(static_cast<std::uint32_t>(width_)) || !write_u32(static_cast<std::uint32_t>(height_)))
        return false;
    if (!write_u32(0x00180001u) || !write_raw("MJPG", 4) || !write_u32(image_bytes)) return false;
    if (!write_u32(0) || !write_u32(0) || !write_u32(0) || !write_u32(0)) return false;

    if (!write_raw("LIST", 4)) return false;
    off_movi_size_ = bytes_;
    if (!write_u32(0) || !write_raw("movi", 4)) return false;
    off_movi_data_ = bytes_;
    return true;
}

} // namespace pm::video
