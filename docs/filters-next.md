# Video filters next — face beauty first, then live / virtual cam

Frame pipe: **source → filters → sink**. Same `BeautyProcessor` for stills,
file clips, and `video record`. **`video filter` is the test harness**
(stills / offline clips). **`video record` is the product path.** The
virtual-cam sink (`pm::vcam::VcamSink`) is shared: CLI `--virtual-out` and
XBlox `videoVirtualOut` both push BGR24 into it. Beauty XBlox blocks stay
after the drop-queue / perf gate.

**Primary inference:** MediaPipe **Tasks** `FaceLandmarker` (468-pt mesh,
video / live-stream mode, tracking). That is the face pipeline. Do **not**
embed SegmeCam’s Bazel `CalculatorGraph` / EGL GPU graphs.

MediaPipe runs **in-process**. Bazel produces one prebuilt Windows DLL;
pixlwiz soft-probes and loads it through `media::library`
(`src/core/library_utils.hpp`). There is no hard link, helper process, IPC,
service, or runtime “sidecar”.

**Filters:** own pixel ops on `VideoFrame` BGR24 — same style as
`videoGrayscale` / `videoColor` (no OpenCV). Copy SegmeCam skin / lip / teeth
/ feather math into in-tree functions where needed. Do not take a
`FEATURE_OPENCV` dependency for this.

Related: [video-next.md](./video-next.md) (PiP `blurBackground` stays radar
until this pipe is cheap). MediaPipe CalculatorGraph → XBlox named
pipes is parked in
[filter-graphs-mediapipe.md](./filter-graphs-mediapipe.md) — do not
embed `.pbtxt` / calculators.

---

## Slices

| Slice | What | When |
|-------|------|------|
| **-1** | Prove and package the in-process MediaPipe DLL bridge | Before pixlwiz edits |
| **0** | Flags + CLI stub + reserved options | First |
| **1 — MVP** | Offline test CLI (`video filter`) + same processor on `video record` | **Shipped** (record flags on; RT later) |
| **2** | Realtime preview / drop-queue / perf gate | After record looks right |
| **3** | Virtual cam — MF Frame Server, frictionless | **Shipped** (session + CurrentUser; signing later) |
| **4** | XBlox filter blocks | After 2; `videoVirtualOut` already uses the same sink |

Background blur / replace is **not** the MVP. Add `ImageSegmenter` (or a
copied blur) only after face beauty looks right. Store / MSIX: keep flags
**OFF**. Desktop MSI/EXE only for slice 3.

---

## Feature flags

```
FEATURE_VIDEO_BEAUTY
  requires FEATURE_VIDEO + FEATURE_MEDIAPIPE_TASKS

FEATURE_VIDEO_VCAM
  requires FEATURE_VIDEO, Win32, not Store
  MF Frame Server source DLL + VcamSink (CLI and XBlox)

FEATURE_MEDIAPIPE_TASKS
  prebuilt Tasks C++ (FaceLandmarker, later ImageSegmenter)
  Bazel produces an in-process prebuilt — not part of the pixlwiz CMake graph
```

`FEATURE_OPENCV` / `FEATURE_ONNX` are **not** required. ONNX stays available
for YOLO; do not route face mesh through it.

Tasks packaging/loading rules:

- A pinned full upstream MediaPipe checkout is built by a separate PowerShell
  command / CI job. The sparse SegmeCam checkout is reference material, not a
  sufficient build root
- Prefer one `/MD` Release DLL; do not export MediaPipe, protobuf, or Abseil
  C++ types to pixlwiz. An import `.lib` may be staged for diagnostics but the
  app does not link it
- Export a small C ABI (`create`, `detect`, `destroy`) from the DLL. Its
  implementation calls Tasks `Detect` / `DetectForVideo`
- Windows: CPU / XNNPACK (`MEDIAPIPE_DISABLE_GPU`). Do not bring EGL graphs
- No `obs.h`, no libobs, no SegmeCam binary

---

## Non-invasive architecture

Only the CLI registration and feature-gated CMake branches touch existing
code. Decode, capture, frame storage, and encoding remain unchanged.

```
IFramePump (existing)
  -> VideoFrame BGR24 (existing)
  -> BeautyProcessor (new)
       -> BGR24 -> RGB scratch
       -> pixlwiz_face_landmarker.dll C ABI
       -> internal LandmarkSet (468, or 478 with iris)
       -> face regions + pixel effects
  -> VideoEncoder / save_frame (existing)
  -> VcamSink (optional, FEATURE_VIDEO_VCAM)
       -> scale 1280×720 → NV12 → shared memory
       -> tanit_vcam_source.dll (IMFMediaSource for Frame Server)
```

### Dependency boundary

The DLL owns all MediaPipe objects and allocations. Pixlwiz resolves the
exports with `media::library::load` / `sym` and sees only POD:

```c
typedef struct pm_face_landmarker pm_face_landmarker;
typedef struct pm_face_landmark { float x, y, z; } pm_face_landmark;
typedef struct pm_face_result {
  uint32_t face_count;
  uint32_t landmarks_per_face;
  const pm_face_landmark* landmarks; /* valid until next detect/destroy */
} pm_face_result;

uint32_t pm_face_abi_version(void);
int pm_face_create(const char* model_utf8, int running_mode,
                   pm_face_landmarker** out, char* error, size_t error_cap);
int pm_face_detect_rgb(pm_face_landmarker*, const uint8_t* rgb,
                       int width, int height, int stride,
                       int64_t timestamp_ms, pm_face_result* out,
                       char* error, size_t error_cap);
void pm_face_destroy(pm_face_landmarker*);
```

Rules:

- C ABI is versioned (`PM_FACE_ABI_VERSION`) and checked immediately after
  `load`, before `create`
- No exceptions, STL containers, strings, protobufs, or Abseil cross the DLL
  boundary
- Caller owns RGB input; DLL owns result storage
- DLL copies/wraps input only for the duration of `detect`
- IMAGE accepts timestamp `0`; VIDEO requires strictly increasing ms
- One handle is used by one processing thread
- RAII unload order is strict: destroy every landmarker handle before
  `media::library::unload`
- MVP is Win32-only because the current `library_utils` POSIX implementation
  is a stub

This avoids Bazel/CMake C++ ABI coupling, calculator-registration loss from
partial static archives, and CRT ownership bugs.

### Files (shipped)

```
packages/mediapipe-tasks/
  build-win.ps1
  UPSTREAM.txt
  README-PREBUILT.md
  copy-if-exists.cmake
  bridge/BUILD
  bridge/pixlwiz_face_landmarker.h
  bridge/pixlwiz_face_landmarker.cpp
  prebuilt/win-x64/{bin,include,models}/

src/video/filters/
  beauty_config.hpp
  beauty_processor.hpp/.cpp
  face_landmarker.hpp/.cpp       # library_utils + C-ABI adapter; no MP headers
  landmarks.hpp                  # points + face/lip/eye index arrays
  face_effects.hpp/.cpp          # lips / teeth / skin on BGR24
  image_ops.hpp/.cpp             # masks / blur / morphology / Lab

src/cli/pm_image_cmd_video_filter.cpp
tests/video-filters/             # fixtures + out/
tests/orchestrator/test-video-filter-basics.mjs
```

All new filter runtime sources live under `src/video/filters/`.
`face_effects.*` and `image_ops.*` must not include MediaPipe or OpenCV
headers. Only the Bazel-built bridge includes MediaPipe.

---

## TODO -1 — build/integration spike (before CLI surface)

Do this first; it answers the highest-risk question without touching the
working video path.

- [x] Pin a full upstream MediaPipe commit and Bazelisk version in
      `packages/mediapipe-tasks/UPSTREAM.txt` (`v1.0.0` / Bazel 7.4.1 / Bazelisk v1.29.0)
- [x] Prove the upstream Windows CPU build:

```
bazel build -c opt --define MEDIAPIPE_DISABLE_GPU=1 \
  //mediapipe/tasks/cc/vision/face_landmarker:face_landmarker \
  //pixlwiz_bridge:pixlwiz_face_landmarker
```

- [x] Add the bridge target as `cc_binary(linkshared = True)` with a `.def`
      file exporting only `pm_face_*`
- [x] Stage script copies DLL, public C header, `face_landmarker_v2.task`, PDB,
      optional import lib, and a `STAMP.txt` (run `packages/mediapipe-tasks/build-win.ps1`)
- [x] Add a standalone bridge smoke executable **inside the package**, not
      pixlwiz: dynamically load DLL -> synthetic RGB frame -> detect -> print
      face / landmark count
- [x] Verify `/MD`, x64 Release, `MEDIAPIPE_DISABLE_GPU=1`. ImageToTensor
      needs official OpenCV 4.11 `opencv_world4110.dll` beside the bridge
      (empty stub is not enough). dumpbin may list that world DLL only
- [x] `STAMP.txt`: `dll_bytes=8889344` (~8.5 MB), `smoke_ms=130`, ABI 1/1,
      blank frame 0 faces

Stop here if the one-DLL bridge cannot build or its deployment dependency set
is not self-contained. Do not compensate by linking Bazel’s transitive static
library forest into pixlwiz.

Acceptance:

- The package smoke tool runs beside the staged DLL and model on a clean
  Windows machine / VM
- A face image returns 468 landmarks (478 if the v2 task includes iris);
  a blank image returns zero faces
- Missing/invalid model produces a bounded UTF-8 error, not an exception

---

## TODO 0 — Flags + CLI stub + reserved options

- [x] `FEATURE_MEDIAPIPE_TASKS` + `FEATURE_VIDEO_BEAUTY` in `CMakeLists.txt`
      and `CMakePresets.json` `pm.features`
- [x] Compile defs on `pm-media` / `pm-image` / `pm-image-cli`
- [x] `FEATURE_VIDEO_BEAUTY=ON` requires `FEATURE_VIDEO`, but does **not**
      require the bridge DLL or model at configure/build time
- [x] `FaceLandmarkerLibrary` uses `media::library::probe`, `load`, `sym`, and
      `unload`; resolve every required export into typed function pointers
- [x] Runtime search order: explicit CLI DLL path (diagnostic override), app
      directory, packaged dependency directory, then OS loader paths
- [x] Missing DLL, missing export, wrong ABI, and missing model are separate
      capability states with actionable errors
- [x] POST_BUILD copy bridge DLL to `${PM_DIST_DIR}` and model to
      `${PM_DIST_DIR}/../models/vision/face_landmarker_v2.task` **when the
      prebuilt files exist**; absence is a status message, not CMake failure
- [x] CLI: `pm-image video filter` — parse args; slice 1 writes stills/clips
- [x] Flags (stable names; `--blur` / `--background` still reserved):

```
--face                  bool    FaceLandmarker (default on for this command)
--skin                  bool / strength
--lips                  bool / color / alpha
--teeth                 bool / strength
--model-face            path    FaceLandmarker .task / bundle (empty = bundled)
--running-mode          image | video     (video = DetectForVideo timestamps)

--mirror                bool    Horizontal flip (no model)
--blur / --background / --blur-strength / --feather-px
--bg-color / --bg-image
--model-seg             path    ImageSegmenter selfie_segmenter.task
```

No xblox params in this slice.

Exact existing-file edits:

- `CMakeLists.txt`: feature validation, feature-gated sources, optional dist
  copy; no imported target and no edits to `FEATURE_OPENCV` or ONNX branches
- `CMakePresets.json`: descriptions + ON in `base` / `release-debug`; explicitly OFF in Store / viewer
- `src/cli/pm_image_cli_state.hpp`: append filter state at the documented
  append-only tail; do not insert into existing ABI-sensitive data
- `src/cli/pm_image_cmd_video.hpp`: feature-gated declaration
- `src/cli/pm_image_cmd_video.cpp`: register + dispatch only
- Add `src/cli/pm_image_cmd_video_filter.cpp`: all filter command runtime

Do not edit `video_source_win32.cpp`, `video_encoder.cpp`,
`video_filter_blocks.cpp`, or `WinCameraOverlayController` in slices -1/0/1.

---

## TODO 1 — MVP: offline face beauty (shipped)

`video filter` is the still/clip test harness. Same flags and processor run
on `video record` (CPU frame pump, `.mp4`). No preview window, no virtual
device, no OpenCV. Realtime is slice 2.

```
pm-image video filter --input talk.mp4 --dst talk-out.mp4 --face --skin --lips --teeth
pm-image video filter --input face.jpg --dst out.jpg --face --skin
pm-image video record --dst out.mp4 --lips --teeth
```

- [x] Soft-load the in-process MediaPipe bridge prebuilt for Windows CPU
- [x] Bundle the official Face Landmarker task file (Apache-2.0)
- [x] `pm::video::BeautyProcessor`: `VideoFrame` BGR24 in → BGR24 out
- [x] In the bridge: map RGB → `mediapipe::Image`, call `Detect` (stills)
      or `DetectForVideo` (clips)
- [x] In pixlwiz: convert bridge output once to internal `LandmarkSet`; all
      downstream code is MediaPipe-free
- [x] Reimplement the useful SegmeCam face-effect math on packed BGR24:
      - [x] Skin smooth (face-oval ROI, half-res box blur + upsample; not bilateral)
      - [x] Lip color + feather (four-arc bands, Lab a/b)
      - [x] Teeth whitening (inner-lip opening, erode, Lab b* toward 128)
- [x] Decode via `make_frame_pump`; normalize with `to_bgr24`
- [x] Video output: lazily open existing `VideoEncoder`, call
      `set_constant_rate(true)`, use `pump->source_fps()` with 30 fps fallback,
      write every processed frame, and always close/finalize on clean end
- [x] Still output: process the single frame and use existing `save_frame`
- [x] Landmark overlay opt-in (`--show-landmarks`) for debugging only

Acceptance: a clip or still comes out with visible skin / lip / teeth
changes, CLI-only. Mesh quality should match Tasks, not a home-grown ONNX
graph.

### Offline command control flow

```
validate input/dst/model
ensure_com_initialized()
make_frame_pump(input, { cancel = cli_cancel })
create BeautyProcessor once
for each frame:
  normalize BGR24
  timestamp = monotonic_timestamp(frame.timestamp_ms, frame_index, fps)
  processor.process(frame, timestamp)
  write frame
close encoder even after cooperative cancellation
```

For still input, construct the landmarker in IMAGE mode. For video, construct
it in VIDEO mode once and preserve monotonically increasing timestamps. If
container PTS repeats/regresses, use `max(previous + 1, frame_index*1000/fps)`.

Model resolution order:

1. Explicit `--model-face`
2. Install models root: `exe/../models/vision/face_landmarker_v2.task`
3. Package prebuilt: `packages/mediapipe-tasks/prebuilt/win-x64/models/face_landmarker_v2.task`
4. `${MODELS_DIR}/vision/face_landmarker_v2.task`

Print every attempted path when resolution fails.

Bridge resolution is independent from model resolution. `video filter
--status` (or the command’s startup diagnostics) should report:

```
compiled: yes/no
dll: path | not found
abi: expected / actual
model: path | not found
available: yes/no
```

When no beauty option is requested, a missing bridge must not affect ordinary
video commands (`video record` defaults `--face` **off**). When beauty is
requested, fail before opening the output file. Fast WGC / NVENC cannot see
CPU BGR24 frames — beauty forces the compat frame pump.

### MVP pixel-op scope

SegmeCam’s face-effects file has extensive OpenCV use; copying the file is
not non-invasive. Port only its topology and formulas, backed by small
in-tree primitives:

- `LandmarkSet` + face/lip/eye index arrays
- scanline polygon mask
- binary erode/dilate
- separable Gaussian mask feather
- BGR <-> Lab conversion
- bounded-radius edge-aware smoothing
- masked per-pixel blend

First delivery order (done):

1. [x] Landmark debug overlay (proves geometry/orientation)
2. [x] Lip tint (polygon + feather + Lab)
3. [x] Teeth whitening (inner-lip mask + erode + Lab)
4. [x] Simple skin smoothing (face oval minus eyes/lips)

Defer advanced wrinkle detection, distance transforms, histogram analysis,
OpenCL, background segmentation, and SegmeCam’s full parameter matrix.

Unit-test each primitive on tiny deterministic buffers. Add golden-image
tests for mask orientation and effect boundaries; tolerate a small per-channel
delta for color conversion.

### Testing

Fixtures live in `tests/video-filters/`. The harness is
`tests/orchestrator/test-video-filter-basics.mjs` (same style as
`test-vibevoice.mjs`, using `test-commons.js`).

```
npm run tests:video-filter-basics
npm run tests:video-filter-basics -- --mode status
npm run tests:video-filter-stills
```

- [x] Synthetic fixtures: `blank-64.png` (0 faces), `checker-64.png`,
      `skin-swatch-64.png`
- [x] `tests:video-filter-basics` — package probe + CLI `--help` / `--status`
      / `--input`+`--dst` required
- [x] Model probe includes package prebuilt + `dist/models/vision/` so
      `--status` can report `available: yes` without a reconfigure
- [x] `face-spot.jpg` committed close-up; stills write
      `out/face-spot-{landmarks,lips,teeth,skin}.png`
- [x] Blank still → 0 faces; face-spot → 1 face, 478 landmarks (iris)
- [ ] Pixel-op unit tests on tiny buffers (skin / lips / teeth primitives)
- [ ] Golden stills under `tests/video-filters/goldens/` (small per-channel
      delta)

---

## Record path (same processor, not realtime yet)

`video record --skin/--lips/--teeth/--show-landmarks` (and `--face`) runs
`BeautyProcessor` in the existing Win32 H.264 frame-pump loop. Defaults stay
off so a missing MediaPipe DLL does not change ordinary record.

- [x] Shared Beauty flag group on `video filter` and `video record`
- [x] Record: VIDEO mode, monotonic capture timestamps, BGR24 before encode
- [x] Beauty on → skip fast WGC / NVENC; require `.mp4`
- [x] `video record --preview` — window client size matches capture
      (`--width/--height/--mode`, or device default); fit-to-screen if needed
- [ ] Drop-queue / live-stream FPS (slice 2)

`--skin` uses the FaceLandmarker oval as a bbox (eyes/lips cut out), then
downsample → box blur → upsample → mask blend. On Windows, D3D11 hardware
does that blur when `D3D11CreateDevice` succeeds; otherwise the same CPU
path. Lips/teeth stay CPU (small ROI). MediaPipe stays CPU. Drop-queue /
infer-crop are still slice 2.

## TODO 2 — Realtime (no virtual device yet)

Same processor on `VideoInput` / `make_frame_pump`. Use Tasks
**live-stream** or `DetectForVideo` with capture timestamps (tracking
depends on this). Record already calls the processor; this slice is
interactive preview + staying at camera FPS.

- [x] `pm-image video record --preview` — live window at capture size
      (not a drop-queue yet; encode still waits on each processed frame)
- [x] Optional `--dst` when `--virtual-out` is set (file is one sink)
- [ ] Optional `--dst` for preview-only (no file, no vcam)
- [ ] Fan-out: writer / vcam / preview are peer sinks (see Filter Design).
      XBlox `videoWriter` must peek, not consume, if another out follows.
- [ ] Measure FPS / infer_ms (same habit as `video detect`)
- [ ] **Perf gate** before virtual out or xblox:
      - [ ] One FaceLandmarker instance, reused
      - [ ] Never call the bridge from the MF capture callback; capacity-one
            latest-frame queue + drop
      - [ ] Scale-down for infer if 720p misses budget; apply mesh in
            full-res pixel space
      - [ ] Drop frames under load
- [x] PiP face / bg knobs on `WinCameraOverlayController` when
      `cameraOverlay` is on (screen stays WGC; beauty is the webcam bubble)

Acceptance: 720p webcam stays interactive with `--face --skin` on a typical
dev box. If not, do not start slice 3 or 4.

Required metrics in CLI status/logs:

- capture fps, processed fps, dropped frames
- p50 / p95 landmark inference ms
- p50 / p95 effect ms
- queue depth (must remain 0/1)
- selected input size and model path

---

## TODO 3 — Virtual out (MF Frame Server)

Zoom / Teams pick **Pixlwiz Cam**. **MF Frame Server** (`IMFVirtualCamera`).
Do not reopen OBS / libobs / `win-dshow` virtualcam sources.

```
BeautyProcessor → VideoFrame BGR24 → scale 1280×720 → NV12
                                              ↓
              %ProgramData%\Pixlwiz\vcam.frames  (file mapping)
                                              ↓
                    tanit_vcam_source.dll  (IMFMediaSource)
                                              ↓
                         IMFVirtualCamera (session, CurrentUser)
```

Public API is `pm::vcam::VcamSink` — no CLI types. Same object for
`video record --virtual-out` and XBlox `videoVirtualOut`.

- [x] **Media Foundation Frame Server** (`IMFVirtualCamera`, Win11 /
      build 22000+). Soft-load `MFCreateVirtualCamera` from
      `mfsensorgroup.dll`
- [x] Split: producer in `pm-image` / `tanit-cli`; own COM/MF DLL is the
      device; shared memory between them
- [x] Fixed v1: 1280×720 @ 30. Source advertises RGB32 + NV12 (VCamSample
      order) and uses Frame Server’s `IMFVideoSampleAllocator`
      (`UsesProvidedAllocator`) so OBS does not freeze. SHM stays NV12.
- [x] `FEATURE_VIDEO_VCAM` (requires video, Win32). OFF on Store
- [x] HKLM `InprocServer32` for the source CLSID (Frame Server is Local
      Service). One-time UAC via `video vcam-register` / auto-prompt from
      `--virtual-out`. HKCU is written too but is not enough.
- [x] CLI: `--virtual-out` / `--virtual-out-name "Tanit Cam"`;
      `--dst` optional
- [x] XBlox `videoVirtualOut` (same sink; process-lifetime)
- [ ] Sign device DLL + installer with the cert pipeline in progress
- [ ] Clean uninstall / HKCU teardown
- [ ] DShow 32-bit only if a specific app is blind to Frame Server

Frictionless: no OBS plugin, no extra GUI. Missing DLL / Win11 API
print a one-line error. First `--virtual-out` on a machine may raise
UAC once (`video vcam-register`) so Frame Server can load the source
from HKLM + ProgramData. After that, session + CurrentUser is enough.

Acceptance: Zoom / Teams / Meet / Discord / Chrome / OBS (Video Capture
Device) show the live pipe while `video record --virtual-out` (or an XBlox
graph with `videoVirtualOut`) is running. Windows may append “Windows
Virtual Camera” to the name. Session lifetime — stop the process and the
device disappears. A few old DirectShow-only / 32-bit clients may not see
Frame Server cameras.

### Format / res — not pass-through (v1)

Zoom / OBS always get **1280×720 @ 30**, even if capture is 1080p.
Every frame is nearest-neighbor scaled, then BT.601 BGR→NV12 into
`%ProgramData%\Pixlwiz\vcam.frames`. The source advertises RGB32 + NV12;
RGB32 clients get a convert from that NV12. The header stores
`width` / `height` / `fps`; the source ignores them and uses the constants.

Why it is locked: Frame Server negotiates the type when the client opens
the device. Changing size after Zoom/OBS have opened usually fails or goes
black. A fixed contract keeps the DLL, SHM, and clients aligned.

If we ever grow it (not scheduled):

1. **Size at start** — `VcamOptions` width/height/fps from the first frame
   or from `--width/--height/--fps`. SHM + advertised type match that.
   Still NV12. Session-fixed; switch camera mode = restart `--virtual-out`.
2. **Skip convert when it already matches** — same advertised type; skip
   scale (and later skip color convert if the pump is already NV12).
3. **True format pass-through (BGR / MJPG / …)** — do not. Cameras are
   NV12/YUY2 to Zoom/OBS. Keep NV12 out.
4. **Live resize / several modes** — advertise 720p and 1080p, or
   renegotiate mid-call. Later Frame Server slice, not this one.

---

## TODO 4 — XBlox (filters still deferred)

`videoCapture` now has the same built-in knobs as `video record --skin
--lips --teeth --virtual-out` (compat CPU pump). Per-filter pipe blocks
still wait on the slice 2 perf gate.

- [x] `videoCapture` built-in: `skin` / `skinBlend`, `lips` / `lipsColor` /
      `lipsBlend`, `teeth` / `teethBlend`, `landmarks`, `virtualOut` /
      `virtualOutName` — same `BeautyProcessor` + `VcamSink` as CLI
- [ ] `videoBeautify` next to `videoGrayscale` (same BGR24 / `FrameStore`
      contract — still no OpenCV)
- [ ] Same option names as CLI on the pipe blocks
- [x] `videoVirtualOut` — same `VcamSink` as `video record --virtual-out`
      (item-pipe peer sink; recorder uses the built-in knob)
- [ ] Live path stays a tight pump; blocks configure the processor. Do not
      wrap every frame in the xblox runner until slice 2 numbers are good

---

## Next filters (candidates — not scheduled)

The meeting / OBS cam is the product. Add effects that make a face look
fine on **Pixlwiz Cam**, not a makeup suite. Same rules as MVP: own BGR24
ops, same `BeautyProcessor` + `video filter` stills first, then `video
record` / vcam. XBlox `pd_*` only after the slice 2 perf gate.

Already shipped (face): **skin**, **lips**, **teeth**, **landmarks**,
**face-gain**, **warmth**, **undereye**, **redness**, **eye-sharpen**.
Already shipped (no mesh, XBlox only): `videoGrayscale`, `videoColor`
(brightness / contrast / sat), `videoCrop`, `videoResize`.
Shipped (no mesh): **mirror**. Shipped (ImageSegmenter): **`--blur` / `--background`**.

### Do these if we add more

Cheap, visible, reuse the 478-pt mesh (or no model). Order is the guess
at “needed”:

| Filter | Why it earns a slot | How |
|--------|---------------------|-----|
| **Mirror / flip** | Every webcam app has it. Zoom users expect to see themselves the way a mirror does. | **Shipped.** No mesh. Horizontal flip on BGR24 before infer / encode / vcam. `--mirror` on record + filter. |
| **Face exposure / warmth** | Webcams crush faces or go cold. Skin blur does not fix that. | **Shipped.** Face-oval Lab L* / warm b* toward a meeting target. Same oval as skin. `--face-gain` / `--warmth` on filter + record + `videoCapture`. |
| **Under-eye lift** | Dark circles read as tired on 720p vcam. High meeting value. | **Shipped.** Under-eye landmark bands, lighten + slight blur, strong feather. `--undereye` / `--undereye-blend`. |
| **Redness mute** | Skin is only a blur. Acne / flush still shows. | **Shipped.** Oval minus eyes/lips; pull Lab a* toward neutral. `--redness` / `--redness-blend`. |
| **Eye sharpen** | Skin smooth softens irises. A little unsharp on the eye polys restores focus. | **Shipped.** Tiny ROI, after skin. `--eye-sharpen` / `--eye-sharpen-blend`. |
| **Background blur / replace** | The one Zoom feature people actually ask for after beauty. | **Shipped** on `video filter` / `video record` / `videoCapture`. Tasks `ImageSegmenter` (`pm_seg_*` in the face DLL) + box/D3D blur or color/image replace. Same pipe as [video-next.md](./video-next.md) PiP `blurBackground`. Needs a rebuilt bridge DLL that exports `pm_seg_*` and `selfie_segmenter.task`. |

`videoColor` already exists as an XBlox block. If record/vcam needs a
global grade, wire those knobs onto the pump rather than a second
color-science stack.

### Maybe later (same mesh, easy to fake)

- **Blush** — cheek patches, Lab a*, low alpha. Pretty, not needed for meetings.
- **Brow / lash darken** — thin landmark strokes; looks costume-y if strong.
- **Chroma key** — no new model; useful for OBS users with a green wall.
  Separate from person-segment blur.

### Do not add unless someone asks

- Face slim / big-eye **warp** — uncanny, tracking jitter, not a meeting look.
- Full SegmeCam wrinkle / histogram / OpenCL matrix — skin is already a
  cheap ROI blur.
- Blendshapes / transformation matrices — extra infer cost, no pixel win
  for v1 effects.
- LUTs, vignette, film grain, beauty “presets” — creator candy; after the
  cam is realtime.
- Glasses glare, beard, hair recolor — bad masks, high regret.

Ship one row from the table at a time. Still fixture on `face-spot.jpg`,
then a short `--preview` / `--virtual-out` take. Do not open
`--blur` / `--background` until 720p `--face --skin` holds camera FPS
(slice 2).

---

## Radar (keep, don’t schedule)

- Next face / cam filters: table above (meeting grade + under-eye +
  redness + eye sharpen shipped; segmenter blur already shipped)
- Preset JSON for built-in looks (not Effect House). Face swap = later
  stage, not a beauty `apply_*` — see Filter Design notes
- PiP face / bg on `WinCameraOverlayController` — **shipped** when
  `cameraOverlay` is on (same `BeautyProcessor` as the cam path)
- DShow 32-bit client fallback
- Vcam size-at-start / skip-convert (still NV12; see TODO 3). Not live resize.
- Web sliders / imgui preview
- Kernel AVStream — never
- OpenCV / ONNX face-mesh rewrite — only if the Tasks prebuilt fails to ship

---

## Option deploy checklist (when a reserved flag becomes real)

1. CLI flag on `video filter` (append fields at end of `PmImageCliState`)
2. Processor config struct + `*_from_json` if xblox will share it later
3. Note “reserved” in `--help` until the slice’s runtime ships
4. XBlox `pd_*` only in slice 4
5. `GROUP_LABEL` in BlockHelpPanel when blocks exist

---

## Play (fixtures)

From the repo root. Outputs land in `tests/video-filters/out/` (gitignored).
Stills default to `--running-mode image`. `--face` is on unless you pass
`--no-face`.

PowerShell treats `#` as a comment — quote hex colors (`"#C41E3A"`) or omit
the hash (`C41E3A`).

```
set CLI=dist\win-x64\tanit-cli.exe
set IN=tests\video-filters
set OUT=tests\video-filters\out
```

Probe the bridge:

```
tanit-cli video filter --status
tanit-cli video filter --help
```

Blank still (must report `faces: 0`):

```
tanit-cli video filter --input blank-64.png --dst blank.png --show-landmarks
```

Spotted close-up — one effect at a time:

```
tanit-cli video filter --input face-spot.jpg --dst play-landmarks.png --show-landmarks
tanit-cli video filter --input face-spot.jpg --dst play-lips.png --lips
tanit-cli video filter --input face-spot.jpg --dst play-lips-hot.png --lips --lips-color "#C41E3A" --lips-alpha 0.55
tanit-cli video filter --input face-spot.jpg --dst play-teeth.png --teeth --teeth-strength 0.7
tanit-cli video filter --input face-spot.jpg --dst play-skin.png --skin --skin-strength 0.8
tanit-cli video filter --input face-spot.jpg --dst play-gain.png --face-gain 0.4
tanit-cli video filter --input face-spot.jpg --dst play-warmth.png --warmth 0.45
tanit-cli video filter --input face-spot.jpg --dst play-undereye.png --undereye
tanit-cli video filter --input face-spot.jpg --dst play-redness.png --redness
tanit-cli video filter --input face-spot.jpg --dst play-eye.png --eye-sharpen
tanit-cli video filter --input face-spot.jpg --dst play-meet.png --face-gain 0.35 --warmth 0.3 --undereye --redness --eye-sharpen --preview
```

Stack them (skin first, then teeth, then lips):

```
tanit-cli video filter --input face-spot.jpg --dst play-all.png --skin --skin-strength 0.7 --lips --lips-alpha 0.4 --teeth
tanit-cli video filter --input face-spot.jpg --dst play-all-mesh.png --skin --lips --teeth --show-landmarks
```

Optional MediaPipe portrait (only if you fetched it):

```
node tests\video-filters\make-fixtures.mjs --fetch-face
tanit-cli video filter --input face.jpg --dst play-portrait.png --skin --lips --show-landmarks
```

Checker / skin swatch (no face — effects are no-ops, good for orientation):

```
tanit-cli video filter --input checker-64.png --dst play-checker.png --show-landmarks
tanit-cli video filter --input skin-swatch-64.png --dst play-swatch.png --skin
```

Clip (when you have one). VIDEO mode + encoder — test harness:

```
tanit-cli video filter --input talk.mp4 --dst talk-out.mp4 --face --skin --lips --teeth
```

Product path — webcam record (face off unless you pass a beauty flag):

```
tanit-cli video record --help
tanit-cli video record --dst rec-lips.mp4 --lips --preview --duration-ms 4000
tanit-cli video record --dst rec-mesh.mp4 --show-landmarks --preview --width 1280 --height 720
tanit-cli video record --dst rec-meet.mp4 --preview --width 1280 --height 720 --face-gain 0.35 --warmth 0.3 --undereye --redness --eye-sharpen
```

Virtual cam (Win11). Device name **Pixlwiz Cam** (Windows may add
“Windows Virtual Camera”). `--dst` is optional. The device is always
1280×720 NV12 @ 30 — capture `--width/--height` does not pass through:

```
tanit-cli video record --virtual-out --preview --lips
tanit-cli video record --virtual-out --dst rec.mp4 --skin --preview
tanit-cli video record --virtual-out --virtual-out-name "Tanit Cam" --preview
```

XBlox: `videoSource → …filters… → videoVirtualOut`. Same sink, same device.

`--preview` opens a window whose client pixels match the captured frame
(the same size `--width/--height/--mode` asked the camera for). Close the
window to stop. If the frame is larger than the screen, the window is
scaled down and the title says `(fit)`.

`--skin` on a live 1080p camera is not realtime yet. Use `video filter` stills
to judge skin, or record a short clip at a smaller `--width/--height`.

```
tanit-cli video record --dst rec.mp4 --preview --width 1280 --height 720
tanit-cli video record --dst rec-lips.mp4 --lips --preview --duration-ms 8000
```

### Filter Design

Minimal shared surface so CLI (`video filter` / `video record`) and XBlox
(`videoGrayscale` … `videoBeautify` / `videoVirtualOut`) call the same
pixel ops. No CLI types, no `BlockIO`, no nlohmann in `src/video/filters/`.
Hosts adapt; filters do not know who called them.

XBlox already has the frame spine in
`src/xblox/blocks/video_filter_blocks.cpp`:

```
resolve_frame_consume → packed BGR24 → op(frame) → emit_frame (mem:// or path)
```

`videoVirtualOut` is the same consume/normalize, then a **sink**
(`VcamSink::push`), then re-emit. Sinks are not filters. Preview and
`VideoEncoder` are sinks too.

```
IFramePump / videoSource
  → FilterChain                 // sequential, 0..N
  → fan-out (same frame, peek)
       ├─ videoWriter / --dst   // one out; may drop
       ├─ videoVirtualOut       // another out; latest-frame
       └─ preview               // another out
```

#### Sinks / realtime (notes)

This is the gap for “really real-time, flexible.” Filters are a
straight line. **Outs are peers.** `videoWriter` is one sink.
`videoVirtualOut` is another. Preview is a third. None of them is
“the” end of the pipe.

Today that is only half true:

- CLI `video record` already fans out in one loop (`write_file` +
  `vcam.push` + preview). Encode still **blocks** the next capture
  frame (slice 2 drop-queue not done).
- XBlox `items` is a **linear consume** chain
  (`tests/xblox/video-detect-filter.xblox`). `videoWriter` uses
  `resolve_frame_consume` and does **not** re-emit. Put writer before
  vcam and the handle is gone. `pictureOut` already peeks
  (`resolve_frame`). Sinks should peek; only filters consume+emit.

Graph shape — do not invent a new `ParamKind`. Add a second child
list (or a convention: last N blocks are sinks that peek):

```
{
  "kind": "videoSource",
  "input": "",
  "items": [
    { "kind": "videoBeautify", "skin": true, "blend": 0.45 }
  ],
  "outs": [
    { "kind": "videoWriter", "outputPath": "rec.mp4" },
    { "kind": "videoVirtualOut" }
  ]
}
```

`items` = sequential filters (consume + emit, one landmarker if
`videoBeautify`). `outs` = same `mem://` token, **peek**, any mix,
order does not matter. Empty `outs` = today’s writer-in-`items`
fixture still works.

Realtime rules (this is slice 2, not more filter math):

1. Capture never waits on a sink. Capacity-one latest-frame queue
   between pump and `FilterChain`. Overrun = drop, count it.
2. **Vcam / preview** always get the latest processed frame. They
   must not wait on MP4 encode.
3. **Writer** may skip a frame if the encoder is busy (`--dst` is
   optional; a live Zoom cam with no file is valid). Do not stall
   vcam for a disk hiccup.
4. One `FilterChain` per `videoSource` session — not one per `items`
   child on a live cam.
5. Metrics: capture fps, processed fps, sink fps (writer vs vcam),
   dropped (queue + writer).

Do not build a node graph / compositor. One source, one chain, N
sinks. Screen vs cam does not change this.

#### Signatures (in `pm::video`, no host headers)

Packed BGR24 in, packed BGR24 out. Mutate in place when size stays the
same (skin / lips / color). Replace the frame when size changes (crop /
resize) — same as today’s `run_filter` ops.

```cpp
struct FilterContext {
    VideoFrame& frame;          // packed BGR24
    std::int64_t timestamp_ms;  // 0 for IMAGE; monotonic for VIDEO
    const LandmarkSet* mesh;    // null if unused / no face
};

struct FilterDesc {
    const char* id;             // "skin" — CLI --skin, XBlox videoSkin
    bool needs_mesh;
};

// One apply per effect. Typed params, not a json bag.
bool apply_mirror(FilterContext& cx, const MirrorParams& p, std::string& err);
bool apply_skin(FilterContext& cx, const SkinParams& p, std::string& err);
bool apply_lips(FilterContext& cx, const LipsParams& p, std::string& err);
// …
```

`needs_mesh` means “read `cx.mesh`”. It does **not** mean “run
FaceLandmarker”. Infer is a stage on the chain, once per frame:

```cpp
class FilterChain {
public:
    bool configure(const BeautyConfig& cfg, std::string& err);
    bool process(VideoFrame& frame, std::int64_t timestamp_ms, std::string& err);
    const LandmarkSet& last_mesh() const;
};
```

`process`:

1. `to_bgr24` if needed
2. if any enabled filter `needs_mesh` → one `FaceLandmarker` detect
   (reuse the handle; VIDEO timestamps)
3. call each enabled `apply_*` in a fixed order (skin → teeth → lips →
   overlay; new rows append)
4. return the same buffer

`BeautyProcessor` becomes a thin owner of `FilterChain` + landmarker +
`SkinGpu`. Do not grow it into a second plugin API.

#### Two hosts, one apply

| Host | How it calls |
|------|----------------|
| CLI `video filter` / `video record` | Build `BeautyConfig` from `--skin` / `--lips-color` / … One `FilterChain` for the session. |
| XBlox live / `videoBeautify` | Same chain. One block, camelCase params, one landmarker. Use this on a pump. |
| XBlox single-op (`videoGrayscale` today) | `run_filter` + one `apply_*`. Fine for no-mesh ops (`mirror`, `color`). Do **not** put mesh filters on this path for live graphs (N infers). |
| XBlox sink | `videoVirtualOut` / `pictureOut` — consume + push/encode, not `apply_*`. |

XBlox block shape (copy `videoColor`, not `videoCapture`):

```
kind: videoSkin
params: input, blend, outputPath, storeAs
exec: {false, false, true}     // frame in → frame out
```

`input` is `pd_image().in().prev()` (mem:// or path). Empty `outputPath`
keeps `mem://`. Same `run_filter` helper; the `op` lambda fills
`SkinParams` from `io.f64("blend")` and calls `apply_skin`.

#### General param: `blend`

Every paint filter has one wet/dry mix: **`blend`** in `0..1`
(`lerp(src, fx, blend)`). `0` = original, `1` = full effect. That is
what `skin_strength`, `lips_alpha`, and `teeth_strength` already are —
one name, not a second knob.

```cpp
struct SkinParams { float blend = 0.5f; /* effect-only fields after */ };
struct LipsParams { float blend = 0.35f; std::string color = "#C45C6A"; /* … */ };
```

- XBlox: `pd_float("blend").opt().dflt(…).range(0.0, 1.0)` on every
  paint block (`videoSkin`, `videoLips`, …)
- CLI: `--skin-blend` / `--lips-blend` / `--teeth-blend`. Keep
  `--skin-strength` / `--lips-alpha` / `--teeth-strength` as aliases
  that write the same field
- Preset JSON: `"skin": { "blend": 0.45 }`
- Do **not** ship `strength` **and** `blend` on the same filter unless
  they mean different jobs (e.g. blur radius vs mix). Radius stays
  `feather_px` / `band_px`, not a second 0..1
- Mirror / crop / resize have no `blend` (they are not a mix)
- Optional later: chain-level `blend` on `videoBeautify` /
  `--beauty-blend` to scale a whole preset. Per-filter `blend` first

#### Param names

One field, two spellings. Core structs use snake. Hosts map:

| Struct field | CLI | XBlox |
|--------------|-----|--------|
| `skin` / `blend` | `--skin` / `--skin-blend` (`--skin-strength` alias) | `blend` on `videoSkin`; `skinBlend` on `videoBeautify` |
| `lips` / `color` / `blend` | `--lips` / `--lips-color` / `--lips-blend` (`--lips-alpha` alias) | `lipsColor` / `blend` |
| `teeth` / `blend` | `--teeth` / `--teeth-blend` (`--teeth-strength` alias) | `blend` on `videoTeeth` |
| `mirror` | `--mirror` | `mirror` on `videoMirror` or `videoBeautify` |

Defaults stay in the param struct (`SkinParams::blend = 0.5f`). CLI
`default_val` and XBlox `pd_float().dflt()` must match.

`*_from_json` only if `videoBeautify` needs a blob. Prefer discrete
`pd_*` like `videoColor` (`brightness` / `contrast` / `saturation`).

#### XBlox params (notes)

**No new `ParamKind`.** Do not add `frame`, `video_frame`, or `filter`.
PREVIOUS is already JSON. Filters already pipe a **string token**:
`mem://N` or a file path, typed as `image_path`. `FrameStore` holds the
pixels. That is the same contract as `videoGrayscale` / `videoColor` /
`videoVirtualOut` (`pd_image("input").in().prev()`,
`primary_output(ParamKind::image_path)`).

`ParamKind` is a UI + resolver tag (`block_types.hpp`). A new kind
touches pickers, OpenAPI mapping, and manifests for no runtime win.
`block_params.hpp` already says: compose features on an existing kind,
do not grow the enum.

Use what exists:

| Need | Kind / helper |
|------|----------------|
| Frame in / out | `pd_image` + `.prev()` / `image_path` result (`mem://` or path) |
| Strength / gain | `pd_float("blend")` + `.range(0, 1)` — see above |
| Toggles | `pd_bool` |
| Lip / grade colour | `pd_color` (already a kind) — not a raw string |
| Preset file | `pd_file` (the JSON look above) |
| Preset inline | `pd_json` only if someone pastes the blob; prefer the file + discrete knobs |
| Donor (swap, later) | another `pd_image("donor")` |
| Vcam name | `pd_str` (already on `videoVirtualOut`) |

Optional later, still **not** a new kind: `pd_frame()` as an alias for
`pd_image().in().prev()` with a fixed desc (“mem:// or image path”).
Composition helper only.

**No XBlox struct for `FilterContext` / `BeautyConfig`.** Those stay in
`pm::video`. The block reads `io.f64("blend")` and fills `SkinParams`.
Do not put a `json_value` “config” blob on `videoBeautify` as the
primary UI — same reason `videoColor` is three floats, not one object.

The real XBlox gap is **lifetime**, not kinds: a live cam is one
`FilterChain` for the session (CLI record). A graph of `videoSkin` →
`videoLips` re-infers if each block owns a landmarker. For live, one
`videoBeautify` (or keep the chain in the pump and let XBlox only set
params). Single-op blocks stay for stills / offline `mem://` chains.
Do not invent a streaming pin type to fix that.

#### Sources (cam / screen / file)

Filters do not know the source. `videoSource` already pumps **webcam,
screen/window, still, or file** into the same `mem://` PREVIOUS
(`video_core_blocks.cpp`). `make_frame_pump` routes:

| `input` | Kind | Finite? |
|---------|------|---------|
| `""` / `"NexiGo"` | camera | live until `frames` / `durationMs` / cancel |
| `"screen:0"` / `screen:N:wintitle=GLOB` | screen | live |
| `clip.mp4` | file | EOF |
| `face.jpg` | still | one frame |

Existing fixture — file in, grayscale + color, writer out:

```
tests/xblox/video-detect-filter.xblox
  videoSource (mp4) → videoGrayscale → videoColor → videoWriter
```

Same graph, swap `input` only:

```
{ "kind": "videoSource", "input": "", "items": [ … ] }            // default cam
{ "kind": "videoSource", "input": "screen:0", "items": [ … ] }    // monitor
{ "kind": "videoSource", "input": "face-spot.jpg", "items": [ … ] }
```

CLI `video record --input` uses the same pump grammar (cam or
`screen:N`). Beauty / vcam sit on that pump, not on a camera-only path.

Mesh filters on a **screen** or a clip with no face: landmarker returns
0 faces, `apply_*` no-ops. That is correct — do not special-case
screen. `videoColor` / `mirror` / crop still do work on desktops.

Sinks after the same items: `videoWriter`, `pictureOut`,
`videoVirtualOut`. A meeting look is `videoSource` (cam) →
`videoBeautify` → `videoVirtualOut`. A desktop look is `videoSource`
(`screen:0`) → `videoColor` → `videoWriter` (or vcam). Mix freely.

#### What stays out

- No filter DLL / `RegisterFilter` / OBS-style plugin list
- No MediaPipe / OpenCV types in apply signatures
- No Frame Server / NV12 / shm inside a filter (that is `VcamSink`)
- No `BlockIO` or `CLI::App` in `src/video/filters/`
- No per-filter landmarker
- Crop / resize stay size-changing applies; they already exist as XBlox
  blocks — call the same functions if CLI ever needs them

#### Add-a-filter checklist

1. `FooParams` + `apply_foo` + `FilterDesc{"foo", needs_mesh}`
2. Still fixture on `video filter --foo` (`face-spot.jpg` if mesh)
3. CLI flag in the shared beauty group (`pm_image_register_video_beauty_options`)
4. Chain slot (order listed in `FilterChain::process`)
5. XBlox: either a field on `videoBeautify`, or a `videoFoo` `run_filter`
   wrapper — not both unless the single-op is no-mesh
6. `GROUP_LABEL` / `pd_*` only when the block exists

#### TikTok / custom / face swap (notes)

Direction only — not a slice. We line up with TikTok **built-in
retouch** (skin / lips / teeth / tone: mesh → masks → pixels). We do
**not** line up with Effect House: no marketplace, no JS, no 3D
stickers, no downloaded shader packs, no filter DLL.

TikTok “custom filters all around” are a sandbox + store. Ours is a
fixed `apply_*` chain. The only custom that fits this design is a
**preset**: JSON that turns built-in knobs. Unknown ids fail closed.
No extra models, no scripts.

```json
{
  "id": "meeting-soft",
  "version": 1,
  "chain": ["skin", "teeth", "lips", "undereye"],
  "params": {
    "skin": { "blend": 0.45 },
    "teeth": { "blend": 0.35 },
    "lips": { "color": "#C45C6A", "blend": 0.28 },
    "undereye": { "blend": 0.4 },
    "mirror": true
  }
}
```

CLI: `video filter --preset meeting-soft.json` (or a named look later).
XBlox: same blob on `videoBeautify`, or discrete `pd_*` that serialize
to this. Hosts map kebab / camel; the file uses the struct field names.

An Effect House–shaped pack (assets + graph + `index.js`) is a second
runtime. Keep it out of `src/video/filters/`. Do not invent one to
“match TikTok.”

**Face swap** is not an `apply_*` next to lips. It is a **stage**, same
class as `ImageSegmenter`: extra model, extra input (donor image /
embedding), temporal lock or it flickers. Cheap mesh-warp of a still
onto our 478 pts is technically `needs_mesh` + a donor buffer; it looks
like a Halloween filter and jitters. ML swap is its own C ABI / ONNX
handle, not a bool on `BeautyConfig`.

If it ever exists:

```
FilterChain
  → mesh (once)
  → [optional] swap stage      // not apply_lips
  → beauty apply_*             // or skip
  → sinks
```

XBlox: `videoFaceSwap` with `input` + `donor` (`mem://` or path). CLI:
`--swap --swap-from face.png`. Stills first; live needs its own perf
gate. Leave it off the meeting-cam list until someone asks.
