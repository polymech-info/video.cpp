#include "pm/video/mjpeg_avi.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace pm::video {

MjpegAviWriter::MjpegAviWriter(const std::string& path, int width, int height, int fps)
    : width_(width), height_(height), fps_(fps > 0 ? fps : 30)
{
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
    write_headers();
}

MjpegAviWriter::~MjpegAviWriter()
{
    if (fp_) {
        std::fclose(fp_);
        fp_ = nullptr;
    }
}

void MjpegAviWriter::write_frame(const std::uint8_t* jpeg_data, std::size_t jpeg_size)
{
    const auto chunk_pos = static_cast<std::uint32_t>(
        static_cast<long>(std::ftell(fp_)) - static_cast<long>(off_movi_data_));
    std::fwrite("00dc", 4, 1, fp_);
    write_u32(static_cast<std::uint32_t>(jpeg_size));
    std::fwrite(jpeg_data, 1, jpeg_size, fp_);
    if (jpeg_size & 1)
        std::fputc(0, fp_);
    if (jpeg_size > max_frame_bytes_)
        max_frame_bytes_ = jpeg_size;
    index_.push_back({chunk_pos, static_cast<std::uint32_t>(jpeg_size)});
    ++frame_count_;
}

void MjpegAviWriter::finalize()
{
    if (!fp_)
        return;

    const long idx1_start = std::ftell(fp_);
    const auto idx_size = static_cast<std::uint32_t>(index_.size() * 16);
    std::fwrite("idx1", 4, 1, fp_);
    write_u32(idx_size);
    for (const auto& entry : index_) {
        std::fwrite("00dc", 4, 1, fp_);
        write_u32(0x10u);
        write_u32(entry.movi_offset);
        write_u32(entry.data_size);
    }

    const long file_end = std::ftell(fp_);
    patch_u32(off_riff_size_, static_cast<std::uint32_t>(file_end - 8));
    const auto movi_sz = static_cast<std::uint32_t>(
        idx1_start - static_cast<long>(off_movi_data_) + 4);
    patch_u32(off_movi_size_, movi_sz);
    patch_u32(off_avih_total_frames_, static_cast<std::uint32_t>(frame_count_));
    const auto avg_bps = static_cast<std::uint32_t>(max_frame_bytes_) * static_cast<std::uint32_t>(fps_);
    patch_u32(off_avih_max_bps_, avg_bps);
    patch_u32(off_strh_length_, static_cast<std::uint32_t>(frame_count_));

    std::fclose(fp_);
    fp_ = nullptr;
}

void MjpegAviWriter::write_u32(std::uint32_t v)
{
    std::fwrite(&v, 4, 1, fp_);
}

void MjpegAviWriter::patch_u32(long offset, std::uint32_t value)
{
    const long cur = std::ftell(fp_);
    std::fseek(fp_, offset, SEEK_SET);
    write_u32(value);
    std::fseek(fp_, cur, SEEK_SET);
}

void MjpegAviWriter::write_headers()
{
    const std::uint32_t strl_data_sz = 4 + 64 + 48;
    const std::uint32_t hdrl_data_sz = 4 + 64 + (8 + strl_data_sz);

    std::fwrite("RIFF", 4, 1, fp_);
    off_riff_size_ = std::ftell(fp_);
    write_u32(0);
    std::fwrite("AVI ", 4, 1, fp_);

    std::fwrite("LIST", 4, 1, fp_);
    write_u32(hdrl_data_sz);
    std::fwrite("hdrl", 4, 1, fp_);

    std::fwrite("avih", 4, 1, fp_);
    write_u32(56);
    write_u32(fps_ > 0 ? (1000000u / static_cast<std::uint32_t>(fps_)) : 33333u);
    off_avih_max_bps_ = std::ftell(fp_);
    write_u32(0);
    write_u32(0);
    write_u32(0x10u);
    off_avih_total_frames_ = std::ftell(fp_);
    write_u32(0);
    write_u32(0);
    write_u32(1);
    write_u32(0);
    write_u32(static_cast<std::uint32_t>(width_));
    write_u32(static_cast<std::uint32_t>(height_));
    write_u32(0);
    write_u32(0);
    write_u32(0);
    write_u32(0);

    std::fwrite("LIST", 4, 1, fp_);
    write_u32(strl_data_sz);
    std::fwrite("strl", 4, 1, fp_);

    std::fwrite("strh", 4, 1, fp_);
    write_u32(56);
    std::fwrite("vids", 4, 1, fp_);
    std::fwrite("MJPG", 4, 1, fp_);
    write_u32(0);
    write_u32(0);
    write_u32(0);
    write_u32(1);
    write_u32(static_cast<std::uint32_t>(fps_));
    write_u32(0);
    off_strh_length_ = std::ftell(fp_);
    write_u32(0);
    write_u32(0);
    write_u32(0xFFFFFFFFu);
    write_u32(0);
    write_u32(0);
    write_u32((static_cast<std::uint32_t>(width_) & 0xFFFFu) |
              ((static_cast<std::uint32_t>(height_) & 0xFFFFu) << 16));

    std::fwrite("strf", 4, 1, fp_);
    write_u32(40);
    write_u32(40);
    write_u32(static_cast<std::uint32_t>(width_));
    write_u32(static_cast<std::uint32_t>(height_));
    write_u32(0x00180001u);
    std::fwrite("MJPG", 4, 1, fp_);
    write_u32(static_cast<std::uint32_t>(width_ * height_ * 3));
    write_u32(0);
    write_u32(0);
    write_u32(0);
    write_u32(0);

    std::fwrite("LIST", 4, 1, fp_);
    off_movi_size_ = std::ftell(fp_);
    write_u32(0);
    std::fwrite("movi", 4, 1, fp_);
    off_movi_data_ = std::ftell(fp_);
}

} // namespace pm::video
