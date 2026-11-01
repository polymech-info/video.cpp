# MediaPipe graphs → XBlox — parked

**Status: parked.** Do not implement. Do not embed `CalculatorGraph`.
Do not add MediaPipe calculator names as XBlox kinds.

This is the **composition model**, not the face/beauty pipe. Forehead
oval seams, `kFaceOval`, and `BeautyProcessor` stay in
[filters-next.md](./filters-next.md). Inference stays Tasks C ABI
(`pm_face_*` / `pm_seg_*`) + BGR24.

Named pipes (named streams / `blockOutput` slots) are how XBlox
overcomes `PREVIOUS` linearity. That is already the direction in
[xblox-schemas.md](../xblox/xblox-schemas.md). This note records that
the MediaPipe CalculatorGraph is the same idea, and that users could
compose new vision graphs in XBlox without running MediaPipe’s
runtime.

Related: [filters-next.md](./filters-next.md),
[video-next.md](./video-next.md),
[xblox-schemas.md](../xblox/xblox-schemas.md).

Reference graph (Coral desktop live face **detect**, not landmarker):

`packages/mediapipe-tasks/checkout/mediapipe/examples/coral/graphs/face_detection_desktop_live.pbtxt`

Sibling: `object_detection_desktop_live.pbtxt` (same skeleton, 300×300).

---

## Confirmation

**Yes.** The MediaPipe *graph model* maps onto XBlox. Users can compose
new graphs once data moves on **named pipes**, not `PREVIOUS`.

“Whole MediaPipe” here means the **composition rules**, not the
checkout. Users compose **XBlox graphs that look like MediaPipe
graphs**. They do not run MediaPipe’s `.pbtxt`.

| Layer | Verdict |
|-------|---------|
| IR + UX (pins, buses, subgraphs) | Maps. Named pipes + typed ports + nested documents. |
| Runtime for discrete XBlox | Maps. Files, LLM, detect-once already have producers/consumers. |
| Live video as a user graph | Maps the shape. Needs a back-edge / drop-while-busy pipe (`FINISHED`) and frames as first-class slot types. Scheduler feature, not a new `ParamKind`. |
| Import CalculatorGraph / 400 calculators | **No.** |

---

## Stream names are wires, not functions

`transformed_input_video` is **not** a framework function. It is a
**named wire**. The function is
`ImageTransformationCalculator::Process()`. The `.pbtxt` only says
which **pin** publishes onto which **bus**.

```
output_stream: "IMAGE:transformed_input_video"
```

Read it as `TAG:stream`.

| Piece | Role |
|-------|------|
| `IMAGE` | pin on the calculator (`GetContract` declares `ImageFrame`) |
| `transformed_input_video` | graph-local bus name, like an XBlox `storeAs` / output slot |

The next node does `input_stream: "IMAGE:transformed_input_video"`:
bind **my** `IMAGE` pin to that same bus. Same payload, new socket
name. `LETTERBOX_PADDING:letterbox_padding` is a second pin on the
same node — one block, two typed outputs.

`GetContract` = XBlox manifest (`params` + `outputs`). The `.pbtxt` =
one document instance. The scheduler matches timestamps and calls
`Process()`.

---

## Two ways they compose new graphs

**1. Text graph (Coral demo)** — string buses. Fork the file, rename a
stream, splice a node.

**2. Builder API (Tasks)** — same wires, held by the compiler:

```
auto& image_transformation = graph.AddNode("ImageTransformationCalculator");
image_cpu_or_gpu >> image_transformation.In(IMAGE);
auto transformed_image = image_transformation.Out(IMAGE);
transformed_image >> tensor_converter.In(IMAGE);
```

`transformed_image` is a typed `Source<T>` handle. Same wire as
`transformed_input_video`. Then they wrap the chain as a **subgraph**
(`ImagePreprocessingGraph`, `ImageSegmenterGraph`) and drop that in as
one node. That is the “composing new graphs” move.

MediaPipe Tasks (`FaceDetector` / `FaceLandmarker` / `ImageSegmenter`)
**collapsed** graphs like the Coral demo into one C API. The checkout
still has the expanded form — that is why it feels like XBlox. Pixlwiz
does not run these `.pbtxt` files.

---

## Map to XBlox

| MediaPipe | XBlox today |
|-----------|-------------|
| `input_stream` / `output_stream` | document in/out ports |
| `node { calculator: "Foo" }` | block `kind` + params |
| `TAG:stream` | named pipe = output slot bound to an input |
| `output_stream: "IMAGE:foo"` | `outputs: [{ slot: "image", kind: "image" }]` |
| next `input_stream: "IMAGE:foo"` | `bindings.input = { kind: "blockOutput", blockId, output: "image" }` |
| `>>` builder | wiring edge `blockOutput` |
| `PREVIOUS` | MP’s implicit “only one unlabeled stream” |
| subgraph node | a `kind` that is itself a graph (nested document) |
| `GetContract` types | manifest ports + `ParamKind` / `supportedKinds` |
| `options { output_width: 128 }` | block params |
| side packet (anchors, once) | scope variable, written once |
| back edge `FINISHED` | **not yet** |

Linear `roots[]` + `${transformed}` is the pbtxt-string style. The
wiring view (`blockOutput` / ports / type mismatch) is the builder-API
style. Same composition model.

`docs/xblox/xblox-schemas.md` already decided this: drop implicit
`PREVIOUS` as the result bus; persist `blockId` + slot. That *is* a
CalculatorGraph, with XBlox names.

Linearity is only a **document order** problem. MediaPipe never used
order as identity; the stream name is the identity. Named pipes do the
same: one producer can fan out, one consumer can pull two buses
(`image` + `pad`), a later overlay can ignore everything in between.
`PREVIOUS` cannot do that without lying.

---

## Coral demo as an XBlox-shaped document

Not Face Landmarker. Coral Edge TPU **face detection**: box + 6
keypoints (eyes, nose, mouth, ears). Mid-forehead oval / 468 mesh
never appear here.

```
input_video
    │
    ▼
FlowLimiter  ◄──── FINISHED:detections   (drop frames while busy)
    │
    ▼
Resize 128×128 FIT + letterbox pad
    │
    ▼
Image → quantized TfLite tensor
    │
    ▼
Infer  face-detector-quantized_edgetpu.tflite
    │
    ▼
SSD decode (896 boxes, 6 keypoints, score ≥ 0.75)
    │         ▲
    │    anchors (side packet, built once)
    ▼
NMS (IoU 0.3, weighted)
    │
    ▼
Un-letterbox (map 128² coords back to the real frame)
    │
    ▼
Draw boxes
    │
    ▼
Overlay on the *throttled* frame → output_video
```

Closest shipped cousins: `video-detect-yolo26` / `video-detect-pose`.
Same skeleton (frame → model → boxes/keypoints → draw), different
backend (ONNX vs Edge TPU TfLite).

`videoCapture` + beauty is **not** this graph. Beauty is Face
Landmarker (detect → crop → 468 mesh) plus our own pixel ops. Tasks
hid that calculator list behind `pm_face_*`.

As named pipes (legal XBlox **if** those kinds exist as host blocks
with typed ports):

```
videoIn.frame ──IMAGE──► resize128.image ──IMAGE──► toTensor.image
resize128.pad ──PAD───► unletterbox.pad
toTensor.tensors ──────► infer.tensors ──► decode.tensors ──► nms ──► overlay
videoIn.frame ───────────────────────────────────────────────► overlay.image
```

No MediaPipe scheduler required for that shape.

### The one live-video node we do not have

`FlowLimiterCalculator` with `FINISHED:detections` as a **back edge**:

- First frame goes through.
- Nothing else passes until detections finish.
- Frames that arrive while the TPU is busy are **dropped**.

That is live-video backpressure. Same reason the meeting path cares
about “don’t queue 30 frames behind the mesh.” WGC screen capture
stays fast; beauty on the PiP is the slow branch. MediaPipe makes that
policy a wire, not a comment.

XBlox wiring has `execution` edges; they do not yet mean “this output
ACKs that input and drops the rest.” For a meeting cam that is the
important missing piece, not letterbox.

---

## Steal vs do not import

**Steal the IR:** named streams, tagged ports, optional second
outputs, subgraphs as one block. That is how someone builds
`throttle → letterbox → infer → NMS → draw` without a new `ParamKind`.

**Do not import CalculatorGraph.** [filters-next.md](./filters-next.md)
already parked that: Packets, `ImageFrame` / `GpuBuffer`, protobuf
options, timestamp alignment, GPU EGL. Beauty is Tasks C ABI + BGR24.
Mapping `ImageTransformationCalculator` 1:1 would pull OpenCV/GPU
contracts we refused.

**Do not expose MediaPipe calculator names as XBlox kinds.** Map
*jobs*: `videoResize`, `videoInfer`, `videoNms`, `videoOverlay`.
Behind `videoInfer` we already have ONNX (YOLO) and the face DLL. The
graph author should not see `TfLiteConverterCalculator`.

**Do not expect this graph to grow a hairline.** The Coral detector
never claims skin. A box around the face still ends below the
hairline. Oval grow / feather is a beauty-mask job, not a graph-IR
job.

---

## When to unpark

Unpark only if all of these are true:

1. XBlox documents persist explicit `blockOutput` bindings (named
   pipes), not implicit `PREVIOUS`, as in
   [xblox-schemas.md](../xblox/xblox-schemas.md).
2. Video blocks publish typed slots (`frame`, `detections`, `pad`)
   instead of a single consumed handle.
3. Sinks peek; only filters consume+emit (already noted in
   filters-next “Sinks / realtime”).
4. Someone needs a **user-authored** vision DAG beyond
   `videoBeautify` + YOLO. Until then, keep the collapsed Tasks /
   ONNX kinds.

Still out when unparked:

- Bazel `CalculatorGraph` inside pixlwiz
- EGL / GPU MediaPipe graphs
- New `ParamKind` for pipes
- Face-mesh calculator expansion (Tasks stays one call)
