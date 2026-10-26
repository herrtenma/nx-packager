# Homeport: the Packager inside LiveContainer

One app that both builds ports and runs them. Today the user builds an `.ipa` in the Packager, shares it to
LiveContainer, installs it there, then launches it from LiveContainer's list. Homeport puts the two together:
port packs and installed apps sit on one home screen, **Build** writes the port straight into LiveContainer's
`Applications/`, and **Play** launches it in the same process.

The UI prototype is [`design/homeport.html`](../design/homeport.html). Open it in a browser to see the build and
launch flows.

## Design

A modern take on the 3DS home menu, with the PS5's hero treatment for the selected title.

| Part | What it shows |
|---|---|
| Top screen (5:3) | The selected title's logo floating in 3D. It is extruded in layers, turns toward the pointer or device tilt and sways when idle. The backdrop is tinted from the title's colours. A status bar shows LiveContainer and JIT state, counts and the clock. A caption gives the kind (port pack, built port, LiveContainer app), the full name, the state and the A action. |
| 3D slider | The extrusion depth and parallax strength. 0 is flat, as on the 3DS. The setting is kept per user. |
| Bottom screen (4:3) | A system row (Packager folder, Sources, Tweaks, Settings) above a 2-row shelf of tiles. Selection is an amber pulsing frame. A tile badge shows the state: ready to build, needs a dump, building (progress ring) or built port. |
| Action bar | **Y Details** and **A** with the tile's action: Build, Add dump, Play or Install IPA. **B** goes back. |
| Details sheet | For a port: the Packager's three checks (pack, dump, `prod.keys`) and where it is installed. For an app: LiveContainer's per-app settings (container, JIT, multitask). |
| Wide screens | On iPad landscape the two screens sit side by side. |

Logos: a port pack can ship `art/logo.png` (a cut-out logo with transparency) and `art/colors.json`. Without them,
Homeport sets the port's name in an extruded wordmark, as the prototype does. The tile icon can come from the
user's own dump, from the control NCA's `icon_<language>.dat` (a 256×256 JPEG). `core/nxcontent.c` already opens
the control data for the name, so the pack never needs to carry Nintendo art.

## How it fits into LiveContainer

LiveContainer is a SwiftUI app (`LiveContainerSwiftUI/`) with a tab view (`LCTabView.swift`: Sources, Apps,
Tweaks, Settings). The relevant pieces:

| LiveContainer | Used for |
|---|---|
| `LCPath.bundlePath` (`Documents/Applications`) | Where installed `.app` bundles live. Built ports go here. |
| `LCAppListView.installIpaFile(_:)` | Unzips an IPA into `Payload/`, reads `LCAppInfo`, handles same-bundle-ID replacement, moves the app into `bundlePath`. Homeport reuses everything after the unzip. |
| `LCAppInfo` / `LCAppModel` | Per-app settings (container, JIT, tweaks folder, signing) and the model the app list shows. |
| `LCAppListView.launchAppWithBundleId(bundleId:container:urlStr:forceJIT:)` | Launch. **Play** calls this. |
| `livecontainer://livecontainer-launch?bundle-name=…`, `livecontainer://install?url=…` | URL schemes. Used only by the fallback below. |

### Plan (fork, recommended)

1. **Fork LiveContainer** and add this repo's `core/` (`nxcontent.c`, `nxzip.c`, `nxpack.m`) to the
   LiveContainer target. Expose `nxpack.h` through `LiveContainerSwiftUI-Bridging-Header.h`. `-lz` and CommonCrypto
   are already available on iOS.
2. **`NxBuildApp`**: a variant of `NxBuildIPA` that stops after step 4 and writes the filled `Payload/<app>.app`
   to a directory instead of zipping it. This avoids zipping 60+ MB only for LiveContainer to unzip it again.
   `NxBuildIPA` stays for the standalone Packager and the Mac CLI.
3. **Install hook**: split `installIpaFile` into "unzip" and "install an unpacked `.app`" (`installAppBundle(at:)`).
   Homeport calls the second half with the folder from step 2. Signing, replacement and `LCAppInfo` setup stay
   LiveContainer's.
4. **Ports model**: `NxPortModel` (Swift) scans `Documents/Packs/` the way `ios/app.m` scans its folder: the newest
   `.nxport` per pack ID, `prod.keys`, and the dumps checked in the background with `NxDumpInfo` /
   `NxDumpMismatch`. It records which installed bundle a pack produced (`nxport.json` → `app` + bundle ID), so a
   built port shows as one tile, not two.
5. **Home tab**: `HomeportView` becomes the first tab and the default. The shelf merges `NxPortModel`'s ports with
   `DataManager.shared.model.apps`. The existing Apps list stays one tab over for power features (folders,
   hidden apps, containers).
6. **Theme**: the floating logo is a `SceneKit`-free SwiftUI stack. There are N copies of the wordmark (or of
   `logo.png` tinted down) offset in z by `rotation3DEffect` plus per-layer offsets, driven by `CMMotionManager`
   attitude (pointer on iPad). The 3D slider value lives in `@AppStorage("homeport.depth")`.

### Fallback (no fork)

Keep the Packager a separate app and add a **Send to LiveContainer** button: write the IPA to the shared folder and
open `livecontainer://install?url=file://…`. After install, a "Play" tile opens
`livecontainer://livecontainer-launch?bundle-name=<bundle>.app`. This needs no LiveContainer changes, but it is
two apps, not one launcher, and `install?url=` expects a downloadable URL. A file URL into the Packager's sandbox
works only when the Packager itself runs inside LiveContainer.

## Licences

LiveContainer is **AGPL-3.0**, this repo is **GPL-3.0**. GPLv3 §13 explicitly allows combining GPLv3 code with
AGPLv3 code. The combined Homeport app is then distributed under the AGPL's terms. Its source must be offered to
users, which a public fork already does. `core/` files keep their GPLv3 headers and can still be used by the
standalone Packager and the CLI.
