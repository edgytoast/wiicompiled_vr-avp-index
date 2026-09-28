# Mario Kart Wii on Apple Vision Pro: from your disc to the headset

This guide takes you from a Mario Kart Wii disc image you own to WiiCompiled
running on your Apple Vision Pro, in stereo, with your hands. One script does
the work; this page tells you what to install first, what to expect, and what
to do when something stops.

The app is not on the App Store and cannot be: visionOS only runs code Apple
or *you* signed, so the game has to be translated and built on your own Mac,
from your own disc, and signed with your own Apple ID. Nothing of the game is
downloaded or distributed by this project.

## What you need

| | Why |
| --- | --- |
| **An Apple Silicon Mac** (M1 or later), 15 GB free besides Xcode | The visionOS toolchain is arm64 only; the extracted disc (2.5 GB), the Retro Rewind pack (4 GB) and the build (3 GB) take room |
| **Xcode** (free, Mac App Store), with the **visionOS** platform | Only Xcode has the visionOS SDK and can sign apps for a headset. About 15 GB |
| **An Apple ID signed in to Xcode** | It signs the app. A free account works (see *Free or paid account* below) |
| **Homebrew** with `cmake`, `ninja` and the .NET SDK | The translator is a .NET program, the build uses CMake |
| **Your Mario Kart Wii PAL disc** as `.iso`, `.rvz`, `.wbfs`, `.ciso` or `.gcm` | The game. Only the European `RMCP01` release works; the script checks it |
| **Your Apple Vision Pro**, paired with the Mac | Pairing is done once in Xcode |
| An hour the first time | Most of it is compiling; later runs take a minute or two |

### Free or paid account

A free Apple ID signs apps that **stop opening after 7 days** and allows three
sideloaded apps on the headset. Re-running the script with `--reinstall`
(a minute or two) makes it good for another week. A paid Apple Developer
account ($99/year) signs for a year. Everything else is the same.

## Step by step

### 1. Install Xcode and the visionOS platform

1. Install **Xcode** from the Mac App Store and open it once; accept the
   licence and let it finish installing components.
2. **Xcode > Settings > Components**: install **visionOS** (a few GB).
3. **Xcode > Settings > Accounts**: press **+**, sign in with your Apple ID.

### 2. Install the build tools

Install [Homebrew](https://brew.sh) if you do not have it, then in Terminal:

```bash
brew install cmake ninja
brew install --cask dotnet-sdk
```

### 3. Pair the headset

1. On the Vision Pro: **Settings > General > Remote Devices**.
2. On the Mac, in Xcode: **Window > Devices and Simulators**; the headset
   appears, pair it and enter the code the headset shows.
3. When asked, turn on **Developer Mode** on the headset (**Settings >
   Privacy & Security > Developer Mode**) and restart it.

A cable is not required, but the disc copy (2.5 GB) is far faster over one.

### 4. Get the project

```bash
git clone --branch vision-pro https://github.com/iChris4/Wiicompiled_VR.git
cd Wiicompiled_VR
```

### 5. Run the script

Either double-click `visionos/Make-VisionOS-App.command` in Finder (it asks for
the disc image and whether you want Retro Rewind), or in Terminal:

```bash
visionos/Make-VisionOS-App.command --game ~/Downloads/RMCP01.rvz --retro-rewind download
```

Leave out `--retro-rewind download` for the plain game. The script prints
seven steps:

1. **Checking this Mac**: Xcode, the visionOS SDK, the tools, your team, the
   headset. Anything missing stops here with the fix to apply.
2. **Extracting the disc** (nodtool, downloaded once): checks it is a clean
   PAL image and leaves the extracted game in `Assets/DATA`.
3. **Retro Rewind**: downloads the pack from Retro Rewind's own server (about
   1.9 GB) and the Retro-WFC payload for online play.
4. **Translating the game**: the translator is built, then turns the game's
   PowerPC code into C++ (a few minutes).
5. **Building and signing the app**: the first time this compiles the graphics
   library (Dawn), the runtime and the translated code, **30 to 60 minutes**
   depending on the Mac. Later builds reuse all of it.
6. **Copying the disc into the app** on the headset (a few minutes over the
   cable, much longer over Wi-Fi).
7. **Launching** the app.

Keep the headset unlocked and awake during steps 5 to 7; a dozing headset
drops the connection (the script retries).

### 6. On the headset

- The first time, visionOS refuses the app as coming from an untrusted
  developer: **Settings > General > VPN & Device Management**, tap your Apple
  ID and **Trust**. Then open **WiiCompiled Vision** from the Home View.
- The launcher shows the disc as found. Press **Play**: the menu appears on a
  virtual screen in your room; look at a button and pinch, move your hand to
  adjust the pointer while pinching, let go to press.
- For **Retro Rewind**: pick it in the Play tab, press **Download Retro
  Rewind** (about 2 GB from Retro Rewind's server to the headset), then Play.
- Race with a Bluetooth game controller, or with bare hands (**Settings >
  Drive with your hands**). Press the Digital Crown to leave the game.

## Later

| You want to | Do |
| --- | --- |
| Open the app after the 7-day expiry (free account) | `visionos/Make-VisionOS-App.command --reinstall` |
| Update to a newer version of this project | `git pull`, then run the script again; it rebuilds what changed |
| Add or remove Retro Rewind | Run the script with or without `--retro-rewind download` (the game is retranslated) |
| Follow a Retro Rewind update that changed `Code.pul` | The app tells you it does not match the pack. Run `--retro-rewind download --retranslate` |
| Change graphics or controls | The launcher's **Settings** tab, or `Config.toml` in the Files app (On My Apple Vision Pro > WiiCompiled Vision > WiiCompiled) |
| Use another headset or team | `--device UDID`, `--team TEAMID` |

## When it stops

The script names what is wrong and what to do. The usual ones:

- **"Xcode is not installed" / "visionOS platform is required"**: step 1.
  If Xcode is installed but not selected, `sudo xcode-select -s /Applications/Xcode.app`.
- **"No Apple developer team found"**: sign in to Xcode (step 1.3). If you
  have several teams, pass `--team` with the one to use; Xcode > Settings >
  Accounts shows them.
- **"No paired Apple Vision Pro"**: step 3. `xcrun devicectl list devices`
  must list it as `available (paired)`.
- **"disc is not the supported clean PAL RMCP01 image"**: only the European
  release is supported, unmodified. NTSC and modified dumps are refused.
- **Signing errors during the build**: open Xcode > Settings > Accounts and
  check the Apple ID is still signed in (its session can expire). A free
  account that already has three sideloaded apps on the headset must remove
  one.
- **"Failed to install the app on the device"**: unlock the headset and keep
  it awake; run again with `--reinstall`.
- **The launcher says "No extracted disc yet"**: the disc copy did not
  finish. Run the script again; it checks the headset and copies again if
  needed. If the game itself stops, its transcript is in the Logs folder next
  to `Config.toml` in the Files app.
- **Everything else**: the full output is in Terminal; open an issue with it.

## What it does not do

- Run on the visionOS **simulator** with your hands (build for it with
  `visionos/Build-VisionOS.sh --simulator`; a controller works there).
- Support **other regions** than PAL, or other games.
- Distribute the game: there is no build to download. Everyone makes their
  own, from their own disc.

For how the port works, see [`visionos-port.md`](visionos-port.md); for the
scripts underneath, `visionos/README.md`.
