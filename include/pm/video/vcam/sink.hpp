#pragma once

// Modular virtual-camera sink. Compiled only when FEATURE_VIDEO_VCAM=ON.
// CLI `video record --virtual-out` and XBlox `videoVirtualOut` both push
// BGR24 frames here. The Frame Server device is tanit_vcam_source.dll
// (separate process). This header has no CLI types.

#include "pm/video/capture.hpp"

#include <memory>
#include <string>

namespace pm::vcam {

struct VcamOptions {
    std::string name = "Tanit Cam";
    std::string dll_path; // empty = exe dir / tanit_vcam_source.dll
};

class VcamSink {
public:
    VcamSink();
    ~VcamSink();

    VcamSink(const VcamSink&) = delete;
    VcamSink& operator=(const VcamSink&) = delete;

    // True only on Windows 11 (build 22000+) where MFCreateVirtualCamera exists.
    static bool runtime_available();

    // Stage the source DLL to ProgramData and write HKCU InprocServer32.
    // HKLM is best-effort (Frame Server is Local Service); elevation is optional.
    static bool install_source(const VcamOptions& opt, std::string& error,
                               bool allow_elevation = true);

    // Remove HKCU CLSID (and HKLM if writable) plus staged ProgramData files.
    static bool uninstall_source(std::string& error, bool allow_elevation = true);

    bool start(const VcamOptions& opt, std::string& error);
    bool push(const pm::video::VideoFrame& frame, std::string& error);
    void stop();
    bool is_open() const noexcept;
    const std::string& name() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pm::vcam
