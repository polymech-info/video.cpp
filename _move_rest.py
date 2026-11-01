import shutil
from pathlib import Path

root = Path(r"c:\Users\zx\Desktop\pixlwiz\pixlwiz")
pkg = root / "packages" / "video.cpp"

copies = {
    "src/core/video.cpp": "src/capture_win.cpp",
    "src/core/video_linux.cpp": "src/capture_linux.cpp",
    "src/core/video_osx.mm": "src/capture_osx.mm",
    "src/video/vcam/vcam_sink.cpp": "src/vcam/sink.cpp",
    "src/video/vcam/vcam_sink.hpp": "include/pm/video/vcam/sink.hpp",
    "src/video/vcam/source/vcam_source.cpp": "src/vcam/source.cpp",
    "src/video/vcam/source/vcam_source.def": "src/vcam/source.def",
    "src/video/frame_preview.cpp": "src/preview.cpp",
    "src/video/frame_preview.hpp": "include/pm/video/preview.hpp",
    "src/video/capture_idle_detector.cpp": "src/idle.cpp",
    "src/video/capture_idle_detector.hpp": "include/pm/video/idle.hpp",
    "src/video/win_interactive_zoom_controller.cpp": "src/zoom.cpp",
    "src/video/win_interactive_zoom_controller.hpp": "include/pm/video/zoom.hpp",
    "src/video/win_camera_overlay_controller.cpp": "src/overlay.cpp",
    "src/video/win_camera_overlay_controller.hpp": "include/pm/video/overlay.hpp",
    "src/video/key_overlay.hpp": "include/pm/video/key_overlay.hpp",
    "src/video/creator_fx.hpp": "include/pm/video/creator_fx.hpp",
    "src/video/filters/face_effects.cpp": "src/filters/face_effects.cpp",
    "src/video/filters/face_effects.hpp": "include/pm/video/filters/face_effects.hpp",
    "src/video/filters/bg_effects.cpp": "src/filters/bg_effects.cpp",
    "src/video/filters/bg_effects.hpp": "include/pm/video/filters/bg_effects.hpp",
    "src/video/filters/beauty_processor.cpp": "src/filters/beauty_processor.cpp",
    "src/video/filters/beauty_processor.hpp": "include/pm/video/filters/beauty_processor.hpp",
    "src/video/filters/face_landmarker.cpp": "src/filters/face_landmarker.cpp",
    "src/video/filters/face_landmarker.hpp": "include/pm/video/filters/face_landmarker.hpp",
    "src/video/filters/image_segmenter.cpp": "src/filters/image_segmenter.cpp",
    "src/video/filters/image_segmenter.hpp": "include/pm/video/filters/image_segmenter.hpp",
    "src/video/filters/skin_gpu.cpp": "src/filters/skin_gpu.cpp",
    "src/video/filters/skin_gpu.hpp": "include/pm/video/filters/skin_gpu.hpp",
    "src/video/detect/boxes.cpp": "src/detect/boxes.cpp",
    "src/video/detect/boxes.hpp": "include/pm/video/detect/boxes.hpp",
    "src/video/detect/cls.cpp": "src/detect/cls.cpp",
    "src/video/detect/cls.hpp": "include/pm/video/detect/cls.hpp",
    "src/video/detect/obb.cpp": "src/detect/obb.cpp",
    "src/video/detect/obb.hpp": "include/pm/video/detect/obb.hpp",
    "src/video/detect/pose.cpp": "src/detect/pose.cpp",
    "src/video/detect/pose.hpp": "include/pm/video/detect/pose.hpp",
    "src/video/detect/seg.cpp": "src/detect/seg.cpp",
    "src/video/detect/seg.hpp": "include/pm/video/detect/seg.hpp",
    "src/video/detect/render.cpp": "src/detect/render.cpp",
    "src/video/detect/render.hpp": "include/pm/video/detect/render.hpp",
    "src/video/detect/serialize.cpp": "src/detect/serialize.cpp",
    "src/video/detect/serialize.hpp": "include/pm/video/detect/serialize.hpp",
    "src/video/detect/commons.cpp": "src/detect/commons_onnx.cpp",
    "src/video/detect/bytetrack.cpp": "src/detect/bytetrack.cpp",
    "src/video/nvenc_driver_api_win.cpp": "src/nvenc/driver.cpp",
    "src/session/recorder/pm_nvenc_plugin.cpp": "src/nvenc/plugin.cpp",
    "src/session/recorder/pm_nvenc_plugin_api.hpp": "include/pm/video/nvenc_api.hpp",
    "src/session/recorder/pm_nvenc_loader.cpp": "src/nvenc/loader.cpp",
    "src/session/recorder/pm_nvenc_loader.hpp": "include/pm/video/nvenc_loader.hpp",
    "src/win/dpi_thread.hpp": "include/pm/video/win_dpi.hpp",
}

repls = [
    ('#include "core/video.hpp"', '#include "pm/video/capture.hpp"'),
    ('#include "core/frame_pump.hpp"\n', ''),
    ('#include "core/cli_cancel.hpp"\n', ''),
    ('#include "core/app_exe_directory.hpp"\n', '#include "pm/video/hooks.hpp"\n'),
    ('#include "core/library_utils.hpp"\n', '#include "pm/video/dynlib.hpp"\n'),
    ('#include "core/settings_store.hpp"\n', ''),
    ('#include "core/hotkeys.hpp"\n', '#include "pm/video/hooks.hpp"\n'),
    ('#include "win/dpi_thread.hpp"', '#include "pm/video/win_dpi.hpp"'),
    ('#include "logger/logger.h"\n', '#include "pm/video/hooks.hpp"\n'),
    ('#include "video/detect/types.hpp"', '#include "pm/video/detect/types.hpp"'),
    ('#include "video/detect/commons.hpp"', '#include "pm/video/detect/commons.hpp"'),
    ('#include "video/detect/boxes.hpp"', '#include "pm/video/detect/boxes.hpp"'),
    ('#include "video/detect/cls.hpp"', '#include "pm/video/detect/cls.hpp"'),
    ('#include "video/detect/obb.hpp"', '#include "pm/video/detect/obb.hpp"'),
    ('#include "video/detect/pose.hpp"', '#include "pm/video/detect/pose.hpp"'),
    ('#include "video/detect/seg.hpp"', '#include "pm/video/detect/seg.hpp"'),
    ('#include "video/detect/render.hpp"', '#include "pm/video/detect/render.hpp"'),
    ('#include "video/detect/serialize.hpp"', '#include "pm/video/detect/serialize.hpp"'),
    ('#include "video/filters/beauty_config.hpp"', '#include "pm/video/filters/beauty_config.hpp"'),
    ('#include "beauty_processor.hpp"', '#include "pm/video/filters/beauty_processor.hpp"'),
    ('#include "face_effects.hpp"', '#include "pm/video/filters/face_effects.hpp"'),
    ('#include "bg_effects.hpp"', '#include "pm/video/filters/bg_effects.hpp"'),
    ('#include "image_ops.hpp"', '#include "pm/video/filters/image_ops.hpp"'),
    ('#include "landmarks.hpp"', '#include "pm/video/filters/landmarks.hpp"'),
    ('#include "skin_gpu.hpp"', '#include "pm/video/filters/skin_gpu.hpp"'),
    ('#include "face_landmarker.hpp"', '#include "pm/video/filters/face_landmarker.hpp"'),
    ('#include "image_segmenter.hpp"', '#include "pm/video/filters/image_segmenter.hpp"'),
    ('#include "beauty_config.hpp"', '#include "pm/video/filters/beauty_config.hpp"'),
    ('#include "vcam_sink.hpp"', '#include "pm/video/vcam/sink.hpp"'),
    ('#include "vcam_convert.hpp"\n', ''),
    ('#include "vcam_protocol.hpp"', '#include "pm/video/vcam/protocol.hpp"'),
    ('#include "../vcam_protocol.hpp"', '#include "pm/video/vcam/protocol.hpp"'),
    ('#include "frame_preview.hpp"', '#include "pm/video/preview.hpp"'),
    ('#include "capture_idle_detector.hpp"', '#include "pm/video/idle.hpp"'),
    ('#include "win_interactive_zoom_controller.hpp"', '#include "pm/video/zoom.hpp"'),
    ('#include "win_camera_overlay_controller.hpp"', '#include "pm/video/overlay.hpp"'),
    ('#include "key_overlay.hpp"', '#include "pm/video/key_overlay.hpp"'),
    ('#include "creator_fx.hpp"', '#include "pm/video/creator_fx.hpp"'),
    ('#include "pause_reason.hpp"', '#include "pm/video/pause.hpp"'),
    ('#include "pm_nvenc_loader.hpp"', '#include "pm/video/nvenc_loader.hpp"'),
    ('#include "pm_nvenc_plugin_api.hpp"', '#include "pm/video/nvenc_api.hpp"'),
    ('media::app::exe_parent_directory()', 'pm::video::exe_dir()'),
    ('media::app::exe_cli_executable()', 'pm::video::cli_path()'),
    ('media::app::exe_executable()', 'pm::video::exe_path()'),
    ('media::settings::get_embedding_models_dir()', 'pm::video::embedding_models_dir()'),
    ('media::settings::get_models_dir()', 'pm::video::models_dir()'),
    ('media::library::', 'pm::video::dlib::'),
    ('logger::info(', 'pm::video::log_info('),
    ('logger::warn(', 'pm::video::log_warn('),
    ('logger::error(', 'pm::video::log_error('),
    ('media::hotkeys::format_key_display(', 'pm::video::key_label('),
]

pumps = [
    '''std::unique_ptr<IFramePump> make_frame_pump(const std::string& /*spec*/,
                                            const FramePumpOptions& /*opts*/,
                                            std::string& err) {
    err = "make_frame_pump: not implemented on linux yet";
    return nullptr;
}

''',
    '''std::unique_ptr<IFramePump> make_frame_pump(const std::string& /*spec*/,
                                            const FramePumpOptions& /*opts*/,
                                            std::string& err) {
    err = "make_frame_pump: not implemented on macOS yet";
    return nullptr;
}

''',
]

for src, dst in copies.items():
    target = pkg / dst
    target.parent.mkdir(parents=True, exist_ok=True)
    text = (root / src).read_text(encoding="utf-8")
    for a, b in repls:
        text = text.replace(a, b)
    for pump in pumps:
        text = text.replace(pump, "")
    target.write_text(text, encoding="utf-8")
    print("copied", dst)

video_h = (root / "src/core/video.hpp").read_text(encoding="utf-8")
body = video_h
body = body.replace("#if defined(FEATURE_VIDEO) && FEATURE_VIDEO\n\n", "")
body = body.replace("\n#endif // FEATURE_VIDEO\n", "\n")
(pkg / "include/pm/video/capture.hpp").write_text(body, encoding="utf-8")
print("capture.hpp")

forwards = {
    "src/core/video.hpp": "pm/video/capture.hpp",
    "src/video/vcam/vcam_sink.hpp": "pm/video/vcam/sink.hpp",
    "src/video/frame_preview.hpp": "pm/video/preview.hpp",
    "src/video/capture_idle_detector.hpp": "pm/video/idle.hpp",
    "src/video/win_interactive_zoom_controller.hpp": "pm/video/zoom.hpp",
    "src/video/win_camera_overlay_controller.hpp": "pm/video/overlay.hpp",
    "src/video/key_overlay.hpp": "pm/video/key_overlay.hpp",
    "src/video/creator_fx.hpp": "pm/video/creator_fx.hpp",
    "src/video/filters/face_effects.hpp": "pm/video/filters/face_effects.hpp",
    "src/video/filters/bg_effects.hpp": "pm/video/filters/bg_effects.hpp",
    "src/video/filters/beauty_processor.hpp": "pm/video/filters/beauty_processor.hpp",
    "src/video/filters/face_landmarker.hpp": "pm/video/filters/face_landmarker.hpp",
    "src/video/filters/image_segmenter.hpp": "pm/video/filters/image_segmenter.hpp",
    "src/video/filters/skin_gpu.hpp": "pm/video/filters/skin_gpu.hpp",
    "src/video/detect/boxes.hpp": "pm/video/detect/boxes.hpp",
    "src/video/detect/cls.hpp": "pm/video/detect/cls.hpp",
    "src/video/detect/obb.hpp": "pm/video/detect/obb.hpp",
    "src/video/detect/pose.hpp": "pm/video/detect/pose.hpp",
    "src/video/detect/seg.hpp": "pm/video/detect/seg.hpp",
    "src/video/detect/render.hpp": "pm/video/detect/render.hpp",
    "src/video/detect/serialize.hpp": "pm/video/detect/serialize.hpp",
    "src/session/recorder/pm_nvenc_plugin_api.hpp": "pm/video/nvenc_api.hpp",
    "src/session/recorder/pm_nvenc_loader.hpp": "pm/video/nvenc_loader.hpp",
}
guard = {
    "src/core/video.hpp": "FEATURE_VIDEO",
}
for path, inc in forwards.items():
    dest = root / path
    if path in guard:
        text = (
            "#pragma once\n\n"
            "#if defined(FEATURE_VIDEO) && FEATURE_VIDEO\n"
            f'#include "{inc}"\n'
            "#endif\n"
        )
    else:
        text = f'#pragma once\n#include "{inc}"\n'
    dest.write_text(text, encoding="utf-8")
    print("forward", path)

delete_cpp = [
    "src/core/video.cpp",
    "src/video/vcam/vcam_sink.cpp",
    "src/video/vcam/source/vcam_source.cpp",
    "src/video/frame_preview.cpp",
    "src/video/capture_idle_detector.cpp",
    "src/video/win_interactive_zoom_controller.cpp",
    "src/video/win_camera_overlay_controller.cpp",
    "src/video/filters/face_effects.cpp",
    "src/video/filters/bg_effects.cpp",
    "src/video/filters/beauty_processor.cpp",
    "src/video/filters/face_landmarker.cpp",
    "src/video/filters/image_segmenter.cpp",
    "src/video/filters/skin_gpu.cpp",
    "src/video/detect/boxes.cpp",
    "src/video/detect/cls.cpp",
    "src/video/detect/obb.cpp",
    "src/video/detect/pose.cpp",
    "src/video/detect/seg.cpp",
    "src/video/detect/render.cpp",
    "src/video/detect/serialize.cpp",
    "src/video/detect/commons.cpp",
    "src/video/nvenc_driver_api_win.cpp",
    "src/session/recorder/pm_nvenc_plugin.cpp",
    "src/session/recorder/pm_nvenc_loader.cpp",
    "src/video/caption_sidecar.cpp",
    "src/video/detect/bytetrack.cpp",
]
for rel in delete_cpp:
    p = root / rel
    if p.exists():
        p.unlink()
        print("deleted", rel)

linux_stub = r'''#if defined(FEATURE_VIDEO) && FEATURE_VIDEO
#include "core/frame_pump.hpp"

namespace pm::video {

std::unique_ptr<IFramePump> make_frame_pump(const std::string& /*spec*/,
                                            const FramePumpOptions& /*opts*/,
                                            std::string& err) {
    err = "make_frame_pump: not implemented on linux yet";
    return nullptr;
}

} // namespace pm::video
#endif
'''
osx_stub = linux_stub.replace("linux yet", "macOS yet")
(root / "src/core/video_linux.cpp").write_text(linux_stub, encoding="utf-8")
(root / "src/core/video_osx.mm").write_text(osx_stub, encoding="utf-8")
print("stubs")

cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
drop = [
    "src/core/video.cpp",
    "src/video/vcam/vcam_sink.cpp",
    "src/video/frame_preview.cpp",
    "src/video/detect/commons.cpp",
    "src/video/detect/boxes.cpp",
    "src/video/detect/cls.cpp",
    "src/video/detect/obb.cpp",
    "src/video/detect/pose.cpp",
    "src/video/detect/seg.cpp",
    "src/video/detect/serialize.cpp",
    "src/video/detect/render.cpp",
    "src/video/filters/face_landmarker.cpp",
    "src/video/filters/image_segmenter.cpp",
    "src/video/filters/beauty_processor.cpp",
    "src/video/filters/face_effects.cpp",
    "src/video/filters/bg_effects.cpp",
    "src/video/filters/skin_gpu.cpp",
    "src/video/win_interactive_zoom_controller.cpp",
    "src/video/win_camera_overlay_controller.cpp",
    "src/video/capture_idle_detector.cpp",
    "src/video/caption_sidecar.cpp",
    "src/session/recorder/pm_nvenc_loader.cpp",
]
out_lines = []
for line in cmake.splitlines(keepends=True):
    if any(p in line for p in drop):
        print("cmake drop", line.strip())
        continue
    line = line.replace(
        "src/video/vcam/source/vcam_source.cpp",
        "packages/video.cpp/src/vcam/source.cpp",
    )
    line = line.replace(
        "src/video/vcam/source/vcam_source.def",
        "packages/video.cpp/src/vcam/source.def",
    )
    line = line.replace(
        "src/video/nvenc_driver_api_win.cpp",
        "packages/video.cpp/src/nvenc/driver.cpp",
    )
    line = line.replace(
        "src/session/recorder/pm_nvenc_plugin.cpp",
        "packages/video.cpp/src/nvenc/plugin.cpp",
    )
    out_lines.append(line)
(root / "CMakeLists.txt").write_text("".join(out_lines), encoding="utf-8")
print("cmake updated")
