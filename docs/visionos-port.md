# WiiCompiled VR on Apple Vision Pro (visionOS)

This document is the design and build reference for the Apple Vision Pro build.
It complements `OPENXR.md`, which remains the specification for the
presentation policy, the virtual screen, the immersive window, the first-person
camera and frame interpolation: all of that is shared, unchanged, with the
Windows and Quest products. What differs is everything below the stereo replay:
the headset runtime (visionOS has no OpenXR), the graphics binding (Metal), the
input (hands, no controllers) and the app shell (SwiftUI, a static library).

Status: **runs on an Apple Vision Pro.** On 2026-09-26 the base game booted on
a headset running visionOS 26.6.1, reached the menus on the virtual screen with
the hand-driven pointer, and entered a race with the immersive stereo replay
submitted as a projection layer at 1888 x 1792 per eye; the same build runs in
the visionOS 27 simulator (head tracking, no hands). Comfort, frame timing and
input tuning on hardware are the open work. See
[Validation status](#validation-status).

## Sources of the design

- **KartPad** showed that
  the translated game runs on the iOS family: Aurora on Dawn/Metal, SDL3, a
  static library inside an app bundle. Its lessons carried over: the guest's 4
  GiB flat address space has to sit low (16 GiB) because the iOS family's
  virtual address space is small; `/tmp` is not writable (use `TMPDIR`);
  Dawn's `ios-arm64` prebuilt cannot be reused for another Apple platform, so
  Dawn is built from source for the SDK at hand (`scripts/build-dawn-ios-simulator.sh`
  there, `visionos/Build-VisionOSDawn.sh` here); the app exports its
  directories to the runtime through the environment.
- **Dolphin iOS**  informed the shape of the
  interface: a small native launcher, game data through the Files app, the
  emulator core on a thread of its own with UIKit never blocked by it.
- **Apple's CompositorServices and ARKit C APIs** supply what OpenXR runtimes
  supply elsewhere: frame pacing and prediction (`cp_frame_*`), the eye
  drawables and their view transforms and projections, the device pose
  (`ar_world_tracking_provider`) and the hand skeletons
  (`ar_hand_tracking_provider`).

## Architecture

### One runtime, a third graphics binding

`runtime/src/vr/openxr_integration.cpp` owns the pacing thread, policy
evaluation, the retained-layer protocol and the head-pose maths, written against
the backend-neutral vocabulary in `runtime/include/vr/openxr_backend.h`. It
selects one backend class at compile time:

| Platform | Backend | Binding |
| --- | --- | --- |
| Windows | `OpenXRD3D12Backend` (`openxr_d3d12.cpp`) | Dawn's D3D12 device; eyes copied on its queue into the XR images. |
| Android | `OpenXRVulkanBackend` (`openxr_vulkan.cpp`) | A second Vulkan device from the runtime; Dawn and it meet on `AHardwareBuffer`s. |
| visionOS | `OpenXRMetalBackend` (`openxr_metal.mm`) | Dawn's Metal device; the XR images are `IOSurface`s Dawn imports as shared texture memory. |

The Metal backend follows the D3D12 one step for step: Aurora renders straight
into the acquired swapchain images (no intermediate copy), a second pair of
swapchains holds the last submitted frame for retained resubmission, the
settings panel gets its own quad layer, and the virtual screen crops the eye
image to the drawn area so the room frames the picture.

### The OpenXR provider

visionOS ships no OpenXR runtime and no loader. Rather than rewrite the pacing
thread, the controller actions and the swapchain protocol against Apple's APIs,
`runtime/src/vr/visionos/` implements the OpenXR 1.0 entry points those call
(the ~45 `xr*` functions the runtime uses, plus `XR_KHR_convert_timespec_time`
and `XR_FB_display_refresh_rate`) on top of CompositorServices and ARKit. It
links in place of the Khronos loader as `mkw_openxr_visionos`
(`runtime/CMakeLists.txt`); the Khronos headers are fetched, nothing else of
the SDK is needed.

| OpenXR | visionOS |
| --- | --- |
| Instance, system, `xrEnumerateViewConfigurationViews` | The `cp_layer_renderer_t` the app hands over (`xr_visionos_set_layer_renderer`); eye sizes from the first drawable's view texture map, 1920 x 1824 until one is seen. |
| Session states | `cp_layer_renderer_get_state`: paused → `READY`/`SYNCHRONIZED` (should-render off), running → `VISIBLE`, `FOCUSED`, invalidated → `STOPPING`, `EXITING`. |
| `xrWaitFrame` | `cp_layer_renderer_query_next_frame` + `cp_frame_predict_timing`; the predicted display time is the frame's presentation time, the period its measured cadence (11.1 ms until measured). |
| `xrBeginFrame` | `cp_frame_end_update`, `cp_time_wait_until(optimal_input_time)`, `cp_frame_start_submission`, the drawable. |
| `xrLocateViews` | `cp_view_get_transform` (device-from-view) composed with the ARKit device anchor at the display time; the FOV from `cp_drawable_compute_projection`, which the runtime turns back into its own projection. |
| `xrEndFrame` | The provider *composites*: projection layers are drawn as full-view textured quads at a constant depth per eye, quad layers as world-placed quads; depth is written so the compositor can reproject; `cp_drawable_encode_present`, `cp_frame_end_submission`. |
| Swapchains | Triple-buffered `IOSurface`s with an `MTLTexture` each (`XrSwapchainImageMetalMKW` in `runtime/include/vr/visionos/xr_visionos.h`). |
| Reference spaces | `LOCAL` and `STAGE` are the ARKit world origin, `VIEW` the device anchor. |
| Actions | `xr_visionos_input.mm`: hand-tracking gestures. |
| Time | `XrTime` is nanoseconds on the `mach_absolute_time` clock, the clock behind `cp_time_t`. |

Synchronisation between Dawn's queue and the compositor's uses
`MTLSharedEvent`s in place of the Quest's sync fds: after `xrWaitSwapchainImage`
the backend asks for the event and value at which the compositor's last read of
the image completes (`xr_visionos_swapchain_image_acquire_fence`), and Aurora's
Dawn `BeginAccess` waits on it; Dawn's `EndAccess` exports the event and value
its copy reaches, which the backend hands back before `xrReleaseSwapchainImage`
(`xr_visionos_swapchain_image_set_release_fence`), and the compositor's render
pass waits on it. Aurora's side is `aurora-main/lib/webgpu/metal_interop.cpp`
(`aurora/metal_interop.h`), the Metal twin of `vulkan_interop.cpp`, using Dawn's
`SharedTextureMemoryIOSurface` and `SharedFenceMTLSharedEvent` features.

### Passthrough and immersion

The room is shown two ways at once. The SwiftUI `ImmersiveSpace` opens in
*mixed* or *full* immersion (the launcher's "Show my room" switch); in mixed
immersion the compositor blends the drawable over the surroundings by alpha.
The game's live `[vr] passthrough` setting (the same one as on the Quest, on by
default) reaches the provider as `xr_visionos_set_frame_environment`: when set,
frames outside an immersive race clear the drawable transparent, so the room
frames the virtual screen and the immersive window; immersive races draw
opaque eyes and cover the room either way.

### Input

The Vision Pro has no controllers. `xr_visionos_input.mm` derives the OpenXR
action state from the ARKit hand skeletons, for the `oculus/touch_controller`
and `khr/simple_controller` profiles the runtime suggests bindings for:

| Gesture | Action |
| --- | --- |
| Index pinch | trigger / select |
| Middle-finger pinch | A (right) / X (left) |
| Ring-finger pinch | B (right) / Y (left) |
| Little-finger pinch | menu |
| Fist (fingers curled) | squeeze / grip |
| Aim pose | From the wrist through the index knuckle; grip pose at the wrist. |

That is enough for the menus, the settings panel and hand steering (see
`OPENXR.md`, "Steering wheel and hand steering"), not for racing at speed. A
Bluetooth game controller (SDL3's GameController backend) is the intended way
to race; `[controls]` bindings apply as on the desktop. Haptics are no-ops.

### Platform glue

- **Static library products.** The products are `libWiiCompiledGame.a` and
  `libRetroRewindGame.a` (`runtime/cmake/PublicProducts.cmake`); the app links
  one with `-force_load` so the registration shards' static initialisers
  survive. `main()` becomes `mkw_runtime_main`, called on a 64 MiB game thread
  by the bridge in `runtime/src/platform/visionos/visionos_host.mm`
  (`runtime/include/platform/visionos/visionos_host.h`).
- **SDL without a window.** Aurora selects SDL's `offscreen` video driver and
  renders through a detached `CAMetalLayer` (`aurora-main/lib/dawn/MetalBinding.mm`,
  `aurora-main/lib/window.cpp`): SDL never touches UIKit, so the game thread
  need not be the main thread. Aurora's frame worker stays enabled here
  (`aurora.cpp`), unlike macOS; the headset owns the display.
- **Guest memory.** The flat guest space is reserved at 16 GiB
  (`runtime/include/guest_flat_memory.h`); `vm_allocate` replaces
  `mach_vm_allocate`, whose header the SDK refuses; the alias backing files go
  to `TMPDIR` (`guest_flat_memory_macos.cpp`). The app carries the
  `extended-virtual-addressing` and `increased-memory-limit` entitlements
  (`visionos/App/WiiCompiledVision.entitlements`).
- **Directories.** The app's `Documents/WiiCompiled` folder holds `Config.toml`,
  `DATA` (the extracted disc), `NAND` and `Logs`; it is visible in the Files
  app and through Finder file sharing (`UIFileSharingEnabled`). Bundled
  resources (`wii_bootstrap/`, `dsp_coef.bin`, `initial_pipeline_cache.db`)
  live at the bundle root, where `RuntimePlatform::ExecutableDirectory()` finds
  them (`host_platform.cpp`). The runtime's transcript is mirrored to the
  unified log (subsystem `org.wiicompiled.vision`) since stdout goes nowhere on
  a device.
- **Blob assembly.** A translation made on Windows or Linux carries
  PE/COFF or ELF section directives in `data_sections_init_blobs.S`;
  `PublicProducts.cmake` rewrites them for Mach-O and adds the `_`-prefixed
  label aliases (`generate-data-init --target-os macos` emits that syntax
  directly).

### The app

`visionos/App/` is a SwiftUI app: a launcher `WindowGroup` (disc status, the
folder paths, the immersion switch, Play) and an `ImmersiveSpace` whose content
is a `CompositorLayer` (dedicated layout, `bgra8Unorm_srgb`, `depth32Float`,
foveation off). The layer's `LayerRenderer` is passed to the provider and the
game thread starts. A watchdog polls the bridge: when the runtime returns, the
launcher says so; when the immersive space is dismissed (the layer is
invalidated) the app asks the runtime to quit, since it would otherwise carry on
rendering to an invisible mirror. **A second run needs a relaunch of the app**:
the runtime keeps process-wide state (the fixed guest reservation, HLE
singletons) a second start would trip over.

## Building

Prerequisites: an Apple Silicon Mac, Xcode 16 or later with the visionOS
platform installed, CMake 3.28+, Ninja, Python 3, git, and the .NET 8 SDK for the
translator. An Apple ID (a free personal team suffices for a headset paired
with the Mac) for signing.

1. **Translate the game** exactly as for the desktop:
   `docs/building-macos.md`, steps 1 to 5 (extract the disc, build the
   translator, translate, `generate-data-init`, `emit-build-shards`). The
   translation is platform-neutral; `generated/build_shards/shards.cmake` must
   exist.
2. **Build Dawn for the visionOS SDK** (once, cached under
   `.scratch/visionos-dawn/`; compiles all of Dawn and Tint, Metal only):

   ```bash
   visionos/Build-VisionOSDawn.sh            # device
   visionos/Build-VisionOSDawn.sh --simulator
   ```

3. **Build the app**:

   ```bash
   visionos/Build-VisionOS.sh --team <TEAMID> [--product base|retro_rewind] [--simulator] [--install]
   ```

   This configures `visionos/CMakeLists.txt` with the Xcode generator (the
   runtime tree is a subdirectory of that project), builds
   `WiiCompiledVision.app` under `build-visionos/Release-xros/`, and with
   `--install` puts it on the paired headset with `devicectl`. `--open` opens
   the generated Xcode project instead, for signing setup or debugging.

   By hand:

   ```bash
   cmake -S visionos -B build-visionos -G Xcode \
       -DCMAKE_SYSTEM_NAME=visionOS -DCMAKE_OSX_SYSROOT=xros \
       -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=2.0 \
       -DAURORA_DAWN_PACKAGE_URL=file://$PWD/.scratch/visionos-dawn/dawn-visionos-arm64.tar.gz \
       -DMKW_VISIONOS_TEAM=<TEAMID>
   cmake --build build-visionos --config Release --target WiiCompiledVision -- -allowProvisioningUpdates
   ```

4. **Game files.** Launch the app once so it creates `Documents/WiiCompiled`
   with a first `Config.toml` (VR on, `dvd_root = "DATA"`), then copy the
   extracted PAL disc (the folder holding `sys/` and `files/`) into `DATA` with
   the Files app or Finder file sharing. Retro Rewind builds read the pack from
   `RetroRewind6` beside it.

Sideloading with a free Apple ID re-signs weekly and allows three apps on the
device; the capabilities used here (Extended Virtual Addressing, Increased
Memory Limit) are ordinary Xcode capabilities.

## Validation status

Done on an Apple Silicon Mac with Xcode 27 (visionOS 27 SDK):

- `visionos/Build-VisionOSDawn.sh` builds the pinned Dawn revision for
  `CMAKE_SYSTEM_NAME=visionOS` unpatched (Metal only, 19 MB static archive,
  Mach-O platform `xros`, `SharedTextureMemoryIOSurface` present).
- `cmake -S runtime -G Xcode -DCMAKE_SYSTEM_NAME=visionOS ...` with that
  package configures (`MKW_BUILD_PRODUCTS=OFF`, no translation at hand), and
  `aurora_core` (including `metal_interop.cpp`, `gpu.cpp`, `MetalBinding.mm`,
  `window.cpp`, `aurora.cpp`), `mkw_openxr_visionos` (all 49 exported `xr*`
  symbols) and `mkw_platform` build without warnings from the new sources.
- Syntax-checked against the same SDK (`clang++ -fsyntax-only`, target
  `arm64-apple-xros2.0`): `openxr_metal.mm`, `openxr_integration.cpp`,
  `openxr_input.cpp`, `visionos_host.mm`, `host_platform.cpp`,
  `guest_flat_memory_macos.cpp`, `main.cpp`, `settings_overlay.cpp`.
- The Swift sources type-check with `swiftc -typecheck` against the SDK.
- With a base-game translation (29,637 functions, 72 shards),
  `visionos/Build-VisionOS.sh --team <id>` produces a 119 MB
  `WiiCompiledVision.app` that passes `codesign --verify --deep --strict`, with
  the two kernel entitlements and the bundled resources in place. Two Xcode
  facts shaped `visionos/CMakeLists.txt`: every Mach-O in the project would be
  signed (so signing is disabled for the library targets and enabled for the
  app alone), and a post-build copy lands after the seal (so the resources are
  bundle resources, the bootstrap folder as a folder reference).

- **Simulator** (visionOS 27, Apple M1 Max): the app installs with `simctl`,
  the disc copied into its container, and with `simctl launch ... --autoplay`
  the game boots into the immersive space and renders the title sequence on
  the virtual screen (3840 x 2160 per simulated eye). ARKit reports hand
  tracking unsupported there, so the provider runs head tracking only.
- **Device** (Apple Vision Pro, visionOS 26.6.1, wired): installed and
  launched with `devicectl`, the disc pushed into the app's data container
  with `devicectl device copy to`. The OpenXR session reached FOCUSED, eyes are
  1888 x 1792, hand tracking was granted, the Wii Remote pointer (hand aim pose)
  reached the menu screen, the settings panel got its quad layer, and a race
  started with the stereo GX replay prepared for both eyes and submitted as a
  projection layer.

Two runtime facts surfaced on the way and are now handled: globals in the
settings overlay load the config during static initialisation, before any Swift
runs, so the bridge fills `dvd_root` into the cached config as well as the file
(`visionos_host.mm`); and SDL ties its virtual-joystick driver, which relays
OpenXR input to the game, to HIDAPI, which it turns off on visionOS, so
`aurora-main/cmake/AuroraSDL3Patches.cmake` loosens that dependency.

Not done: frame timing measurements, comfort tuning of the projection quad
depth, the gesture thresholds, `render_scale`. Things to expect to tune first on hardware: the constant depth the
projection quads are drawn at (3 m in `xr_visionos_compositor.mm`, which sets
how the compositor reprojects late frames), the gesture thresholds in
`xr_visionos_input.mm`, and the default `render_scale` (1.0; the M2 is far
stronger than the Quest's XR2 but drives two 1920 x 1824 eyes at 90 Hz).

## Known gaps and next steps

- Hands only: racing needs a Bluetooth controller; a virtual on-hand steering
  wheel gesture set is the obvious follow-up (the hand-steering code path in
  `OPENXR.md` already takes hand poses).
- Foveated rendering is off; CompositorServices' rasterization rate maps would
  have to shape Dawn's eye passes, the same problem the Quest solved with a
  Dawn patch (`docs/quest-port.md`, "Foveated rendering").
- The provider composites projection layers as quads at a fixed depth, so the
  compositor's late reprojection is planar. Writing the game's real depth
  would need Aurora to export its depth buffers through the same IOSurface
  bridge.
- One game run per app launch.
- No launcher-side disc extraction: the disc is extracted on a computer and
  copied over.
