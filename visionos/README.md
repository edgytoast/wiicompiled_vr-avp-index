# WiiCompiled Vision

The Apple Vision Pro app around the game runtime. Design, build steps and
status: [`../docs/visionos-port.md`](../docs/visionos-port.md).

- `CMakeLists.txt`: the Xcode project (the runtime tree as a subdirectory, the
  SwiftUI app embedding one product as a framework it loads at Play).
- `App/`: the SwiftUI sources (launcher with Play and Settings tabs, the game
  loader, the Config.toml editor), `Info.plist` and entitlements.
- `Build-VisionOSDawn.sh`: Dawn (Metal, static) for the `xros` or
  `xrsimulator` SDK, as the package Aurora's provider consumes.
- `Build-VisionOS.sh`: Dawn, configure, build, optionally install on the
  paired headset.

```bash
visionos/Build-VisionOS.sh --team <TEAMID> --install
```
