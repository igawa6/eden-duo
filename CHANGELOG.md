# Changelog

## 1.1.0

Companion runtime 18. Every companion that works on 1.0.2 keeps working unchanged.

**New: Second Screen settings**

In **Settings → Graphics → Second Screen**, and per game (long-press the game, **Settings → Graphics → Second Screen**). A game's own value overrides the global one; long-press a setting to reset it to the global value.

- **Swap Screens.** The game on the second screen and the companion on the main screen. Works from the game list and from frontends. A game with its own Swap Off always opens on the main screen.
- **Companion Ratio.** Fit (default) keeps the companion's shape and adds bars if needed. Stretch fills the screen.
- **No Companion.** What the second screen does for a game without a companion: Icon (default, the game icon dimmed on black), Black, Off (no window, so the second screen is free for other apps), or App (opens an app you choose on the second screen when the game starts, picked from a searchable list). Until you choose an app, App works like Off.
- No Companion can be changed while a game runs. It applies when you return to the game (Icon and Black from the next start).
- With Swap Screens on and a game that has no companion, Off (or App with no app chosen) keeps the game on the main screen.
- When the game runs on a second display (for example the Retroid Pocket with its Dual Screen Add-on), the companion opens on the main display.

**New: runtimes 16–18 for companion makers** (see [Contribute to Another Game](https://github.com/igawa6/eden-duo-companions/blob/main/docs/CONTRIBUTE.md))

- **Controller navigation.** A button chord moves a focus over the companion's tappable widgets with the D-pad; A taps, B leaves. The game gets a neutral pad meanwhile. On by default for packages that set `"min_runtime": 17` or higher.
- **Companion settings page.** A manifest `settings` list gives a built-in options page, saved per game.
- **Player files.** `user:<path>` reads files the player puts in `dualscreen/user/<TITLEID>/`.
- **Data.** Clock points (`@clock.*`, `@game.seconds`), `countdown` and `expr` derived values.
- **Pictures.** Image rotate and scale (fixed or bound), tint, tiled and 9-slice image fills, bars that fill from any side, and a **chart** widget for values over time.
- **Text.** Labels and buttons that size to their text (`auto_w`), `max_lines` with an ellipsis, thousands separators on values (`group`), `{i}` in repeat templates, and paged font atlases for large character sets.
- **Input and modules.** A hold and a drag can share one widget. A module can refuse an action (refused haptic, nothing after it runs). Selection and drag points are ready before taps, so gates can judge them. `read_romfs` works in a module's `create()`. Longer module image keys.
- Packages that use these set `"min_runtime": 17` (or 16); an older Eden Duo then shows its "update" page.

- **Runtime 18.** Fitted single-line labels (`fit_text`, `text_min_scale`), vertically centered labels (`text_center_h`), image scrollbars (`bar_src`, `bar_track_src`), and map marker size caps (`size_max`). Modules can refresh their font after a late language change through `__font_epoch`.
- **Rendering fixes.** Null label text, decimal widget coordinates, negated repeat gates and format-only image sources now work. Font refresh discards stale atlases and keeps image-cache accounting accurate.

**Release build**

- Development console, heap tools, profiling, animation dumps and test overrides are excluded from the APK.

**Maybe fixed**

- A game's update or DLC was sometimes missing when the game started right after opening Eden Duo (#1).
- An installed companion sometimes did not appear in Add-ons, or the second screen only showed the logo (#3). Companion installs now check that Eden Duo can find the package, and say why if it cannot.
- The companion did not show on the Retroid Pocket Dual Screen Add-on when Eden Duo ran on the add-on screen (#4).

**Fixed**

- Add-on on/off switches could change on their own while scrolling a long list, or reset after an install. Deleting an add-on that was off no longer keeps it off when reinstalled.

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
