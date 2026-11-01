#pragma once

// Live BGR24 preview window for `video record --preview` (Win32).
// Client size matches the captured frame when it fits the work area.

#if defined(_WIN32)

#include <cstdint>
#include <memory>
#include <string>

namespace pm::video {

class FramePreview {
public:
    FramePreview();
    ~FramePreview();

    FramePreview(const FramePreview&) = delete;
    FramePreview& operator=(const FramePreview&) = delete;

    /// Open a window. `width`/`height` are capture pixels (client size, or
    /// scaled down to fit the work area). Title shows the capture size.
    bool show(int width, int height, const std::string& title);

    /// Thread-safe; copies the packed BGR24 frame for the UI thread.
    void update(const std::uint8_t* bgr24, int width, int height, int stride);

    bool is_open() const noexcept;
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pm::video

#endif
