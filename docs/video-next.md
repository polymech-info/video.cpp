# Video next — creator features

Already solid for developers: cameras/screens/windows, still + record +
sessions, WGC vs compat, viewport/zoom/follow, audio mux, streaming, YOLO,
pipeline mode.

For professionals / content creators, prioritize a few higher-level features
over more low-level knobs. **Do not implement everything below.** Keep the
radar; ship in slices. Deploy option schemas early so UI/CLI/docs stay
stable while behavior lands later.

---

## Near-term slices

Options (xblox params + CLI flags + transform config) ship first — reserved /
no-op until the slice’s runtime lands. That avoids rebuild churn when
implementation arrives.

| Slice | Priority | Options now | Runtime |
|-------|----------|-------------|---------|
| **1. Cursor** | Top | yes | **live** (highlight / clicks / trail / hide-when-typing) |
| **2. Camera overlay** | After cursor | yes (PiP + `cameraAudio` / gain) | **live** (PiP + webcam mic mix) |
| **3. Audio polish** | Low-risk only | yes (narrow) | **not yet** (DeepFilterNet live; AGC/normalize reserved) |
| **4. Capture idle** | Next | **yes** | tiled luma + input/speech; pause-reason bits; ~500 ms preroll |
| **5. Captions** | With idle | **yes** | `stt_route` param kind + SRT/VTT/SBV; `captionStt` on video |

### Slice 1 — Cursor (top)

Tutorial-grade pointer feedback. Builds on `includeCursor`.

```
cursorHighlight          bool
cursorHighlightColor     #RRGGBB
cursorHighlightSize      px (0 = auto)
cursorClicks             bool   — click ripples
cursorClickColor         #RRGGBB  — left click; right/middle = +180°/+120° hue
cursorTrail              bool
hideCursorWhenTyping     bool
```

Group: `cursor`. Fast WGC / interactive HUD path (same layered-window path as
zoom status + key overlay). Capture-visible by default so FX appear in the MP4
even when the zoom HUD is `WDA_EXCLUDEFROMCAPTURE`.

### Slice 2 — Webcam overlay (after cursor)

PiP camera without forcing OBS.

```
cameraOverlay            bool
cameraAudio              bool   — webcam mic into record mix (WASAPI, matched to cameraInput)
cameraAudioGain          float  — linear gain for camera mic (0 mute … 1 unity … 4)
cameraInput              device name (empty = default camera)
cameraResolution         auto | max | WxH[@fps] | 720p/1080p/1440p/4k
cameraPosition           top-left | top-right | bottom-left | bottom-right
cameraSize               percent of short edge (or px when > 1 and integer-like)
cameraShape              circle | rounded | square
cameraBorder             bool
cameraBorderColor        #RRGGBB
cameraShadow             bool
```

Group: `camera_overlay`. Fast WGC path: parallel MF `VideoInput` + layered
HWND (same capture-visible idea as cursor FX). Not burned into the encode
graph — WGC picks up the HWND. Post-export composite (Recordly-style) stays
radar if we need cleaner masters later.

PiP capture uses a **native device mode** from `enumerate_device_modes`.
`cameraResolution`:
- `auto` — enough pixels for ≈2× the on-screen PiP, near 30 fps
- `max` — highest-area native mode (4K when the cam has it)
- `1920x1080` / `3840x2160@30` / `4k` — nearest matching native mode

UI lists live modes via `video.camera.modes.{cameraInput}`.

**Camera sound:** `cameraAudio` / `cameraAudioGain` mix a third WASAPI capture leg
into `MixedAudioCapture` (with `audioSource` none/mic/desktop/mix). Device is
resolved from `cameraInput` against audio capture names (token overlap with the
webcam friendly name).

PiP corners track the active **zoom / follow viewport** (screen-mapped
`m_lastViewport`, same as the zoom HUD / key overlay), falling back to full
capture bounds when no viewport is available.

**PiP beauty:** when `cameraOverlay` is on, the same face / background knobs
as `video record` (`skin`, `blur`, `mirror`, …) run on the PiP webcam only.
The screen stays on fast WGC. Tracked in [filters-next.md](./filters-next.md).

### Slice 3 — Microphone polish (low risk only)

```
audioNormalize           bool  — peak/RMS normalize into AAC (post-mix)
audioAutoGain            bool  — gentle AGC on mic path
```

Group: `audio` (alongside existing `audioSource` / gains). Options exist
(warn-only today).

**Stack already in tree (WebRTC APM):**

| Piece | Where | Notes |
|-------|--------|--------|
| WebRTC AudioProcessing | `src/audio/echo_cancellation.cpp` | AEC + HPF + NS + **AGC2** |
| Defaults | `src/audio/audio_constants.hpp` | `k_apm_*` all `true` (AGC2 on) |
| Wired today | `AudioInput` @ **16 kHz mono** only (`audio.cpp`) | STT / realtime path |
| Video record path | `MixedAudioCapture` → `MfVideoAudioMux` | **48 kHz stereo**, static `micGain` / `desktopGain` only — **no APM** |

So we do **not** need a new DSP library for `audioAutoGain`. Gap is plumbing:

1. **`audioAutoGain`** → enable WebRTC **AGC2** on the **mic leg** before mix/upmix.
   - Extend APM init beyond 16 kHz mono (at least 48 kHz mono), or capture mic mono→APM→upmix to stereo for mux.
   - Do **not** run AGC on desktop loopback (comment in `audio_constants` already says OS mixer normalises that).
   - Optional: expose NS/AEC toggles later; APM already has the knobs (`k_apm_ns`, `k_apm_echo_cancel`).

2. **`audioNormalize`** → **post-mix** peak/RMS scale toward target LUFS/peak before AAC
   (`MfVideoAudioMux` / encode write), separate from AGC. Not in APM today;
   small PCM scaler or two-pass offline. Live: slow limiter / target peak on mixed buffer.

**Not in options yet (higher risk DSP):** `echoCancellation`, `compressor`
— mostly already inside APM; surface as flags after AutoGain lands.

**Neural NS** is owned by
[audio-next-filters.md](../audio-next-filters.md). Live DeepFilterNet on
`video record --filter` / `videoCapture.filter` (mic/mix/cameraAudio) runs
on the mux worker, not the WASAPI callback. Virtual mic is skipped for now.
Do not add a second `noiseReduction` flag here.

### Slice 4 — Capture idle (auto-pause)

Skip dead air in the MP4 without stopping WGC / MF / WASAPI. This is a
**capture-idle detector**, not computer vision. No OpenCV.

Tanit’s fast screen path is **WGC**, not DXGI Desktop Duplication. Do not add
a DXGI DUP backend just for dirty rects. Clipped `rrect` / zoom viewport:
score the **crop**, not the full capture item.

**Mental model**

```
activity =
    user_input
    OR speech_activity
    OR significant_frame_change

no activity for idleMs (~3s)  →  set PauseIdle   (AUTO_PAUSED)
activity confirmed            →  flush preroll → clear PauseIdle
```

Keyboard / mouse / speech are first-class activity, not mere “wake triggers.”
Typing into Monaco / a terminal / Notepad often changes fewer than four
logical tiles; input hooks are what keep those sessions recording.

```
autoPause                 off | visual | audio | both     default off
autoPauseIdleMs           0 = off; 3000 typical   (overall idle timer)
autoPauseResumeMs         300 typical (visual must persist; preroll covers the gap)
autoPauseGridW            32  (8–128)   logical tiles
autoPauseGridH            18  (8–72)
autoPauseProbeW           64  (min 64; 96 ok)  intermediate luma
autoPauseProbeH           36  (min 36; 54 ok)
autoPausePixelDelta       12  (luma 0–255)
autoPauseTilePercent      8   (% of a logical tile’s probe samples over delta)
autoPauseMinTiles         4   (visual only; typing is input activity)
autoPauseMinCoveragePercent  2   (sum(activeTile.changedFraction) / gridTileCount)
autoPauseSampleFps        8   (probe rate; independent of encode fps)
autoPauseMicWake          bool  default true when autoPause is audio|both
autoPauseMicActivityDb    -40   (RMS/peak dBFS on speech mic — not VAD/speech)
autoPauseMicHangoverMs    500   (300–700)
autoPauseKeyHoldMs        750   (500–1000; key/click/wheel)
autoPauseMouseHoldMs      350   (250–500; movement only — not idleMs)
autoPauseWakeKey          bool  default true (when autoPause != off)
autoPauseWakeMouse        bool  default true (when autoPause != off)
autoPauseMouseDeltaPx     4     (screen px; ignore jitter below this)
autoPausePrerollMs        500   (video frames kept while PauseIdle; 300–1000)
```

`autoPauseMinAreaPercent` (union bbox gate) is **removed** — sparse distant
changes explode the bbox. BBox may stay as an auxiliary feature, not a gate.

Group: `idle`. CLI: `--auto-pause`, `--auto-pause-idle-ms`,
`--auto-pause-grid 32x18`, `--auto-pause-mic-wake`,
`--auto-pause-mic-activity-db`, `--auto-pause-preroll-ms`,
`--auto-pause-wake-key` / `--auto-pause-wake-mouse`.

**Visual probe (mandatory two-stage)**

```
capture
  → 64×36 (or 96×54) luma plane
  → aggregate into 32×18 logical tiles   (4 or 9 samples / tile)
  → tilePercent has meaning
```

Dirty regions are a **cheap ROI hint**, not activity truth (caret, compositor,
hover, GPU surfaces, video controls all invalidate large rects):

```
dirty regions
    → which tiles need luma checking?
    → luma/hash comparison only for those tiles
    → coverage = sum(activeTile.changedFraction) / gridTileCount
```

If dirty metadata is missing, check every logical tile. Never map dirty
rects directly onto “this tile is active.” `ReportOnly` only if the OS
exposes WGC `DirtyRegionMode` — never `ReportAndRender`.

```
visual_activity =
    coverage >= minCoveragePercent
    AND changed_tiles >= minTiles
```

**Speech / mic (swappable default function)**

Default is short-window RMS/peak on the **speech mic leg** (pre-mix, after
gain). Parameter is `autoPauseMicActivityDb` — do **not** imply VAD/speech.
WebRTC APM/AGC2 VAD can replace the function later without changing the
idle state machine. Desktop-only loopback does not count (OS mixer /
YouTube tab would never pause). Speech mic = `audioSource` mic/mix **or**
`cameraAudio`.

```
mic_hot = AudioActivityFn(pcm)   // default: peak_dbfs >= autoPauseMicActivityDb
          then hangover autoPauseMicHangoverMs
```

`autoPause=both`: a hot mic keeps recording while visuals are static
(talking-head). `audio`: ignore visual idle. `visual`: ignore mic.

**Input hold times** (signals stay hot briefly; the **overall** idle timer
is still `idleMs`):

| Signal | Hold |
|--------|------|
| key / click / wheel | `autoPauseKeyHoldMs` (~750) |
| mouse movement | `autoPauseMouseHoldMs` (~350) — **not** `idleMs` |
| visual | `resumeMs` hysteresis |
| mic | `autoPauseMicHangoverMs` (~500) |

Ignore `LLMHF_INJECTED` / `LLKHF_INJECTED`. Do **not** count `pauseKey` /
`stopKey`. Clicks and wheel always count; cursor jitter below
`autoPauseMouseDeltaPx` does not. Prefer
`WinInteractiveZoomController` LL hooks; `GetLastInputInfo` when hooks
are off (compat / no HUD).

**Pause reasons — never auto-call `toggle_pause()`**

```
PauseNone = 0
PauseUser   = 1 << 0
PauseIdle   = 1 << 1
PauseSystem = 1 << 2

recording_active = (pauseReasons == PauseNone)
```

`pauseKey` sets/clears **only** `PauseUser`. Auto-resume clears **only**
`PauseIdle`. Manual pause always wins.

**Pre-roll (~500 ms, almost mandatory)**

Capture is already running. While `PauseIdle` (and not `PauseUser`), keep a
circular buffer of recent video frames (~15 @ 30 fps) and optionally
300–500 ms of audio. On confirmed activity: flush preroll, then live
frames. Lets `resumeMs` reject noise without clipping compile-done /
remote slide / external-device starts. Keyboard edge-wake does not cover
those cases.

**Do not stop capture.** WGC `FrameArrived`, MF webcam pump, and WASAPI
stay running. Pause means: stop encoder submit / AAC `WriteSample`.
Probe + mic meter + input hooks keep going.

```
capture / probe / mic_meter / input_hooks = always running
if (recording_active) encoder.Submit / mux.WriteSample
```

Compat/webcam uses the same skip + encoded PTS (no capture restart).

### Encoded timeline (A/V + captions)

One session clock. Paused wall time is omitted. Stop independently
shifting `m_pts_t0`.

Prefer sample-contiguous PTS:

```
video next PTS = previous video PTS + frame duration
audio next PTS = previous audio PTS + samples_written / sample_rate
```

(equivalent: `encodedTime = wall - totalPaused - currentPause`). Pieces that
each know about `m_pts_t0`, WASAPI QPC, and WGC times are where pause-before-
first-frame, multiple pauses, encoder backpressure, and dropped frames
desync.

| Clock | Behavior |
|-------|----------|
| Video | contiguous frame-duration PTS; pause skips writes (no t0 shift) |
| Audio mux | sample-contiguous after first sample; first chunk at 0 (or last video PTS). Burst-drop vs wall only on the first chunk. |
| WASAPI | callback **always** runs; mix + speech meter while paused; `WriteSample` only when `recording_active`. Drain DeepFilterNet on PauseIdle/User (no lookahead leak). |
| Captions | encoded clock, never WGC `SystemRelativeTime` / WASAPI QPC |

### Slice 5 — Captions (main STT, not voice commands)

Not [asr-commands.md](../audio/asr-commands.md) / `voiceListen`. That stack is
wake-prefix + command matching. Captions use the **dictation STT path**:

| Surface | Role |
|---------|------|
| App Settings `stt_provider` / `stt_model` | Default routing (`bp::resolve_stt`) |
| `audioTranscribe` | Xblox STT: file or live mic/desktop/mix; `provider` / `model` / `providerOptions` already live here |
| `audio record --stt` / `--text-out` | CLI STT ([whisper-cpp.md](../audio/whisper-cpp.md) for local ggml) |
| `src/audio/stt/*` | whisper.cpp, Tanit/pixlwiz proxy, ElevenLabs |

**STT routing is one param kind, not a pile of video fields.** Same pattern as
`llm` `preset` / `router` / `model` (`options_path` +
`options_path_template` + `provider_options` dyn schema). Do **not** add
flat `captionProvider` / `captionModel` / `captionLanguage` / live HUD on
`videoCapture`. Flexibility comes from a reusable **`stt_route`** kind.

New `ParamKind::stt_route` (`value_kind.hpp` + TS `ParamKind` +
`KNOWN_KINDS`). Stored as one JSON object (empty `{}` = inherit App
Settings Voice & Audio):

```
{
  "preset":   "",   // agent.presets — uses that chat preset's stt_provider/stt_model
  "provider": "",   // providers.stt; empty = preset, then settings
  "model":    "",   // providers.stt.models.{provider}
  "options":  {}    // same xbloxProviderOptionsSchemaGet as audioTranscribe
                    // (whisper: language, backend, threads, flashAttn)
}
```

Dyn UI on the kind (not ad-hoc per block):

```
.dyn({
  "kind": "stt_route",
  "preset_options_path": "agent.presets",
  "provider_options_path": "providers.stt",
  "model_options_path_template": "providers.stt.models.{provider}",
  "schema_method": "xbloxProviderOptionsSchemaGet",
  "mode": "stt"
})
```

Widget: existing `ProviderModelPicker` `mode="stt"` plus the provider-options
panel already used when `dynamic_schema.kind === "provider_options"`. Resolve
with `bp::resolve_stt` after expanding preset → `ChatProviderPreset.chat`.

```
# videoCapture / video record
captions              off | sidecar     default off
captionFormat         srt | vtt | sbv | all    default srt
captionStt            stt_route         default {}   // one field

# audioTranscribe / audio record — same kind (migrate off flat provider/model)
stt                   stt_route         default {}   // aliases: provider, model, providerOptions
subtitleFormat        none | srt | vtt | sbv | all   default none
subtitlePath          empty → next to input / --text-out stem
subtitleMaxChars      42
subtitleMaxLines      2
```

`pd_stt_route("captionStt")` / `pd_stt_route("stt")`. Old `audioTranscribe`
blocks keep working: top-level `provider` / `model` / `providerOptions` map
into the object. No live HUD on this kind — `audioTranscribe mode=transcribe`
stays the dictation HUD.

CLI projection of the same object (no parallel caption-* STT flags):

```
video record --captions srt [--stt-preset Name] [--stt-provider whisper --stt-model large-v3-turbo]
audio record --from-wav rec.wav --stt --subtitle-format srt [--provider …]
```

Empty `--stt-provider` = settings / preset. Language lives in whisper
`options`, not a video language field.

| Provider | Main STT today | Timed cues |
|----------|----------------|------------|
| **whisper** | `whisper_full` on the dump; segments `t0/t1` → cues. Whole file if &lt; ~30 min; else ~10 min jobs with 2 s overlap. Do not shrink `audio_ctx`. | done |
| **tanit / pixlwiz** | File/WAV → string | Untimed unless the proxy grows segments |
| **elevenlabs** | Live PCM; `include_timestamps=true` → `committed_transcript_with_timestamps` | `audioTranscribe` cues + `audio record --json` `segments` |

**Composition** — still valid as a pipeline (`videoCapture` wav → `audioTranscribe`
with its own `stt` route → writer). Convenience: `captions=sidecar` dumps the
encoded-timeline 16 kHz mix, runs `captionStt` (or `{}` = settings), writes
sidecars. If STT is unset, warn and leave the wav. No live HUD on video.

**Formats** ([YouTube’s official list](https://support.google.com/youtube/answer/2734698)):

| Write | Why |
|-------|-----|
| **SubRip `.srt`** | Official YouTube input. UTF-8, **no markup**. Default. |
| **WebVTT `.vtt`** | W3C / HTML5 `<track>`. YouTube accepts it. |
| **SubViewer `.sbv`** | YouTube Studio download format. `sbv` or `all` only. |
| Not now | TTML/DFXP, SAMI, SCC, LRC. |

Cue wrap lives on the **writer** (`subtitleMaxChars` / `MaxLines`), not on
video viewport options.

**When to transcribe**

1. **After finalize (default)** — STT the encoded-timeline dump (pause
   already omitted). Whisper segments → cues. Proxy without times: one
   file-length cue or punctuation split (last resort).
2. **Live during capture** — out of slice 5 unless we later tap the
   existing ElevenLabs session inside `audioTranscribe` / `audio record`.
   Not a third STT client on the WGC thread.

**Burn-in** stays radar. Sidecar stays the YouTube upload path
(Studio → Subtitles). Payload: `subtitlePath` / `subtitlePaths` on the
audio (or convenience video) result, not a new STT settings surface.

---

## Implementation todos

### Slice 4 — idle

- [x] `IdlePauseConfig` in `creator_fx.hpp` + `pd_idle_pause_params()` /
      defaults merge; group `idle`; `GROUP_LABEL` in BlockHelpPanel +
      `nodeLayout.ts`.
- [x] CLI flags appended at end of `PmImageCliState`; `video record`
      `--auto-pause` / `--auto-pause-grid WxH` / mic-activity / preroll.
- [x] `CaptureIdleDetector`: 64×36+ probe → logical tiles, dirty ROI,
      coverage (not bbox gate), hold times, swappable `AudioActivityFn`.
- [x] Pause-reason bitmask (`PauseUser` / `PauseIdle`); never auto
      `toggle_pause()`. ~500 ms video preroll while `PauseIdle`.
- [x] WGC `on_frame`: downsample crop to probe; dirty rects = ROI only;
      skip encode when not `recording_active`; flush preroll on resume.
- [x] Webcam/compat pump: same detector; skip `enc.write` while paused.
- [x] Mic meter on WASAPI callback **while paused**; `autoPauseMicActivityDb`.
- [x] Key/mouse: LL hooks with split hold times; `GetLastInputInfo` fallback.
- [x] Encoded PTS = previous + duration (no t0 shift); audio sample-
      contiguous; filter queue drained on pause.

### Slice 5 — captions (STT owner = audio)

- [x] `ParamKind::stt_route` (C++ `ValueKind` + TS `KNOWN_KINDS`) +
      `pd_stt_route()`. Dyn UI: `agent.presets`, `providers.stt`,
      `providers.stt.models.{provider}`, `xbloxProviderOptionsSchemaGet`.
      Widget: `ProviderModelPicker` mode=stt + existing provider-options panel.
- [x] Resolve via preset → `ChatProviderPreset.chat` then `bp::resolve_stt`.
      `audioTranscribe` migrates to `stt`; keep flat provider/model aliases.
- [x] Timed segments on the **main** STT API: `whisper_local_*` returns
      `{text, t0_cs, t1_cs}[]`; `audioTranscribe` JSON includes `segments`
      when `json=true`.
- [x] ElevenLabs timestamp consumer on `audio record` / `audioTranscribe`.
- [x] `pm::subs` writers + `subtitleFormat` on audio STT / writer block.
- [x] `videoCapture`: `captions` + `captionFormat` + **`captionStt`**
      (`stt_route`, default `{}`). CLI `--stt-preset` / `--stt-provider` /
      `--stt-model` — not captionProvider/language/HUD.
- [x] Encoded-timeline 16 kHz dump while `recording_active`; caption times
      on that clock (slice 4). Leave the wav if STT is unset.

---

## Radar (keep, don’t schedule)

### Recording quality
`preset` (quality / balanced / performance / lossless), CFR vs VFR,
`keyframeInterval`.

### Scene automation
`hideNotifications`, `enableFocusAssist`, `hideDesktopIcons`, `hideTaskbar`,
`restoreAfterRecording`.

### Window recording extras
`captureMode` (window / client / region), `clipRoundedCorners`, `dropShadow`.

### Follow modes (future AI)
`activeWindow`, `focusedControl`, `face`, `object` — beyond current
`none` / `anchor` / `cursor`.

### Outputs
gif, webm, mov, png sequence.

### Instant sharing
`copyToClipboard`, `openFolder`, `openEditor`, `upload`, `shareLink`.

### Timers
`countdown`, `delayStart`, `splitRecordingEvery`.
(`autoStopOnIdle` folded into slice 4 auto-pause; we omit paused time
instead of stopping the session.)

### Watermark / branding
`watermarkImage`, `watermarkText`, `logoPosition`, `timestamp`.

### Productivity
`autoFilename`, `filenameTemplate`, `project`, `tags`
(`${date}_${window}_${counter}.mp4`).

### Privacy
`blurWindows`, `blurApps`, `blurRegion`, `redactEmails`, `redactFaces`,
`redactPasswords`.

### Creator profiles (high value, later)
Single `profile` (Tutorial / Gameplay / Presentation / …) that sets bitrate,
fps, cursor, audio, zoom, webcam, encoder. Power users still override fields.

### AI (differentiator)
`autoZoomSpeaker`, `autoFollowMouse`, `autoHighlightClicks`,
`summarizeRecording`, `generateChapters`, `extractHighlights`,
`generateThumbnail`, `generateTitle`, `generateDescription`,
`detectDeadAir`, `cropToShorts`.

### Enterprise
`recordAuditLog`, `recordConsent`, `recordWindowTitles`, `recordMetadata`,
`encryptRecording`, `signRecording`, `retentionDays`.

---

## Option deploy checklist

When adding a reserved option:

1. Flat param in `pd_*_params()` + defaults merge (xblox).
2. Config struct + `*_from_json` in `src/video/creator_fx.hpp`.
3. `VideoTransformConfig` / `VideoRecorderRequest` + host fill.
4. CLI state fields **appended** at end of `PmImageCliState` (stable offsets).
5. CLI flags on `video record`; warn if enabled before runtime exists.
6. `GROUP_LABEL` in BlockHelpPanel **and** `apps/xblox/.../nodeLayout.ts`.
7. Note “reserved” in `.desc` until the slice’s runtime ships.

New groups: `idle`, `captions`.

https://github.com/webadderallorg/Recordly
https://github.com/Padletut/segmecam
Foundation (tracking / mesh)
Project	What it is
MediaPipe Face Landmarker
Apache-2.0, 468-pt mesh, C++/Python/Android/Web. Best base for offline + live. Beauty is your shaders on top.
Built-in bilateral_filter_calculator
GPU joint bilateral inside MediaPipe graphs (skin smooth building block).
Closest to “beautify webcam / any video”
Project	Stack	Notes
SegmeCam
C++ / TFLite / OpenGL / Linux
Skin smooth, makeup, teeth, BG blur, virtual cam. Most on-point for a desktop beauty pipeline.
FaceRay
Python + MediaPipe + Tauri
Skin bilateral (eyes/mouth carved out), gaze, BG blur → virtual cam. Easy to steal the filter math.
face-light
Web / MediaPipe / WebGL2
Portrait smooth, lighting, BG blur. Good reference for multi-pass GPU beauty.
Makeup / reshape (heavier AR)
Project	Stack	Notes
OpenMakeupSDK
Web / MediaPipe / WebGL
Foundation, lips, reshape — npm package.
Face-Point
Android / MediaPipe / GLES
Realtime face filters + encode.
jeelizFaceFilter
WebGL JS
Lightweight face track + AR overlays (more stickers than beauty).


https://github.com/cth9191/animate

https://github.com/livekit/livekit
https://github.com/livekit/client-sdk-android?utm_source=chatgpt.com
https://github.com/GetStream/webrtc-android?utm_source=chatgpt.com