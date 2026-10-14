# BenchMark

BenchMark is a standalone Android Vulkan benchmark app with its own identity
and UI; it does not replace or modify the system Vulkan driver.

## What it tests

- A continuously animated Vulkan preview rendered into a `TextureView`-backed
  Android Vulkan swapchain, including while the app is idle.
- Selectable instanced cube, rotating triangle, and low-poly animated dolphin
  scenes. The same Vulkan shaders and selected scene are used by the run.
- A draw-stress suite covering 1,024–2,048 rotating triangle instances; all
  phases keep at least 1,024 instances active and render a distinct triangle
  field rather than the cube scene.
- A BC1 CPU software-decompression throughput suite, displayed alongside an
  animated selector mosaic, distinct from the cube renderer. It reports CPU
  decoder MPix/s and is explicitly not a measurement of GPU/driver BC hardware
  or driver-specific emulation.
- A BC2–BC7 CPU reference-decoder suite covering BC2/3 unorm, BC4/5
  unorm/snorm, BC6H unsigned/signed float, and BC7 unorm/sRGB decode paths. It reports
  per-format throughput and checksums; this measures the app's CPU decoder
  workload, not GPU compressed-texture sampling.
- A dedicated BC2–BC7 Vulkan preview: a live grid of animated 4×4 selector
  patterns arranged into ten color-coded bands, one for each decoder format.
  It renders quads instead of cubes. The preview visualizes selector-pattern
  activity; it does not display pixels produced by the separate CPU decoders.
- A Vulkan tessellation pipeline suite that tessellates a circular, shaded
  dome with a soft projected circular shadow. Drag the live preview to rotate
  the active scene, including during a benchmark; **Reset View** restores the
  default orientation. The gesture angle is sampled by the native renderer on
  subsequent frames. It requires the device's core
  `tessellationShader` feature and reports an explicit failure if unavailable.
  The shadow is geometry rendered by the tessellation-evaluation shader, not a
  shadow-map workload.
- Nine driver-development profiles with independent live scenes and explicit
  native `TEST_PROFILE` log records:
  - Depth / Occlusion: overlapping rotating depth layers with Vulkan depth
    testing and writes enabled.
  - Alpha Blend: animated translucent triangles using a real
    `SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA` blend pipeline and depth writes disabled.
  - Fragment Shader Stress: animated overlapping full-screen quads with depth
    testing disabled and a repeated trigonometric fragment workload.
  - Triangle Raster Stress: 1,024–4,096 animated micro-triangles for primitive
    setup and raster coverage.
  - Vertex Transform Stress: dense animated instances run multiple
    trigonometric and multi-axis transform operations per vertex.
  - Fragment Discard Stress: animated full-screen overdraw layers exercise
    fragment coverage and conditional discard with depth testing disabled.
  - Blend Overdraw Stress: translucent full-screen layers use Vulkan
    source-alpha blending with depth writes disabled.
  - Geometry Test: 128–2,048 animated triangles stress vertex processing,
    primitive assembly, and rasterization in a single instanced draw.
  - Large Draw Test: 4,096–65,536 triangles are submitted in one
    `vkCmdDraw` call, with the vertex count and draw-call count recorded.
- A repeatable warm-up, two scene-load ramps, and a sustained-load phase.
- Per-second FPS from successful `vkQueuePresentKHR` returns, successful frame
  count, average present-call FPS, p95 present-call interval, and approximate
  1% low FPS. Swapchain recreation or failed/out-of-date presents do not count
  as rendered frames.
- ICD load errors, GPU/device/API identity, swapchain details, depth format,
  Vulkan device extensions, selected feature/limit properties, BC2–BC7 sampled
  format support, and optional-extension diagnostics in the native log and
  categorized Vulkan Info tabs. The tabs separate overview, instance, device,
  features, texture formats, and D3D runtime status. Device details use a
  one-time snapshot from the idle preview; ongoing FPS/log polling is restricted
  to active benchmark runs.
- An optional detailed-debug switch persists between launches and adds
  per-extension, per-feature, per-format, memory-heap, and preview-heartbeat
  diagnostics. Core failures and capability summaries remain logged when it is
  off.
- Preview reinitialization no longer resets the Android window buffer geometry
  during Vulkan shutdown; render failures and preview stop reasons are recorded
  so a stalled scene can be diagnosed.
- Full instance-extension and layer inventories, all Vulkan 1.0 core feature
  bits, counts for feature-extension structures queried by this app, selected
  device limits and memory heaps, and tiling/usage feature masks for common
  color, depth, ETC2, ASTC, and BC formats.
- JSON report save/share through Android's document picker and share sheet. Reports include device
  and driver identity, imported-driver SHA-256, test settings, native result,
  and the complete native log.
- Raw Vulkan log export as a `.txt` file through Android's document picker.
- Persistent in-run telemetry and a Stop Run control remain available while
  scrolling through the settings and log.
- Driver selection and debug logging appear below benchmark/report controls,
  immediately above the Vulkan log. Suite selection is followed immediately
  by its workload/duration controls and the Start Benchmark action.

The renderer prefers immediate presentation (then mailbox) when the driver
supports it, and falls back to Android's required FIFO mode otherwise. The
selected mode is included in the native log. Each rendered frame's shader time
is sampled from the same monotonic clock used for elapsed time; the live log
ties that exact `render_time_s` to successful present-call FPS and the latest
present-call interval. FPS therefore tracks frames accepted by the Vulkan
present call, not a separate UI timer. It is not a claim about physical panel
scanout: imported ICDs do not all expose display-timing feedback, and this app
reports that limitation rather than guessing. In FIFO mode frame cadence
typically follows display refresh; immediate/mailbox can report a faster
present-call rate than the panel can visibly display. Android 11+ is asked to
use the display's highest reported refresh mode for this render surface.

The current suites focus on geometry, rasterization, CPU-side BC decoding,
tessellation, frame pacing, and sustained rendering. They do not yet provide
compute or memory-bandwidth tests, and they should not be treated as a game-FPS
predictor. This build does not include a shadow-map workload or DXVK/vkd3d-proton
runtimes, a Wine/Windows execution layer, or D3D test executables. Therefore it
does not yet run animated Direct3D 11/DXVK or Direct3D 12/vkd3d workloads; the
Vulkan Info tab reports both compatibility results as **NOT TESTED** rather
than inferring compatibility from Vulkan support or runtime-library presence.
Actual D3D animation testing requires packaging compatible runtimes and an
execution bridge, then validating them on an Android device.

## Imported ICDs

Use **Import ICD** and select the driver `.so`. When testing PanVK, select
`libvulkan_panfrost.so`; this build packages the matching G925 `libdrm.so` and
Android `libc++` runtime, so those companion files do not need to be imported.
The app also accepts optional companions selected alongside the ICD. It copies
selected ELF64 little-endian AArch64 files into private storage, checks their
architecture, seals them read-only, and stores each bundle under a content-
derived private path. This keeps newly imported drivers and companion
libraries distinct from code already mapped by Android's linker, allowing the
preview to restart against a different imported ICD without overwriting mapped
files. Companions are preloaded from the selected bundle before falling back to
the bundled libraries. The system-driver route does not preload these custom
dependencies.

The imported ICD runs in the benchmark app's process. A broken or incompatible
driver can crash the app; it cannot overwrite the system driver, but the app
process itself is not isolated from an ICD crash.
Loaded ICDs and companion libraries remain mapped until the app process exits
because drivers may register thread-local destructors. Previously imported
bundle files are consequently retained; importing a different bundle selects
its distinct path, and the preview is restarted to load that version.

## Build

The Android SDK, NDK 29, CMake 3.22.1, Gradle wrapper, and JDK 21 must be
available. `local.properties` is local-only and ignored by Git. Set its
`sdk.dir` and `ndk.dir` for the machine, or configure the SDK/NDK through
Android Studio.

```sh
./gradlew assembleDebug
```

By default, dependency packaging uses
`dist/android-g925-v13-csf/libdrm.so` and the NDK 29 AArch64
`libc++_shared.so` found from `local.properties`'s `ndk.dir` (or the sibling
`android-ndk-r29` directory). Override either source with
`-PpanvkDrm=<path>` or `-PpanvkCxx=<path>`.

The debug APK is written to
`app/build/outputs/apk/debug/app-debug.apk` with package name
`dev.panvk.bench`. The app targets `arm64-v8a`, Android 10 or newer.

## Regenerate embedded shaders

The native build consumes the checked-in `scene_spv.h`, so the Android build
does not need a shader compiler. To update it after editing GLSL:

```sh
glslangValidator -V app/src/main/cpp/scene.vert -o /tmp/scene.vert.spv
glslangValidator -V app/src/main/cpp/scene.frag -o /tmp/scene.frag.spv
glslangValidator -V app/src/main/cpp/scene.tesc -o /tmp/scene.tesc.spv
glslangValidator -V app/src/main/cpp/scene.tese -o /tmp/scene.tese.spv
python3 tools/embed_spirv.py \
  --vertex /tmp/scene.vert.spv \
  --fragment /tmp/scene.frag.spv \
  --tess-control /tmp/scene.tesc.spv \
  --tess-evaluation /tmp/scene.tese.spv \
  --output app/src/main/cpp/scene_spv.h
```
