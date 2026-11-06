# video.cpp — Engineering Handover / TODOs

**Repository:** [https://github.com/polymech-info/video.cpp](https://github.com/polymech-info/video.cpp)\
**Scope:** Correctness, memory safety, modularity, portability, testing, performance\
**Priority:** P0 correctness → P1 architecture → P2 quality/performance\
**Status:** Static-review findings; fixes not yet implemented or verified

## 0. Objectives

Turn `video.cpp` into a reliable, independently buildable C++ multimedia library suitable for:

- Tanit Desktop (Windows/Linux/macOS)
- Tanit Android through C++/JNI
- Camera and screen capture
- Video recording and encoding
- Real-time video frame processing
- Computer vision pipelines
- WebRTC streaming integration
- Optional GPU acceleration

**Constraints**

- Native C++20, no mandatory heavyweight framework.
- Preserve existing Tanit integrations.
- No unnecessary frame copies.
- Optional dependencies must remain optional.
- Core must compile without Tanit's source tree.
- Maintain existing public interfaces where possible.
- Prefer incremental refactoring over a complete rewrite.

---

# P0 — Correctness and Memory Safety

## TODO-001 — Harden `scale_bgr24()`

**File:** `vcam/convert.cpp`\
**Priority:** P0

- Validate source and destination dimensions before allocation.
- Reject zero and negative dimensions.
- Use checked multiplication for buffer-size calculations.
- Validate source stride against width and bytes per pixel.
- Validate source buffer capacity against the maximum accessed byte.
- Reject unsupported pixel formats.
- Ensure the fast-copy path cannot read beyond the source buffer.
- Handle allocation failures without terminating the host unexpectedly.
- Define whether scaling preserves or normalizes stride.
- Avoid signed/unsigned conversion hazards.

**Acceptance criteria**

- Invalid frames return an explicit error.
- No undefined behavior under ASan/UBSan.
- Correct output for padded and tightly packed BGR24 frames.
- Existing valid-frame behavior preserved.

## TODO-002 — Harden BGR24 → NV12 conversion

**File:** `vcam/convert.cpp`\
**Priority:** P0

- Validate source and destination pointers/capacities.
- Validate dimensions and strides.
- Reject unsupported odd dimensions initially.
- Use checked calculations for Y and UV plane sizes.
- Verify chroma writes stay within the UV plane.
- Define color matrix (BT.601 vs BT.709).
- Define full-range vs limited-range conversion.
- Document plane layout and stride requirements.
- Verify correct 2×2 chroma subsampling.
- Add deterministic conversion reference tests.

**Acceptance criteria**

- No out-of-bounds accesses for malformed inputs.
- Correct NV12 output on supported dimensions.
- Explicit, documented color conversion behavior.
- Tests cover odd dimensions, padded strides and undersized buffers.

## TODO-003 — Audit every frame/pixel conversion routine

**Priority:** P0

Search for:

- Raw pointer arithmetic.
- Buffer sizes derived from untrusted dimensions.
- Integer overflow in width × height × channels.
- Pixel format assumptions.
- Unvalidated row strides.
- Negative dimensions converted to `size_t`.
- Unaligned access assumptions.
- In-place conversion with overlapping buffers.
- Read/write races involving shared frames.

Standardize on an internal validated `FrameView` abstraction with explicit capacity, format, stride and plane information.

**Do not introduce a breaking public API change until existing call sites are inventoried.**

## TODO-004 — Harden MJPEG AVI recording

**File:** `mjpeg_avi.cpp`\
**Priority:** P0

- Check every file operation.
- Handle short and failed writes.
- Propagate errors to the caller.
- Validate JPEG frame sizes.
- Validate all offset and size conversions.
- Detect RIFF/AVI 32-bit limits before overflow.
- Define explicit behavior when maximum file size is reached.
- Ensure finalize handles partially written files.
- Make finalize idempotent.
- Define behavior when destruction occurs without finalize.
- Ensure counters update only after successful writes.
- Add disk-full and simulated I/O failure tests.

**Acceptance criteria**

- No silent write failures.
- No 32-bit size wraparound.
- Recoverable failures produce clear error information.
- Valid output plays correctly in FFmpeg-compatible players.

---

# P1 — Architecture and Modularity

## TODO-005 — Make the library independently buildable

**File:** `CMakeLists.txt`\
**Priority:** P1

- Remove unconditional dependencies on Tanit source directories.
- Identify all source files requiring private Tanit headers.
- Replace private utilities with library-owned equivalents where justified.
- Keep Tanit-specific adapters outside the standalone core.
- Add standalone CMake configuration.
- Export proper CMake targets.
- Define PUBLIC/PRIVATE include directories correctly.
- Make optional dependencies explicit.
- Verify clean checkout builds on supported platforms.
- Add a minimal external-consumer compilation test.

**Acceptance criteria**

A clean checkout can configure and build its core without the Tanit repository.

## TODO-006 — Establish module boundaries

**Priority:** P1

Suggested modules:

| Module               | Responsibility                                                |
| -------------------- | ------------------------------------------------------------- |
| `pm-video-core`      | Frame representation, formats, timestamps, memory, conversion |
| `pm-video-capture`   | Camera, screen and window capture                             |
| `pm-video-record`    | Recording, codecs, encoding                                   |
| `pm-video-effects`   | Filters, segmentation, face effects                           |
| `pm-video-detect`    | YOLO and other inference adapters                             |
| `pm-video-streaming` | Streaming pipeline interfaces                                 |
| `pm-video-platform`  | Platform-specific backends                                    |

- Inventory existing source/header dependencies.
- Identify dependency cycles.
- Define public vs internal headers.
- Define source/sink interfaces independent of platform.
- Move Windows-specific code behind backend interfaces.
- Keep inference engines optional.
- Preserve the existing API through adapters if necessary.
- Avoid introducing virtual interfaces into performance-critical pixel loops.

**Target dependency direction**

`platform/capture/record/effects/detect/streaming → core`

Core must not depend on those higher-level modules.

## TODO-007 — Define canonical video frame contract

**Priority:** P1

Frame metadata:

- Pixel format.
- Width and height.
- Plane count.
- Plane pointers or buffer handles.
- Plane strides and accessible capacities.
- Timestamp and timebase.
- Memory location: CPU/GPU.
- Ownership and lifetime.
- Optional color-space metadata.
- Optional rotation/orientation metadata.

Design requirements:

- Non-owning `FrameView` for synchronous processing.
- Explicit owning/retained frame type for asynchronous processing.
- Avoid implicit deep copies.
- Allow externally owned buffers with explicit lifetime control.
- Support NV12 and packed RGB/BGR natively.
- Reserve extensibility for D3D11 textures and Android hardware buffers.
- Document thread-safety rules.

**Acceptance criteria**

Capture, conversion, recording and inference can exchange frames without platform-specific knowledge or unnecessary copies.

## TODO-008 — Introduce reusable frame pump

**Priority:** P1

Target:

`Source → Frame Queue → Processing → Sink(s)`

- Define source and sink lifecycle contracts.
- Implement bounded queues.
- Support push and pull consumption where needed.
- Define frame-drop policies.
- Support cancellation and deadlines.
- Propagate timestamps.
- Support fan-out to multiple sinks.
- Define behavior for slow consumers.
- Avoid blocking capture callbacks on network or inference operations.
- Define graceful shutdown and queue draining.
- Instrument dropped frames, queue depth and processing latency.

**Initial use cases**

1. Android Camera → processing → WebRTC encoder.
2. Windows screen capture → recording.
3. Camera → detection → annotated preview.
4. Camera → simultaneous preview and recording.

Do not build a general-purpose graph engine initially. Keep the pipeline minimal.

## TODO-009 — Clarify runtime initialization and ownership

**Priority:** P1

- Audit `PM_VIDEO_HOST_SINGLETONS`.
- Identify global and static mutable state.
- Define host initialization order.
- Define shutdown order.
- Ensure callbacks cannot fire after dependent objects are destroyed.
- Document thread affinity for Media Foundation/COM objects.
- Make repeated start/stop safe.
- Prevent stale callbacks from accessing freed contexts.
- Specify plugin loading/unloading behavior.
- Test simultaneous sessions.

---

# P1 — Build and Test Infrastructure

## TODO-010 — Improve compile-time coverage

**Priority:** P1

- Build with `PM_VIDEO_RECORD_ENGINES=ON` in CI.
- Build default minimal configuration.
- Compile optional feature combinations.
- Enable MSVC `/W4` on library-owned code.
- Enable GCC/Clang `-Wall -Wextra -Wpedantic`.
- Add warnings-as-errors CI jobs after baseline cleanup.
- Verify Windows x64 Debug/Release.
- Verify Linux x64 Debug/Release.
- Verify macOS arm64 where supported.
- Add Android arm64 core build.

## TODO-011 — Introduce CTest and sanitizer coverage

**Priority:** P1

- Register native tests with CTest.
- Preserve existing Node CLI tests.
- Add AddressSanitizer build.
- Add UndefinedBehaviorSanitizer build on supported toolchains.
- Add deterministic pixel conversion tests.
- Add malformed frame tests.
- Add recording failure-injection tests.
- Add lifecycle tests.
- Add concurrency tests.
- Add fuzz targets for frame conversion and container parsing.

**Acceptance criteria**

Tests are executable locally and automatically in CI without Tanit.

## TODO-012 — Build explicit backend capability reporting

**Priority:** P1

- Enumerate compiled-in backends.
- Enumerate runtime-available backends.
- Report codec availability.
- Report hardware acceleration availability.
- Provide backend initialization diagnostics.
- Distinguish unsupported, unavailable, initialization failure and runtime failure.

This is particularly important for Windows Media Foundation, NVENC and Android hardware codecs.

---

# P2 — Performance and Real-Time Behavior

## TODO-013 — Add video pipeline benchmarks

**Priority:** P2

Measure:

- BGR24 → NV12 conversion throughput.
- Frame resizing throughput.
- MJPEG writing throughput.
- Memory copies per frame.
- Allocation count per frame.
- Frame queue overhead.
- Pipeline end-to-end latency.
- Capture-to-encode latency.
- Dropped frames under overloaded sinks.
- CPU utilization and memory bandwidth.

Standard resolutions:

- 640×480
- 1280×720
- 1920×1080
- 3840×2160

Include sustained 30/60 FPS tests where hardware supports them.

## TODO-014 — Reduce per-frame allocation and copying

**Priority:** P2

- Profile before optimizing.
- Reuse conversion buffers.
- Add buffer pools where allocation overhead is measurable.
- Avoid redundant RGB ↔ BGR ↔ NV12 conversions.
- Permit direct NV12 submission to compatible encoders.
- Separate metadata handling from pixel memory ownership.
- Benchmark SIMD implementations against scalar reference.
- Preserve deterministic scalar fallback.

## TODO-015 — Prepare hardware-buffer integration

**Priority:** P2

- Introduce opaque hardware-buffer references.
- Define explicit map/unmap operations.
- Define synchronization/fence contracts.
- Support D3D11 resource handles through a Windows adapter.
- Evaluate Android `AHardwareBuffer`.
- Support CPU fallback.
- Avoid exposing platform handles in the generic frame API.
- Document cross-thread resource ownership.

---

# P2 — Android and WebRTC Readiness

## TODO-016 — Add Android capture/encode backend

**Priority:** P2

- Add Android arm64 CMake/NDK target.
- Define JNI adapter boundary.
- Integrate Camera2/CameraX buffer delivery through the Android layer.
- Investigate `AImageReader` native integration.
- Support YUV_420_888 layout handling.
- Handle plane pixel stride and row stride correctly.
- Integrate MediaCodec encoding.
- Support orientation and rotation.
- Handle camera permission loss.
- Handle app background/foreground transitions.
- Handle device disconnect/reinitialization.

Do not assume Android YUV_420_888 memory is contiguous NV12.

## TODO-017 — Separate WebRTC transport from media core

**Priority:** P2

- Document current signaling implementation.
- Inventory incomplete transport functionality.
- Define an encoder-frame output interface.
- Define an incoming decoded-frame interface.
- Keep WebRTC signaling/session logic outside the core.
- Integrate a mature WebRTC transport rather than creating a new RTP/ICE/DTLS stack.
- Define bitrate and congestion-control feedback integration.
- Define timestamp mapping.
- Test reconnect, network loss and stream cancellation.

**Acceptance criteria**

The media core can operate without WebRTC, while a WebRTC adapter can use its frames and codecs.

---

# P3 — API Quality and Documentation

## TODO-018 — Standardize errors and logging

- Audit inconsistent error return conventions.
- Introduce structured error categories.
- Preserve platform error codes where applicable.
- Avoid logging directly to stdout/stderr inside the library.
- Provide optional host logging callback.
- Document recoverable vs fatal errors.
- Ensure errors remain understandable across C++/JNI boundaries.

## TODO-019 — Public API cleanup

- Inventory exported headers and symbols.
- Minimize platform headers in public interfaces.
- Document ownership and lifetime for every buffer API.
- Document thread safety.
- Document callback invocation context.
- Use `noexcept` only where guaranteed.
- Audit C ABI requirements for JNI/plugin boundaries.
- Establish versioning and compatibility policy.

## TODO-020 — Documentation and examples

- Minimal camera capture example.
- Frame conversion example.
- Recording example.
- Detection pipeline example.
- Frame pump example.
- Android integration example.
- Backend/codec capability documentation.
- Build matrix and optional dependencies.
- Performance benchmark methodology.

---

# Implementation Order

| Phase            | Tasks         | Exit condition                                         |
| ---------------- | ------------- | ------------------------------------------------------ |
| 1 — Safety       | 001–004       | Frame and AVI paths pass boundary tests and sanitizers |
| 2 — Standalone   | 005, 010, 011 | Core builds and tests independently                    |
| 3 — Architecture | 006, 007, 009 | Stable ownership and module boundaries                 |
| 4 — Pipelines    | 008, 012      | Reusable bounded frame pump                            |
| 5 — Performance  | 013–015       | Benchmarked, measured improvements                     |
| 6 — Integration  | 016–017       | Android and WebRTC adapters                            |
| 7 — Polish       | 018–020       | Documented, consistent public API                      |

## Instructions for Implementation Agent

1. Start with **TODO-001 through TODO-004**. Do not combine safety fixes with architectural refactoring.
2. Inspect all call sites before changing any public signature or behavior.
3. Add a failing regression test for each confirmed defect before fixing it.
4. Compile and run the relevant tests after each task.
5. Preserve existing functionality and avoid introducing unrelated dependencies.
6. Do not silently change pixel formats, color-space conventions, timestamp semantics or buffer ownership.
7. Do not introduce a new media framework or graph engine without demonstrating that existing interfaces cannot satisfy the requirement.
8. For every task, record modified files, tests executed, benchmark impact if relevant, and remaining risks.
9. Mark a task complete only after its acceptance criteria have been verified.
10. If a reported defect cannot be reproduced, document the reason and close it with evidence rather than introducing speculative changes.

## Definition of Done

The stabilization effort is complete when:

- No known unchecked frame-buffer accesses remain in conversion paths.
- Pixel format, stride and capacity contracts are enforced.
- Recording failures and size limits are handled explicitly.
- Core builds without the Tanit source tree.
- Optional subsystems can be independently enabled/disabled.
- Test and sanitizer coverage runs in CI.
- Frame ownership and threading contracts are documented.
- Android frame delivery can reuse the same core.
- WebRTC is an optional integration rather than a core dependency.
- Representative 720p/1080p/4K performance baselines are recorded.

**Immediate next action:** none. The list below is the record of what was verified.

## Status

| Task | Result |
| --- | --- |
| 001, 002 | Fixed. `tests/test-convert.mjs` locks BT.601 and the boundary cases. |
| 003 | `rgb_frame_fits` guards `to_bgr24`, the encoder, and the screen trim. A short row is rejected. Screen blit and HBITMAP copies reject a stride that overflows `int`. Other capture loops were read and not changed: their writes stay inside buffers they just allocated. |
| 004 | `MjpegAviWriter` checks every write, rejects an empty JPEG, refuses a file whose RIFF size will not fit in 32 bits, saturates the bitrate field, and finalizes from the destructor. A second `finalize` is a no-op. `tests/test-library.mjs`. |
| 005 | `pm/video/text.hpp` replaces the commons path/string includes. `PM_VIDEO_LINK_COMMONS` defaults off. A package build with that option off is the standalone check. |
| 006, 007 | Core headers do not include detect or streaming. `VideoFrame` ownership is documented on the struct. No second frame type was forced through the capture path. |
| 008 | `FrameQueue` is the bounded queue. `IFramePump` stays the source. No graph engine. |
| 009 | Hook table and logging are already `set_log` / `hooks.cpp`, compiled once by the host. Start/stop stays on `VideoInput`. Not exercised against two live cameras. |
| 010 | Windows x64 standalone and Tanit `release-debug` are the builds that ran. `/W4` stays off. `PM_VIDEO_ASAN` exists and was not turned on. Linux, macOS, and Android were not configured. |
| 011 | `enable_testing` / `pm-video-cli` runs `node tests/run.mjs`. `.github/workflows/test.yml` runs the same on Windows. Sanitizer and fuzz jobs were not run. |
| 012 | `pm-video backends` reports compiled / unsupported / plugin. It does not open a device. |
| 013, 014 | `pm-video bench` times scale and NV12 up to 3840×2160. The scale destination is reused when the byte count matches. No SIMD change. |
| 015 | `HardwareBufferRef` plus `map`. The test maps a CPU stand-in. D3D11 and `AHardwareBuffer` are not wired. |
| 016 | `yuv420888_to_nv12` honors row stride and pixel stride. JNI, Camera2, and the NDK are not in this package. |
| 017 | WebRTC transport stays out of the core (`backends` reports `transport: unsupported`). Signaling remains the existing scaffold. |
| 018 | Library logs go through `set_log`. The virtual-camera elevation notice uses that hook. stderr is the fallback when the host installs nothing. |
| 019 | Thread and ownership notes are on `VideoFrame`, `MjpegAviWriter`, `VideoEncoder`, and the README. No mass `noexcept` change. |
| 020 | README examples: `backends`, `bench`, `yuv`, `queue`. |

Detect decoders and the streaming session were not edited. ByteTrack is still not compiled.

## Inventory

Call sites of `pm::vcam::scale_bgr24` and `pm::vcam::bgr24_to_nv12`:

| File | Symbol |
| --- | --- |
| `src/vcam/convert.cpp` | definitions |
| `src/vcam/sink.cpp` | `VcamSink::push` |
| `cli/main.cpp` | `cmd_nv12`, `cmd_convert` |

`src/cli/pm_image_cmd_video.cpp` and `packages/xblox/src/blocks/video_blocks.cpp` do not call either function. Their capture path uses `capture_still` and `save_frame`.

### Confirmed, and fixed

- `scale_bgr24` sized the destination with `size_t(dst_w) * dst_h * 3` before checking the sign. A negative height wraps and `vector::assign` throws `bad_alloc`.
- The packed copy took `begin() + width * height * 3` without checking `data.size()`. A short buffer was an out-of-bounds read.
- The scale loop trusted `stride`. A stride shorter than `width * 3`, or a buffer that does not cover the last pixel, read past the row.
- Any pixel format was scaled as BGR24.
- `bgr24_to_nv12` on an odd width wrote `uv[x + 1]` one byte past that UV row.
- The NV12 function had no source or destination capacity, so a short buffer could not be rejected.
- A source stride below `width * 3` read past the row.

Both functions now return `false` and leave no partial destination (`scale_bgr24` clears its vector; `bgr24_to_nv12` does not write). Scaling still emits tightly packed BGR24. The color conversion is unchanged: BT.601 limited range. A pure red pixel stays Y=82, U=90, V=240. The 2×2 quad stays luma `[82, 144, 41, 235]` with neutral chroma.

`npm test` in `packages/video.cpp` covers the boundary cases in `tests/test-convert.mjs`.

Verified after the rest of the list: standalone configure with `PM_VIDEO_LINK_COMMONS=OFF`, `npm test` (96 checks, including `tests/test-library.mjs`), and repo-root `npm run build:cpp-fast` (links `pm-video-lib`, `tanit.exe`, `tanit-cli.exe`).

### Left alone on purpose

- Capture loops in `capture_win.cpp` and the filter passes allocate their own destination and were not rewritten.
- No cmake split of `pm-video-lib`. `/W4` and `PM_VIDEO_ASAN` stay off. Linux, macOS, and Android were not configured. No 4K timing number was recorded.
- D3D11 and `AHardwareBuffer` are not mapped. WebRTC transport was not added. Detect decoders and the streaming session were not edited.