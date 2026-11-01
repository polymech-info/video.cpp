# Follow-ups

xblox stays a caller. `xblox_video_recorder_host.cpp` and the video blocks include the package headers and link `pm-video-lib`.

ByteTrack is `src/detect/bytetrack.cpp`. It is not compiled: this tree builds with OpenCV off, and the tracker also wants ncnn. Turn it on only when both targets exist.

Screen and window capture, the Windows frame pump, the Media Foundation file encoder, the record-session file, and the WGC session recorder compile into `pm-video-lib` when `PM_VIDEO_RECORD_ENGINES` is on. The host sets that before `add_subdirectory`. `PM_VIDEO_HOST_SINGLETONS` and `PM_AUDIO_HOST_SINGLETONS` keep `hooks.cpp`, `captions.cpp`, and `mixed_capture.cpp` in `pm-media` so the executables share one hook table. The `video record` command loop stays in `src/cli/pm_image_cmd_video.cpp`. Whisper and proxy speech stay on the host; caption sidecars call `set_caption_transcribe`.
