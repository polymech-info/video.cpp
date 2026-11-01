**Use the libmpv dev package (headers + `libmpv-2.dll` + import lib). Do not compile the mpv player, and do not build mpv from source unless you need an LGPL or custom binary.**

The [Windows compile guide](https://github.com/mpv-player/mpv/blob/master/DOCS/compile-windows.md) is for producing `mpv.exe` / `libmpv` from Meson. Embedding in pixlwiz only needs the **client library** already described in `client.h`: `mpv_create()` plus commands/properties/events. The headers you have are that API (v2.5). The player binary is a different product.

---

### What you already have

`third_party/mpv/mpv-dev/` is an incomplete extract of a shinchiro-style **mpv-dev** tree:

| Present | Missing (the actual runtime) |
|---|---|
| `include/mpv/client.h`, `render.h`, `render_gl.h`, `stream_cb.h` | `libmpv-2.dll` |
| updater / installer leftovers, `fonts.conf`, manual | `mpv.lib` (MSVC import lib) |

`packages/mpv/README.md` is the upstream **player** README, not a packaging plan for this app.

Official prebuilts: [shinchiro/mpv-winbuild-cmake releases](https://github.com/shinchiro/mpv-winbuild-cmake/releases) — grab `mpv-dev-x86_64-*.7z` (not the player archive, not the `v3`/AVX2 variant for a shipped app). Same files are mirrored under [SourceForge libmpv](https://sourceforge.net/projects/mpv-player-windows/files/libmpv/).

---

### Why not compile “the whole thing”

1. **MSVC cannot build mpv.** The compile doc is explicit: `cl.exe` is unsupported. You consume a Clang/MinGW-built DLL and link with `mpv.lib` (or generate one with `lib /def:libmpv-2.def`). That matches this Win32xx / MSVC host.
2. A from-source Windows build means Meson + FFmpeg + libplacebo + libass + NASM + (often) Rust nightly. Weeks of toolchain, not a viewer feature.
3. You do not want `mpv.exe` in the process. You want a child surface inside `CFileViewer`.

Compile from source only if you need `-Dgpl=false` (LGPLv2.1 instead of default GPLv2+) or a stripped feature set. The C API is ISC; **the DLL you ship is still GPL unless built with `-Dgpl=false`**. A plugin that `LoadLibrary`s a user-supplied `libmpv-2.dll` is the usual way to keep that optional.

---

### How video works today (no mpv)

`CMainFrame` / `CViewerManager` do not pick a video engine. They only host `CFileViewer`. Routing is already:

```
OpenFile → centre_preview_kind_for_path_ws == 7 → LoadVideo
```

`LoadVideo` is WebView2 + viewer-next `<video>` (`FEATURE_VIEWER_WEB`). Same pattern as PDF/audio. Browser codecs only; `is_video_ext` is already broader than what Chromium plays well (mkv/avi/wmv/mpeg/ts).

`CViewerManager` is tab/primary indirection, not a plugin registry. `SetPreferredViewerKind` is a one-shot web-kind hint (`editor` vs `text`), not a native-engine switch.

`TeardownPreviewSurfaces` already destroys the WebView2 host so media dies when the centre view leaves. An mpv HWND would need the same destroy path.

---

### How this fits `plugin_manager`

There is **no viewer/mpv capability** today. Categories are onnx / local-ai / vision / audio / extensions. PDF and editor are the closest analogs:

| | PDF plugin | Editor plugin | mpv (proposed) |
|---|---|---|---|
| Role | search / rasterize (`PmPdfApi`) | Scintilla HWND (`PmEditorApi`) | playback HWND |
| FileViewer preview | still WebView2 | native editor is a separate `FEATURE_NATIVE_TEXT_EDITOR` path | **this** would be the FileViewer video surface |
| Host lookup | `find_component(guid)` → `task_api` | same (`host_editor.cpp`) | same |

The ABI is C-only: GUID + `PmXxxApi` vtable + optional `pm_plugin_probe_runtime`. Whisper-style wrap of a third-party native lib is the right template: **tanit plugin links or `LoadLibrary`s `libmpv-2.dll`; the exe never links mpv.**

`probe_runtime` should fail with `RuntimeMissing` when `libmpv-2.dll` is absent, so `LoadVideo` can fall back to WebView2.

`Mainfrm.h` does not need a new viewer type. The switch belongs in `CFileViewer::LoadVideo` (and teardown), the same way native editor vs web text already works.

---

### Embedding: `wid` vs render API

`client.h` offers two ways:

1. **`wid` (HWND)** — set the `wid` option to a child of `CFileViewer`. Matches Win32xx. Input/focus can be awkward; fine for a preview pane. Right first path.
2. **`render.h` OpenGL / software** — you own the GL context and call `mpv_render_context_render()`. mpv prefers this for toolkit embedding. More work; FileViewer has no GL surface today.

Do not bring `mpv.exe` or IPC. The in-process client API is the embed path.

A thin `PmMpvApi` is enough: `available` / `create(HWND)` / `load(utf8 path)` / `command` / `resize` / `destroy`. FileViewer keeps play/pause/seek as `mpv_command` strings so the plugin does not grow a full player UI.

---

### Recommended shape (when you implement)

1. Drop a real `mpv-dev-x86_64` package next to the headers: `libmpv-2.dll` + `mpv.lib`. Prefer `GetProcAddress` / delay-load so the exe stays optional.
2. New plugin: `tanit_mpv_plugin` + `plugin_mpv_api.h` + `PM_GUID_VIEWER_MPV` + seed in `plugin_manager.cpp` + `host_mpv.cpp`.
3. `LoadVideo`: if `find_component` is Ready → native child HWND + libmpv; else keep WebView2.
4. Hook destroy into `TeardownPreviewSurfaces` / `ResetAllPreviewSurfaces` so centre-chat does not leave audio running.
5. Keep `is_video_ext` / kind `7` unchanged. mpv can later widen audio the same way (`LoadAudio`).

**Bottom line:** ship/consume **libmpv-dev**, wrap it like whisper/editor, and treat WebView2 as the fallback. Compiling mpv is only justified for license (`-Dgpl=false`) or a custom feature set — not for getting video into `CFileViewer`.

You already have the right artifact. Last pass missed the binaries because the workspace indexer skips them (`.gitignore` also drops `*.lib`). On disk, `third_party/mpv/mpv-dev/` is a [shinchiro 20260829](https://github.com/shinchiro/mpv-winbuild-cmake/releases/tag/20260829) x64 tree, not headers-only.

| File | What it is | For pixlwiz |
|---|---|---|
| `include/mpv/*.h` | Client / render API | Compile against these |
| `libmpv-2.dll` (114.8 MB, PE x64) | Embeddable player | **This is the runtime** |
| `libmpv.dll.a` | MinGW import lib | Do **not** link from MSVC |
| `d3dcompiler_43.dll` | D3D shader sidecar | Ship next to `libmpv-2.dll` |
| `mpv.exe` / `mpv.com` | CLI player | Ignore — not used in-process |
| installer / `updater.bat` | Shell association | Ignore |

No need to compile mpv. `libmpv-2.dll` is the whole decoder/renderer.

The one MSVC gap: there is no `mpv.lib`. Do not consume `libmpv.dll.a`. Match whisper/onnx: `LoadLibrary` + `GetProcAddress` (or generate `mpv.lib` from a `.def` later). `pm_plugin_probe_runtime` should probe `libmpv-2.dll` the same way whisper probes `ggml.dll`.

Ship `libmpv-2.dll` + `d3dcompiler_43.dll` beside the plugin (or on the exe search path). Leave `mpv.exe` out of the app install.

Yes — a GL island is still much smaller than a WebView2 surface. The GL objects themselves are tiny. The cost is `libmpv-2.dll` (~115 MB mapped) plus a decode/GPU path, not Chromium.

pixlwiz has **no OpenGL today**. GPU code is D3D11 (`CNodesView`, session recorder, `skin_gpu`). That does not give mpv a GL context. `render_gl.h` is also explicit: libmpv does **not** load OpenGL for you. You hand it `get_proc_address` and a current context.

---

### What a GL surface actually is here

A child HWND with a WGL (or EGL/ANGLE) context. mpv draws into FBO 0 (the default framebuffer) and you `SwapBuffers`. No GLEW/GLAD unless you draw your own chrome.

**WGL (desktop GL) — enough for preview, ~80–150 lines:**

1. Child `CWnd` of `CFileViewer` (`WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN`).
2. `GetDC` → `PIXELFORMATDESCRIPTOR` (RGBA32, double-buffer, `PFD_SUPPORT_OPENGL`) → `SetPixelFormat`.
3. `wglCreateContext` (or `wglCreateContextAttribsARB` for 3.2 core; mpv wants GL 2.1+ / preferably 3.2).
4. `wglMakeCurrent` on the thread that will call `mpv_render_*`.
5. `mpv_opengl_init_params`:
   - `get_proc_address` = `wglGetProcAddress`, with fallback `GetProcAddress(LoadLibrary("opengl32.dll"))` for 1.1 symbols WGL does not return.
6. `mpv_render_context_create` with `MPV_RENDER_API_TYPE_OPENGL` + those init params. Context must already be current.
7. Render loop (prefer a dedicated thread; do not call other libmpv APIs on it):
   - `mpv_render_context_set_update_callback` → make current → `mpv_render_context_render` with `mpv_opengl_fbo{0, w, h}` + `MPV_RENDER_PARAM_FLIP_Y = 1` (default FB is inverted) → `SwapBuffers`.
8. On resize: update `w/h`. On teardown: `mpv_render_context_free` while current, then `wglDeleteContext`.

Link: `opengl32.lib` (system). No extra DLL for the surface.

**ANGLE (GLES over D3D11) — only if you want zero-copy Windows hwdec via this API:**

`render_gl.h` says Windows hwdec through the render API wants ANGLE (not desktop WGL). That means `libEGL.dll` + `libGLESv2.dll` (~5–15 MB), `eglGetDisplay` / `eglCreateWindowSurface` / `eglCreateContext` / `eglGetProcAddress`. You already have `d3dcompiler_43.dll` in the mpv-dev tree. You cannot reuse WebView2’s private ANGLE.

---

### Footprint vs WebView2

| | Extra disk | Extra process | Host code |
|---|---|---|---|
| WebView2 `<video>` | already in the app (viewer-next + Edge runtime) | Chromium GPU + utility; `CViewerWebPanel` stays alive across kinds | already paid |
| libmpv + **WGL** | `libmpv-2.dll` + `d3dcompiler_43.dll` | in-process; one GL context | child HWND + WGL + render thread |
| libmpv + **ANGLE** | same + ANGLE DLLs | in-process | EGL setup |
| libmpv + **`wid`** | same DLLs | in-process; mpv owns a D3D11/GL window | HWND only |

The GL surface is kilobytes. WebView2 for video is a Chromium compositor, JS bundle, virtual-host mapping, and a media pipeline — even when the panel is reused. If you **replace** the video surface with mpv and tear down `CViewerWebPanel` for that open, you drop that Chromium controller. Chat/settings WebViews stay; those are separate.

If the user then opens a PDF, WebView2 comes back. Savings are largest on long video sessions, not on mixed browsing.

WGL without ANGLE still decodes (often CPU or GPU-copy-back). Fine for preview. `wid` gets mpv’s own Windows VO (usually D3D11) and hwdec **without** you providing GL at all — smaller host, slightly worse toolkit behavior.

---

### Practical take

- **Footprint:** GL (or `wid`) beats spinning WebView2 *for video*. The 115 MB DLL is the price; the context is not.
- **To get a GL surface:** child HWND + WGL as above. Copy the *idea* of `CNodesView` (child render HWND + device + resize), but use WGL, not D3D11 — mpv’s OpenGL backend will not accept a D3D device.
- **Skip ANGLE** unless hwdec-via-render-API is a requirement. Skip software `MPV_RENDER_API_TYPE_SW` for realtime HD.

Closest official walkthrough: [mpv-examples libmpv](https://github.com/mpv-player/mpv-examples/tree/master/libmpv) (SDL/GLFW do exactly this `get_proc_address` + FBO 0 + flip-Y loop).