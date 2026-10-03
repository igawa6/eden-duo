# Changelog

## 1.1.0

**New**

- **Animal Crossing: New Horizons**, **Fire Emblem: Three Houses** and **The Binding of Isaac: Afterbirth+ and Repentance DLC** companion support.
- **Swap Screens.** Put gameplay on the display you prefer and the companion on the other. Handy for different dualscreen layouts on Retroid or AYANEO Android handhelds.  in **Settings → Graphics → Second Screen**.
- **No Companion.** Give the bottom screen a purpose even when a game has no dualscreen mod yet: automatically launch your favourite guide app, browser, media player or other app alongside the game. Choose **App**, then pick the app you want, in **Settings → Graphics → Second Screen**. Icon, Black and Off are available too.
- **Companion Ratio.** Fit keeps the companion's shape; Stretch fills the screen.
- Choose your above 3 layout **per game**, or set a global default.
- For companion makers (see the [Package Format](https://github.com/igawa6/eden-duo-companions/blob/main/docs/PACKAGE_FORMAT.md)):
  - Controller navigation, saved settings, player files, charts and image transforms.
  - Fitted and vertically centered text, game-art scrollbars, map-marker size caps and font refresh after language changes.

**Faster**

- Fitted text needs fewer layout measurements, and failed font reads avoid repeated refresh work.

**Fixed**

- Companion parsing and rendering edge cases, including null label text, decimal coordinates, negated visibility gates and format-only image sources.
- Font refresh discards stale atlases, and image-cache memory accounting stays accurate.

**Maybe fixed**

- Update/DLC detection immediately after startup (#1).
- Installed companions not being found (#3).
- Companions on dual-display add-ons (#4).

Update: install over your current Eden Duo. Your games and settings are kept, and existing companions keep working.

- Requires Android 13+ (arm64) and a device with a second display.
- Companion packages for supported games: https://github.com/igawa6/eden-duo-companions

SHA-256: 97cb25102fdffef9689795139295801da7ad0110540848e6d602fc0fe408ff33

## 1.0.2

Companion runtime 15. Needed by the Super Mario Bros. Wonder companion. Companions made for 1.0.0 and 1.0.1 keep working unchanged.

**New**

- **Super Mario Bros. Wonder** companion support (game version 1.2.1). The companion itself is in [Eden Duo Companions](https://github.com/igawa6/eden-duo-companions#super-mario-bros-wonder).
- For companion makers (see the [Package Format](https://github.com/igawa6/eden-duo-companions/blob/main/docs/PACKAGE_FORMAT.md)):
  - **Swipes.** `on_swipe_left` / `on_swipe_right` / `on_swipe_up` / `on_swipe_down` on any widget.
  - **Pan and zoom.** Map and `pan_zoom` widgets take a default view from a bound rectangle or from bound zoom and centre values, with a reset action.
  - **Map layers.** The map's base picture, overlays and per-slot marker pictures can come from bindings. Modules can read the last map tap.
  - **Coloured words** inside a label (`"color_markup": true`, `{c:#AARRGGBB}…{/c}`), and `outline_copy` for one-colour outline copies.
  - **Outlined game-font text** (`outline`, `outline_px`) and digits that step upwards (`rise`), as some games draw their HUD numbers.
  - **Button combinations** in button actions, for example `"L+R"`.
  - **Persisted settings.** Flags named in `persist_flags` are saved per game and restored on the next start.
  - **More game files.** Modules can read the base game's files (`base:`) and the installed DLC's files (`aoc:`), next to the updated game files.
  - Modules can ask to keep ticking, or not, while the second screen is hidden.

**Faster**

- The second screen does less work on each redraw: cheaper partial redraws, a larger text layout cache, no per-glyph table setup, and the GPU compositor copies only the changed part of the page. Pixel for pixel the same.
- The 5-second "DSMod performance" log line is off in published builds.

**Fixed**

- Starting a game while Eden Duo was still loading the game list (from the list, a shortcut or a front-end) could crash, or start the game without its update (#1, #2).
- The second screen was turned off on every pause and was never shown on a display that was turned on or plugged in after the game started. It now stays up from game start to stop and follows display changes.
- The second screen could stay black when its surface arrived while a game was loading, and the app could stall while a game with a companion was loading.
- Companion pictures and fonts that a companion builds from the running game now retry until the game is ready, instead of staying missing until a restart. Pictures that arrive during a page fade show at once.
- A companion's code patches now apply even when the second screen is hidden at game start (for example with the lid closed).
- Values written by a companion (equipping, swapping items) are now written in one step with the game paused, so the game never sees half a change.
- A second game started in the same session could inherit the previous game's companion state.
- Several rare crashes when a companion reloads, changes fonts or draws pictures near their edges.

**Installing companions**

- Renamed downloads (for example `… (1).dsmod.zip`) install. Zips re-packed on macOS or Windows no longer fail on `__MACOSX/` or `.DS_Store`.
- A companion that needs a newer Eden Duo is refused before anything is replaced, so the installed companion keeps working. A failed install says why.
- The installed companion is always the one the second screen uses, even when an old hand-copied folder for the same game is present.
- The progress bar follows the file, including large companions.
- The "update required" page on the second screen now says Eden Duo instead of Eden.

## 1.0.1

Companion runtime 13. Needed by the Mario Kart 8 Deluxe companion; the other companions keep working unchanged.

**New**

- **Mario Kart 8 Deluxe** companion support (game versions 4.0.0 and 3.0.3, also with CTGP-DX v1.1.1). The companion itself is in [Eden Duo Companions](https://github.com/igawa6/eden-duo-companions#mario-kart-8-deluxe).
- **Press-and-hold** on the second screen. A companion can run an action when a finger rests on a spot, for example to switch a theme, without also counting it as a tap. Holds give a stronger haptic than taps.

**Faster**

- On the AYN Thor the second screen no longer costs the game frames. The ROM's frame pacer used to hold every second-screen update for several milliseconds, and the game waited behind it: Link's Awakening dropped to 49-53 fps while walking with its companion open, and now runs at 56-57 (58.5 with no companion).
- Companion pages draw about a sixth faster, pixel for pixel the same.

**Fixed**

- The second screen could keep showing an old picture for a few seconds after fast changes, such as rows swapping places during an overtake. Every redraw now reaches the screen.
- Animated layout changes (a table growing or shrinking) did not show while they ran.
- Companion images could appear up to a minute late after the game started.
- With a game update installed, companions could read the base game's files instead of the updated ones when the game was booted from a file outside the game list (desktop `eden-cli`). Reads of the game files by the game and by a companion at the same moment could also return wrong bytes.

## 1.0.0

First release, with companions for Persona 5 Royal, Metroid Dread and The Legend of Zelda: Link's Awakening. Companion runtime 12.
