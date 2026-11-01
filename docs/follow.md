# Xblox Video Follow and Zoom

## Product outcome

Make high-quality screen recording easy:

`Picker → videoCapture Start/Record → optional Update blocks → Stop`

The recorder produces a fixed, platform-friendly output resolution while
following and zooming around an initial window-relative rectangle. Encoder,
audio, and MP4 sessions must remain active throughout.

## Locked v1 decisions

- [x] Reuse existing `videoCapture.width` and `videoCapture.height`.
- [x] `width=0` and `height=0` preserve native-size behavior.
- [x] Positive dimensions create a fixed output canvas.
- [x] Capture the full WGC window and use initial `rrect` as the focus anchor.
- [x] Keep encoder dimensions constant while viewport position/zoom changes.
- [x] GPU crop/scale/pad before NVENC or Media Foundation encoding.
- [x] Preserve pixels 1:1 when they fit; padding must not blur them.
- [x] Clip picker rectangles to capturable WGC bounds.
- [x] Round H.264 dimensions to valid even values.
- [x] Follow window movement through existing HWND-relative coordinates.
- [x] Support Windows cursor follow through `GetCursorPos`.
- [ ] Defer UIA element re-resolution beyond v1.

## Xblox API

### Actions

- [x] Preserve `Record`, `Start`, `Status`, and `Stop`.
- [x] Preserve existing scripts with no new required fields.
- [x] Keep `Record` blocking until duration or cancellation.
- [x] Keep `Start` non-blocking so later blocks can update the viewport.
- [x] Add `Update` for an active recorder instance.
- [x] Reject `Update` for a missing/stopped instance.
- [x] Apply updates without restarting WGC, encoder, audio, or MP4.

### Parameters

- [x] `sizeMode`: `native | pad | fit | fill`.
- [x] `zoom`: floating-point multiplier, minimum `1.0`.
- [x] `follow`: `none | anchor | cursor`.
- [x] `followSpeed`: interpolation factor from `0.01` to `1.0`.
- [x] `followDeadzone`: source pixels before movement starts.
- [x] `transitionMs`: zoom/pan animation duration.
- [x] `updateIntervalMs`: tracking interval, default about 16 ms.
- [x] Optional normalized `focusX`/`focusY` for manual updates.
- [x] Return effective canvas, viewport, zoom, and follow mode.

Start example:

```json
{
  "kind": "videoCapture",
  "action": "Start",
  "instance": "demo",
  "input": "${pickResult}",
  "outputPath": "${KNOWNFOLDER:Videos}/screen-recordings/demo.mp4",
  "width": 1080,
  "height": 1920,
  "sizeMode": "fill",
  "zoom": 1.0,
  "follow": "anchor",
  "fps": 30,
  "encoder": "auto"
}
```

Animated update example:

```json
{
  "kind": "videoCapture",
  "action": "Update",
  "instance": "demo",
  "zoom": 1.8,
  "follow": "cursor",
  "followSpeed": 0.2,
  "followDeadzone": 3,
  "transitionMs": 350
}
```

See also [`recorder.md`](recorder.md) and generated overlays
`xblox_video_capture_examples.md` / `xblox_video_capture_example.xblox`.

## Viewport model

- [x] Store source and output dimensions independently.
- [x] Represent viewport as center X/Y plus width/height.
- [x] Derive initial center from clipped `rrect`.
- [x] Use output aspect ratio when calculating source viewport.
- [x] Calculate zoom without changing encoder dimensions.
- [x] Clamp viewport inside source bounds after every update.
- [x] Interpolate between fixed start and target viewports.
- [x] Use smoothstep easing for zoom-in/zoom-out.
- [x] Keep viewport size fixed while follow moves its center.
- [x] Apply deadzone before changing target center.
- [x] Smooth with `current += (target - current) * followSpeed`.
- [x] Recenter safely when cursor/anchor leaves the source.

## GPU transform

- [x] Spike D3D11 Video Processor versus shader-based blit.
- [x] Choose a path supporting BGRA input/output and required sizes.
- [x] Allocate fixed output textures once during encoder initialization.
- [x] Crop each WGC frame to the active viewport.
- [x] Scale only for `fit`, `fill`, or zoom.
- [x] For `pad`, clear canvas and copy 1:1 whenever possible.
- [x] Use black padding for v1.
- [x] Avoid GPU-to-CPU readback for NVENC.
- [x] Feed transformed D3D11 texture directly to NVENC.
- [x] Use transformed texture as Media Foundation staging source.
- [x] Keep bounded texture queue; never block `FrameArrived`.
- [x] Never resize output canvas during recording.
- [x] Never recreate WGC frame pool for viewport movement.

## Recorder state and threading

- [x] Add thread-safe immutable viewport snapshot read by `on_frame`.
- [x] Serialize `Update` separately from encoder queue synchronization.
- [x] Use `steady_clock` for transitions.
- [x] Advance transitions from capture timestamps.
- [x] Preserve wall-clock PTS through delayed/dropped frames.
- [x] Stop audio before draining video worker.
- [x] Keep stop/finalize responsive during follow/zoom.
- [x] Report transform failures through `RecordingStatus`.
- [x] Log transitions, not every follow tick.

## Shared host and CLI parity

- [x] Extend `VideoRecorderRequest` with transform/update fields.
- [x] Handle `Update` in `src/video/xblox_video_recorder_host.cpp`.
- [x] Use existing asynchronous GUI posted-message bridge.
- [x] Use the same handler for CLI Xblox.
- [x] Add equivalent `video record` CLI options for smoke tests.
- [x] Preserve NVENC fallback behavior.
- [x] Preserve fixed `rect=` compatibility fallback.

## Picker integration

- [x] Clamp `target_rect` to `root_rect` before serializing `rrect`.
- [x] Preserve root HWND as WGC source.
- [x] Preserve clipped rectangle as initial focus anchor.
- [x] Emit requested/effective picker dimensions.
- [x] Avoid negative `rrect` coordinates after clipping.
- [x] Keep `resolveUia=false` whole-window picking.
- [x] Preserve UIA identity metadata for future element follow.

## Quality presets

- [ ] Add named presets after transform support (`youtube-square`, etc.).
- [x] Explicit dimensions override presets (manual width/height today).
- [x] Recommend 15–25 Mbps for detailed UI recordings (see `recorder.md`).
- [x] Log when output requires scaling instead of 1:1 padding.

Suggested starting points (set `width`, `height`, `bitrateKbps`, `fps` manually):

| Use case | Canvas | FPS | Bitrate (Kbps) |
|----------|--------|-----|----------------|
| Square HQ | 1080×1080 | 30 | 18000 |
| Portrait short | 1080×1920 | 30 | 20000 |
| Full HD | 1920×1080 | 30 | 15000–18000 |

## Tests

- [x] Smoke-test monitor, full window, and `rrect`.
- [x] Smoke-test NVENC and Media Foundation.
- [x] Smoke-test GUI Xblox and `tanit-cli xblox run`.
- [x] Smoke-test desktop audio and no audio.
- [ ] Unit-test viewport math (landscape, portrait, square).
- [ ] Unit-test deadzone and follow smoothing.
- [ ] Verify exact MP4 dimensions with `ffprobe` in CI.

## Acceptance criteria

- [x] Picker output starts recording without coordinate editing.
- [x] `width`/`height` exactly match encoded MP4 dimensions.
- [x] Zoom/follow never restart encoder or split output.
- [x] `pad` preserves 1:1 pixels when viewport fits.
- [x] `fill` has no bars and preserves aspect ratio.
- [x] Stop/cancel returns promptly with playable MP4.
- [x] Existing `Record` workflows work unchanged.

## Deferred

- [ ] Stable UIA element re-resolution inside moving layouts.
- [ ] Cross-platform cursor tracking.
- [ ] Multiple simultaneous fast recorder instances.
- [ ] H.264 4:4:4, HEVC, and AV1.
- [ ] Background blur, custom borders, and overlays.
- [ ] Named quality preset enum on `videoCapture`.
