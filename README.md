<p align="center">
  <img src="src/android/app/src/main/res/drawable/ic_yuzu.png" width="128" alt="Eden Duo">
</p>

<h1 align="center">Eden Duo</h1>

<p align="center">
  A Nintendo Switch emulator for Android dual-screen handhelds, with live companion screens on the second display.
</p>

---

Eden Duo is a fork of the [Eden](https://git.eden-emu.dev/eden-emu/eden) Switch emulator for Android devices with two screens, such as the AYN Thor. The game runs on the top screen as usual. The bottom screen shows a touch **companion** for the game you are playing: maps, party and inventory menus, status, and more, all driven by the running game's live state.

Companions are separate, installable **`.dsmod.zip` packages**, one per game, so a companion can be updated without updating the emulator. This repository holds the emulator source code and the Eden Duo APK. Companion packages, their documentation and screenshots live in **[Eden Duo Companions](https://github.com/igawa6/eden-duo-companions)**.

## Supported Games

| No | Game | Title ID | Patch Version | Supporter |
|---:|------|----------|---------------|:---------:|
| 1 | [Persona 5 Royal](https://github.com/igawa6/eden-duo-companions#persona-5-royal) | `01005CA01580E000` | 1.0.2 | 🥇<sup>1</sup> |
| 2 | [Metroid Dread](https://github.com/igawa6/eden-duo-companions#metroid-dread) | `010093801237C000` | 2.1.0 | |
| 3 | [The Legend of Zelda: Link's Awakening](https://github.com/igawa6/eden-duo-companions#the-legend-of-zelda-links-awakening) | `01006BB00C6F0000` | 1.0.1 | |
| 4 | [Mario Kart 8 Deluxe](https://github.com/igawa6/eden-duo-companions#mario-kart-8-deluxe) | `0100152000022000` | 4.0.0, 3.0.3 (also with CTGP-DX v1.1.1) | |

<sup>1</sup> 🥇 Thanks to [u/gymgooner123](https://www.reddit.com/user/gymgooner123), who commissioned the Persona 5 Royal companion.

Compatibility is intentionally strict. Each companion is written for exact game builds and checks the running build before it loads. On any other version it does not load and shows a notice, instead of reading memory it does not understand.

Details, screenshots and downloads for each companion: **[Eden Duo Companions](https://github.com/igawa6/eden-duo-companions)**.

## Features

- **Second-screen companion.** The game on the main screen, and a touch companion page on the second display.
- **Live game state.** Companions read the running game's memory every frame. There are no save-file snapshots and no polling delays.
- **Installable packages.** Add a companion from the game's *Add-ons* menu with its `.dsmod.zip`. Packages are validated (title, build, file layout and module checksums) before they are installed.
- **Native companion modules.** A package can carry a small native module for games whose data is too complex for a declarative manifest. Modules talk to the emulator through a small, versioned C interface and never ship inside the APK.
- **Touch that feels native.** Taps, press-and-hold gestures, drag and drop, animated page and layout changes, and haptic feedback on the second screen.
- **Built for handhelds.** Change-driven partial redraws, tile-based texture uploads and an optional GPU compositor keep the second screen at full frame rate without costing the game frames.

## What's New

See the [changelog](CHANGELOG.md).

## Requirements

- Android 13 or newer, arm64 (`arm64-v8a`).
- A device with a second display (developed and tested on the AYN Thor).
- Your own legally obtained game dump, the matching update, and your console's keys and firmware.
- The exact game version listed in [Supported Games](#supported-games).

## Install

1. Download the latest `EdenDuo-*.apk` from [Releases](https://github.com/igawa6/eden-duo/releases) and install it.
2. Set up the emulator as usual: keys, firmware, and your games folder.
3. Download a companion package from [Eden Duo Companions](https://github.com/igawa6/eden-duo-companions).
4. In Eden Duo, long-press the game, open **Add-ons** and tap **Install**. In the **Content type** dialog choose **Dual screen mods**, tap **OK**, then select the `.dsmod.zip` file. The companion appears in the Add-ons list as `<Name>-<version>`, for example `MetroidDreadDS-1.0.0`.
5. Launch the game. The companion appears on the second screen once the game reaches gameplay.

![Installing a companion: Add-ons, Install, Dual screen mods](dist/screenshots/addons_dualscreen.png)

Eden Duo uses its own app ID (`dev.igawa6.edenduo`), so it installs alongside other Eden builds and keeps separate data.

## Launching from a Frontend

| Frontend | Setup |
|----------|-------|
| [CocoonFE](https://github.com/inssekt/CocoonFE) | Built in, thanks to [cream-neapolitan](https://github.com/cream-neapolitan). In Cocoon, open **Settings → Library & Data → Refetch Platforms**. Then, for each game you want on Eden Duo, open the game's settings and set **Player Override** to **Eden Duo**. |
| [ES-DE](https://es-de.org) (Android) | Copy [`es-de/es_systems.xml`](dist/frontends/es-de/es_systems.xml) and [`es-de/es_find_rules.xml`](dist/frontends/es-de/es_find_rules.xml) into `ES-DE/custom_systems/` and restart ES-DE. **Eden Duo (Standalone)** becomes the default Switch emulator. |

The ES-DE files are the upstream files from [es-de-android-custom-systems](https://github.com/GlazedBelmont/es-de-android-custom-systems), with only the Eden Duo entries added.

## Making a Companion for Another Game

Eden Duo is not limited to the games above. Anyone can write a companion for another game and ship it as a `.dsmod.zip`, without changing or rebuilding the emulator.

**What the runtime gives you (runtime 13, Eden Duo 1.0.1):**

| Layer | What you get |
|-------|--------------|
| Package (JSON) | Pages built from 8 widget types: rect, label, value, bar, button, pips, image and map. Widgets support bindings, derived values, flags, page transitions, animations, scroll lists, repeat templates, taps and press-and-hold gestures with haptic feedback. There are 10 action kinds: write, button, page, call, sequence, flag, view reset, module, slot write and map select. Game art is referenced from the player's own files (`romfs:`), so a package never ships game assets. |
| Memory points | Pointer chains, static fields, pattern scans and per-build address tables, all checked against the game's build ID before loading. |
| Native module (C ABI) | For data too complex for JSON. Modules get bounded memory reads and writes, the player's romfs, and publishing of values and map frames. Five optional extensions add actions and module images, font decoding, save-file reads, atomic write batches, and module-generated data. |
| Compatibility | `min_runtime` in the package makes an older Eden Duo show an "update" page instead of failing. |

**What helps you reverse engineer a game:**

- **Desktop development build.** `eden-cli` runs a game with its companion headlessly (`--aux-virtual`, or `--aux-window` for a real second window) and captures both screens (`--screenshot-prefix`). It can be scripted with button presses and taps.
- **Live memory console.** Set `EDEN_DSMOD_CMD` to enable it. Commands:
  - value search and change tracking: `findi`, `findf`, `sfind`, `snap`, `diff`;
  - pointer search: `ptrto`;
  - memory: `watch`, `hexdump`, `readi`, `writeb`;
  - object and manager finders: `objfind`, `mgrfind`;
  - companion control: `value`, `tap`, `drag`, `page`;
  - image dumps: `imgdump`.
- **GDB stub** for breakpoints and memory watches on the running game.
- **Guest function calls** (desktop CPU backend) for research, and `EDEN_DSMOD_NO_GUEST_BRIDGE=1` to test under the same conditions as an Android handheld.
- **`EDEN_DSMOD_PROFILE=1`** for per-stage cost, so a companion stays cheap on handheld hardware.

**Start here:** the step-by-step method, from dumping the game and finding its state through validating against the game's own screens to packaging, is in [**Porting a Game**](https://github.com/igawa6/eden-duo-companions/blob/main/docs/PORTING_A_GAME.md). Also useful:

- [Package Format](https://github.com/igawa6/eden-duo-companions/blob/main/docs/PACKAGE_FORMAT.md)
- [Module Guide](https://github.com/igawa6/eden-duo-companions/blob/main/docs/MODULE_GUIDE.md)
- [Architecture](https://github.com/igawa6/eden-duo-companions/blob/main/docs/ARCHITECTURE.md)
- The [four published packages](https://github.com/igawa6/eden-duo-companions/tree/main/packages), as working examples.

Module sources live in this repository under [`src/core/mods/modules/`](src/core/mods/modules/).

**AI assistance is welcome.** Building a companion is mostly careful reverse engineering and repetitive verification, and AI coding assistants are good at both. Give your assistant the docs above and an existing package as a template. Let it drive the headless desktop build, the memory console and the screenshots. Then verify every value it finds against the game's own screens before you trust it. Companions made with AI help are welcome here.

## Build

Requirements: JDK 17, Android SDK 36, NDK 28.2.13676358, CMake 3.31.6.

```sh
cd src/android
export ANDROID_HOME=$HOME/Android/Sdk
export ANDROID_NDK_ROOT=$ANDROID_HOME/ndk/28.2.13676358

# Local test build ("Eden Duo Dev", installs beside the release build)
./gradlew assembleMainlineRelWithDebInfo

# Release build, signed with your own key
export ANDROID_KEYSTORE_FILE=/path/to/release.jks
export ANDROID_KEYSTORE_PASS=...
export ANDROID_KEY_ALIAS=...
./gradlew assembleMainlineRelease
```

The APK is written to `src/android/app/build/outputs/apk/mainline/<buildType>/`.

A desktop build (`eden-cli`) is supported for companion development. It can run a companion headlessly and capture both screens.

## How It Works

The dual-screen runtime lives in `src/core/mods/`. Each tick it:

1. Samples the game: memory points, pointer chains, and the companion's native module.
2. Evaluates derived values and handles touch input.
3. Redraws only what changed on the companion page.
4. Hands the page to the second display.

The package format, the module interface and the method used to build a companion for a new game are documented in the [Eden Duo Companions docs](https://github.com/igawa6/eden-duo-companions/tree/main/docs).

## Game Assets

Eden Duo and its companions ship **no game assets**. Companion art and text are decoded at runtime from the player's own game files. Game data, keys, firmware and ROMs must not be uploaded to this repository or its releases.

## AI Assistance

Eden Duo was developed with AI assistance. The dual-screen runtime, the companion modules and the reverse engineering of each game were written with an AI coding assistant (Claude, by Anthropic), then reviewed, tested and verified on real hardware.

If anything is wrong with Eden Duo, report issues here rather than to upstream Eden.

## Kind Words from Eden Dev

![Kind Words from Eden Dev](dist/screenshots/kind_words.png)

## Credits and License

Eden Duo is based on the open-source [Eden](https://git.eden-emu.dev/eden-emu/eden) emulator and its predecessors.

Eden Duo is an independent fork. It is not affiliated with or endorsed by the Eden project or Nintendo. All trademarks belong to their respective owners.

Eden Duo is free software, released under the [GNU General Public License v3.0](LICENSE.txt).
