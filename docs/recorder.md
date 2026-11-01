# Video recorder

Record a monitor, window, or selected window region to H.264 MP4. On Windows,
the fast recorder uses Windows Graphics Capture and can keep a fixed output
size while the recorded viewport zooms or follows the pointer.

## CLI

### Record a monitor

```powershell
tanit-cli video record `
  --input "screen:0" `
  --dst recording.mp4 `
  --capture-engine auto `
  --encoder auto `
  --fps 30 `
  --duration-ms 10000
```

`screen:0` is the first monitor, `screen:1` the second, and so on. Set
`--duration-ms 0` to record until Ctrl+C or `tanit-cli video record stop`.

### Record a square viewport that follows the pointer

```powershell
tanit-cli video record `
  --input "screen:0" `
  --dst square.mp4 `
  --width 1080 `
  --height 1080 `
  --size-mode fill `
  --zoom 1.5 `
  --follow cursor `
  --follow-speed 0.2 `
  --bitrate-kbps 18000 `
  --interactive-zoom
```

The output remains 1080×1080. The source viewport moves inside the monitor;
the encoder and MP4 session are not restarted.

With `--interactive-zoom`, hold Shift and use the mouse wheel:

- wheel up zooms in;
- wheel down zooms out;
- zoom is centred near the pointer;
- the small status badge shows zoom, follow mode, and output size;
- the outlined frame shows the part of the source being recorded.

Use `--no-zoom-status` to hide the badge and frame. Use `--pass-zoom-input`
if Shift+wheel should also reach the application under the pointer. Windows
capture exclusion keeps the badge and frame out of the recording. If exclusion
is unavailable, Tanit hides those overlays rather than recording them; zoom
control continues to work.

Use `--hud-capture visible` when OBS or another screen recorder should capture
the badge and viewport frame. The Xblox equivalent is
`"hudCapture": "visible"`. This also makes the overlays visible to Tanit's own
monitor capture when they overlap its recording area.

Use `--pause-key F9` (or another shortcut such as `Ctrl+Shift+P`) to
toggle pause/resume globally while recording. The shortcut is optional and
empty by default.

### Portrait output

```powershell
tanit-cli video record `
  --input "screen:0" `
  --dst short.mp4 `
  --width 1080 `
  --height 1920 `
  --size-mode fill `
  --fps 30 `
  --bitrate-kbps 20000
```

### Record a window or picked region

Window selectors use the same input grammar as the screen picker:

```text
screen:0:hwnd=123456
screen:0:wintitle=Tanit*
screen:0:hwnd=123456:rrect=120,80,900,900
```

`rrect` is relative to the captured window. Picker rectangles are clipped to
the capturable window so negative or out-of-window pixels are not encoded.

### Audio

```powershell
tanit-cli video record `
  --input "screen:0" `
  --dst demo.mp4 `
  --audio-source desktop
```

`--audio-source` accepts `none`, `mic`, `desktop`, or `mix`. Use
`--audio-device`, `--desktop`, `--mic-gain`, and `--desktop-gain` when the
defaults are not suitable. Muxed audio is **48 kHz stereo AAC** in the MP4 —
there are no separate sample-rate flags on `video record`.

### Canvas modes

- `native`: preserve source pixels 1:1 when they fit; otherwise scale down.
- `pad`: preserve source aspect and add black padding.
- `fit`: show the complete viewport and scale it to fit the canvas.
- `fill`: cover the canvas; crop source edges when aspect ratios differ.

Both `--width` and `--height` must be set for a fixed canvas. Leaving both at
zero records at the native window, region, or monitor size.

### Capture and encoder selection

- `--capture-engine auto`: prefer fast WGC and fall back when possible.
- `--capture-engine fast`: require WGC (zoom, follow, `bitrate-kbps`).
- `--capture-engine compat`: use the portable frame-pump recorder (no zoom/follow/bitrate).
- `--encoder auto`: try the optional NVIDIA encoder, then Media Foundation.
- `--encoder nvenc`: require NVIDIA NVENC.
- `--encoder mediaFoundation`: use the Windows encoder.

| Engine | Bitrate | Zoom / follow / fixed canvas |
|--------|---------|------------------------------|
| `fast` | ✅ (`0` = auto) | ✅ |
| `compat` | ❌ ignored | ❌ |

Zoom, follow, fixed-canvas transforms, and interactive zoom require the fast
WGC path. The CLI logs the effective backend, encoder, canvas, bitrate, frame
count, dropped frames, and finalization errors.

## Xblox

Use `videoCapture` with a picker result or a direct input such as `screen:0`.

### Record until duration or cancellation

```json
{
  "kind": "videoCapture",
  "action": "Record",
  "input": "screen:0",
  "outputPath": "${KNOWNFOLDER:Videos}/screen-recordings/demo.mp4",
  "width": 1080,
  "height": 1080,
  "sizeMode": "fill",
  "zoom": 1.5,
  "follow": "cursor",
  "followSpeed": 0.2,
  "followDeadzone": 3,
  "interactiveZoom": true,
  "pauseKey": "F9",
  "showZoomStatus": true,
  "fps": 30,
  "bitrateKbps": 18000,
  "captureEngine": "auto",
  "encoder": "auto",
  "audioSource": "desktop",
  "durationMs": 10000
}
```

`durationMs: 0` means record until the Xblox run is stopped or cancelled.

When using a direct `input`, remove any old `bindings.input` that points to a
disabled picker. A binding takes precedence over the literal field.

### Start, update, and stop

Use `Start` when later blocks need to change the active viewport:

```json
{
  "kind": "videoCapture",
  "action": "Start",
  "instance": "demo",
  "input": "screen:0",
  "outputPath": "${KNOWNFOLDER:Videos}/screen-recordings/demo.mp4",
  "width": 1080,
  "height": 1920,
  "sizeMode": "fill",
  "zoom": 1.0,
  "follow": "anchor",
  "fps": 30
}
```

Update it without restarting capture, audio, encoding, or MP4:

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

Query or finish the same instance:

```json
{ "kind": "videoCapture", "action": "Status", "instance": "demo" }
```

```json
{ "kind": "videoCapture", "action": "Stop", "instance": "demo" }
```

`Status`, `Update`, and `Stop` must use the same `instance` as `Start`.

## Quality guidance

- Use `pad` when avoiding resampling matters more than filling the canvas.
- Use `fill` for a moving square or portrait frame.
- Use 15–25 Mbps for detailed UI recordings uploaded to social platforms.
- Upload platforms re-encode video; local 1:1 playback is the best source
  quality check.
- NVENC reduces CPU work but does not prevent platform-side recompression.

## Troubleshooting

- Recorder ends immediately: check for a stale picker binding or cancellation.
- `Record` does not end: set `durationMs`, press Ctrl+C, or stop the Xblox run.
- `Update` fails: use `Start`, a stable `instance`, and the fast WGC backend.
- No Shift+wheel response: enable `interactiveZoom`/`--interactive-zoom`.
- Frame does not look square: set both dimensions and use `sizeMode: fill`.
- Fast capture unavailable: use `auto` for fallback or inspect trace logs when
  `fast`/`nvenc` is required.
