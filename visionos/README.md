# WiiCompiled Vision

The Apple Vision Pro app around the game runtime. Design, build steps and
status: [`../docs/visionos-port.md`](../docs/visionos-port.md).

- `CMakeLists.txt`: the Xcode project (the runtime tree as a subdirectory, the
  SwiftUI app linking one product with `-force_load`).
- `App/`: the SwiftUI sources, `Info.plist`, entitlements and the bridging
  header onto `runtime/include/platform/visionos/visionos_host.h`.
- `Build-VisionOSDawn.sh`: Dawn (Metal, static) for the `xros` or
  `xrsimulator` SDK, as the package Aurora's provider consumes.
- `Build-VisionOS.sh`: Dawn, configure, build, optionally install on the
  paired headset.

```bash
visionos/Build-VisionOS.sh --team <TEAMID> --install
```
