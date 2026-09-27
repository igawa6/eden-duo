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

| No | Game | Title ID | Patch Version |
|---:|------|----------|---------------|
| 1 | [Persona 5 Royal](https://github.com/igawa6/eden-duo-companions#persona-5-royal) | `01005CA01580E000` | 1.0.2 |
| 2 | [Metroid Dread](https://github.com/igawa6/eden-duo-companions#metroid-dread) | `010093801237C000` | 2.1.0 |
| 3 | [The Legend of Zelda: Link's Awakening](https://github.com/igawa6/eden-duo-companions#the-legend-of-zelda-links-awakening) | `01006BB00C6F0000` | 1.0.1 |

Compatibility is intentionally strict. Each companion is written for one exact game build and checks the running build before it loads. On any other version it does not load and shows a notice, instead of reading memory it does not understand.

Details, screenshots and downloads for each companion: **[Eden Duo Companions](https://github.com/igawa6/eden-duo-companions)**.

## Features

- **Second-screen companion.** The game on the main screen, and a touch companion page on the second display.
- **Live game state.** Companions read the running game's memory every frame. There are no save-file snapshots and no polling delays.
- **Installable packages.** Add a companion from the game's *Add-ons* menu with its `.dsmod.zip`. Packages are validated (title, build, file layout and module checksums) before they are installed.
- **Native companion modules.** A package can carry a small native module for games whose data is too complex for a declarative manifest. Modules talk to the emulator through a small, versioned C interface and never ship inside the APK.
- **Built for handhelds.** Change-driven partial redraws, tile-based texture uploads and an optional GPU compositor keep the second screen at full frame rate without costing the game frames.

## Requirements

- Android 13 or newer, arm64 (`arm64-v8a`).
- A device with a second display (developed and tested on the AYN Thor).
- Your own legally obtained game dump, the matching update, and your console's keys and firmware.
- The exact game version listed in [Supported Games](#supported-games).

## Install

1. Download the latest `EdenDuo-*.apk` from [Releases](https://github.com/igawa6/eden-duo/releases) and install it.
2. Set up the emulator as usual: keys, firmware, and your games folder.
3. Download a companion package from [Eden Duo Companions](https://github.com/igawa6/eden-duo-companions).
4. In Eden Duo, long-press the game, open **Add-ons**, choose **Install**, and select the `.dsmod.zip` file.
5. Launch the game. The companion appears on the second screen once the game reaches gameplay.

Eden Duo uses its own app ID (`dev.igawa6.edenduo`), so it installs alongside other Eden builds and keeps separate data.

## Launching from a Frontend

Ready-made configuration files in [`dist/frontends/`](dist/frontends/) add Eden Duo as a Nintendo Switch emulator:

| Frontend | Files | Setup |
|----------|-------|-------|
| [CocoonFE](https://github.com/inssekt/CocoonFE) | [`cocoonfe/NintendoSwitch.json`](dist/frontends/cocoonfe/NintendoSwitch.json) | Import it as the Nintendo Switch platform, then choose **Eden Duo** as the player. |
| [ES-DE](https://es-de.org) (Android) | [`es-de/es_systems.xml`](dist/frontends/es-de/es_systems.xml), [`es-de/es_find_rules.xml`](dist/frontends/es-de/es_find_rules.xml) | Copy both files into `ES-DE/custom_systems/` and restart ES-DE. **Eden Duo (Standalone)** becomes the default Switch emulator. |

These are the upstream files from [CocoonFE](https://github.com/inssekt/CocoonFE/blob/main/platforms/NintendoSwitch.json) and [es-de-android-custom-systems](https://github.com/GlazedBelmont/es-de-android-custom-systems), with only the Eden Duo entries added.

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

## Credits and License

Eden Duo is based on the open-source [Eden](https://git.eden-emu.dev/eden-emu/eden) emulator and its predecessors.

Eden Duo is an independent fork. It is not affiliated with or endorsed by the Eden project or Nintendo. All trademarks belong to their respective owners.

Eden Duo is free software, released under the [GNU General Public License v3.0](LICENSE.txt).
