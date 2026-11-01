# pm-video architecture

`pm-video-lib` is the portable video library. Screen capture, the file encoder, and the session recorder live in the library when `PM_VIDEO_RECORD_ENGINES` is on. Mixed audio is `pm-audio`. Speech stays on the host. Each diagram is an interactive HTML file next to the JSON it is built from.

| Diagram | What it shows |
|---------|----------------|
| [overview.html](overview.html) | Host, hooks, and the library modules |
| [sources.html](sources.html) | Webcam capture and the virtual camera |
| [filters.html](filters.html) | Face mesh, skin, and background |
| [detect.html](detect.html) | YOLO ONNX decoders |

## Overview

`set_exe_dir`, `set_models_dir`, `set_log`, and `set_parse_shortcut` come from the host and land in one hook table inside `pm-media`. Inside the library: webcam and screen sources, the file encoder, the WGC session recorder, the beauty filters, YOLO decoding, the NVENC plugin, a WebRTC signaling scaffold, and the caption sidecar hook. The virtual camera is a second process (`tanit_vcam_source.dll`) reading frames this process published.

## Sources

`VideoInput` enumerates devices and modes, then streams on its own thread. The callback must not block. Screen and window capture is the same library. `VcamSink` converts BGR24 to NV12 and publishes one of two slots in `%ProgramData%\Pixlwiz\vcam.frames`. The Frame Server DLL reads the published slot. `ready` stays set after the first good frame.

## Filters

`BeautyConfig` decides the work. `needs_mesh()` loads the MediaPipe face landmarker and runs skin, lips, teeth, and grade. `needs_seg()` loads the image segmenter and runs blur or replacement. Skin smoothing and background blur use the D3D11 pass when it is available, and the CPU path otherwise. The processor rejects anything that is not packed BGR24.

## Detect

Decoders live in `media::vdetect`. An ONNX session from `src/lib/onnx` produces tensors, the letterbox is undone, and the head fills a `DecodeResult` in original image pixels. Detect, oriented boxes, and pose then go through NMS. Classify does not. Segment and semantic are named and not implemented. `src/detect/bytetrack.cpp` is not compiled.

## Left off these diagrams

Pause bits are a small mask the recorder checks before it writes a frame. The record command itself stays in the host CLI.
