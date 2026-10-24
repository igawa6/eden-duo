# Changelog

## Unreleased

- Preserve all touched GPU buffer-read units across unaligned and page-crossing ranges so overlapping CPU uploads cannot be reordered ahead of those reads.
- Preserve shadow-RAM tracking and replay in accelerated shader-binding and constant-buffer setup macros.
- Keep busy Vulkan upload-ring regions reserved after a wrap fallback instead of overwriting in-flight geometry or shader data.
- Track GPU usage of exported cached buffers and indirect draw arguments/counts so later CPU uploads cannot overtake their GPU consumers.
- Preserve newer GPU-written buffer ranges when an older asynchronous readback completes, including writes awaiting their next download snapshot.
- Cancel superseded bytes in queued asynchronous GPU readbacks so retired jobs cannot overwrite newer CPU data or consume a fresh readback's ownership.
- Make Vulkan buffer readbacks visible to host reads before CPU publication, invalidate noncoherent mappings, and protect staging-pool metadata during asynchronous retirement.
- Honor requested GPU-buffer preservation on NCE write faults without requiring the optional readback workaround.
- Reduce host-memory commitment for aligned zero-filled guest allocations on Linux/Android, preserving shared aliases and normal initialization when page discard is unavailable.
- Stop waiting for the GPU before parsing GPU-written macro arguments (**Pokémon Legends: Z-A**'s GPU-built indirect draws): accelerated macros refresh only the arguments they read on the CPU, and guest compute writes are ordered on the GPU before the draws and dispatches that consume them. NCE page-fault invalidation now checks and invalidates atomically, so a concurrently recorded GPU write is no longer dropped. Z-A's train station on AYN Thor runs at 30 FPS instead of ~24–27.
- Keep occlusion-query samples across command-buffer submissions. Each submit used to reset the guest sample counter, so a submit between a game's counter clear and its report read zero samples and hid the tested object for a frame (characters and NPCs vanishing in **Pokémon Legends: Z-A**). On Adreno and Mali drivers the samples counted so far are carried over at each submit instead of extending one host query indefinitely, which would have made every draw with counting enabled slower over time.
- Fix a crash at game start when update/DLC files sit next to the game in a shared folder (seen with **Super Smash Bros. Ultimate** on a Retroid Pocket Flip 2): emulated services opening those files called into Java from a fiber stack, which corrupts the thread's Java references; such calls now run on a real thread.
- Stop re-reading the installed firmware's version on every emulated service request. Each request checked its version gate by opening the SystemVersion archive three times (NAND lookup and NCA decryption), even for the functions that have no gate; with firmware installed this kept the service threads busy and delayed every request. **Super Smash Bros. Ultimate** battles on a Retroid Pocket Flip 2 run at 59–60 FPS instead of 44–50.
- Fix **Fire Emblem Engage**'s engage attacks (Lodestar Rush and others) turning every character and effect into flat white shapes on black on Adreno 6xx/7xx (AYN Thor, Odin 2). The game's effect shaders divide by fp16 subnormals; Maxwell keeps those, but these GPUs flush every fp16 subnormal to zero, so the division gave infinity and then NaN, which the bloom spread over the whole frame. The new **Fix FP16 Denormals** option (Graphics → Hacks) handles the subnormal range of fp32↔fp16 conversions in integer math on GPUs that cannot keep them. It is on automatically for Fire Emblem Engage and off for every other game, since it costs a few GPU instructions per conversion.
- Fix characters, lights and pillars glowing as white blobs in **Dragon Quest VII Reimagined** on Adreno 6xx/7xx (seen in the ship's hold at the start). Same cause as Fire Emblem Engage; **Fix FP16 Denormals** is now on automatically for it too.
- Fix buildings, signs and props blinking in and out in **Double Dragon Revive** (stage 1 right after starting a new game). Its Unreal renderer culls objects with occlusion queries and reads their results once the frame's fence signals; at the default fence behavior that fence was released on the CPU before the GPU had written the results, so visible objects read as occluded for a frame. For this game the fence report now waits until the GPU has run the work and the query results are in memory.

## 1.3.2.2

**New**

- "Dual-screen games not in your library" now also appears in the Carousel view, as greyscale cards after your games.

**Fixed**

- Save import and export (one game, or all saves) now use the profile you play as, matching what the dialog says; they always used the first profile.
- For companion makers: a companion reading the game's save file found nothing on Android, because the app looked under a stale account instead of the profile the game uses.

## 1.3.2.1

**Fixed**

- Eden Duo could close on start, and when returning from a game, with the library in the Compact grid or List view: a "Dual-screen games not in your library" tile was drawn with the Grid layout.

## 1.3.2

**New**

- The game library ends with "Dual-screen games not in your library": every game that has an Eden Duo companion but is not in your games folders, as a greyscale tile with its cover art and a "Not in library" chip. Tapping one explains how to add it; the tiles cannot be launched. The section stays with the "dual-screen games only" filter, is hidden while you search or sort by recently played/added, and can be switched off with "Show dual-screen games not in your library" in the library's sort/filter menu. Cover art comes from [GameTDB](https://www.gametdb.com) (downloaded once per game and kept; without a connection the tile shows the game's name instead). Shown in the grid, compact grid and list views, not in the carousel.
- In-game companion toggle: hide the companion during a game so the second screen can be used for another app, and bring it back where it was. Tap the game screen with four fingers, use the new "Companion" Quick Settings tile, or "Hide companion" in the in-game menu. The gesture can be changed to a 5-finger pinch or a 3-finger swipe down, or turned off (Settings -> Graphics -> Second Screen -> Companion toggle gesture); while a gesture is in progress its touches do not reach the game. Every game starts with the companion shown.

**Fixed**

- Based on the Eden nightly of 2026-10-19 (de7e6e01d4). It drops the per-request firmware version check that Eden added and removed again upstream: with firmware installed (for example a NAND folder shared with Eden), every system service call looked the version up from the NAND three times, which could slow games down.
- Importing save data no longer crashes Eden Duo. A zip from Eden (the per-game save export, or any zip with a folder named after the game's title ID, at any depth, in any letter case) imports; any other zip shows what is wrong, including "This save belongs to title ..." when it holds another game's saves. Only the game's own folder is unpacked, and the current saves are replaced only after that worked.
- "Import user data" no longer deletes the user folder before it has unpacked the backup: a damaged or unreadable zip now leaves your data and settings as they were and shows an error.
- A failure inside any progress dialog task (importing, exporting, installing) now shows an error instead of closing the app.
- A companion package whose manifest is larger than 4 MiB or is not valid JSON is no longer dropped without a word: the log names the file and why the package was not loaded.
- For companion makers: a font given as a member of an archive (`…FontData.szs#Title.bffnt`) now finds its own glyph sheets like a plain `.bffnt`, and a font without an atlas is reported in the log instead of silently falling back to the built-in font.
- For companion makers: `text_center_h` now centres a value widget's text vertically, as it already did for labels.
- The library's companion badges and the "not in your library" section no longer disappear when GitHub briefly answers with an empty release list; the games that come with Eden Duo always keep their badge.
- Import from another emulator is easier to follow: plain names for each kind of data, "Share with the other emulator" or "Separate copy" for every folder (saves default to a separate copy), and a confirmation that lists what happens to each folder. A running game is reported before the copy starts instead of after it, Restore shows when the backup was made and its size, and after an import with several user profiles Eden Duo asks which one you play as. Importing one game's save names the profile it goes into.
- Games such as Link's Awakening and Castlevania run at full speed again: includes Eden's fix for a slowdown from its multithreading rework (upstream #4548).
- For companion makers: a module can read a companion setting with `get_i64("@flag:<name>")` while it samples; it used to get the fallback value every time.

## 1.3.1

**Fixed**

- A memory leak in the RomFS builder: every archive build kept about 300 bytes alive. A game that asks the system for its firmware version thousands of times a second (VA-11 Hall-A) while Eden Duo cannot read the installed firmware grew by about 60 MB a minute until Android stopped it with an out-of-memory abort after one to two hours.
- The firmware version is now resolved once per game session instead of on every request, which removes the CPU cost of those requests altogether.
- When firmware is installed but its version archive cannot be decrypted (a prod.keys older than the firmware's key generation, for example firmware 23.x with keys from 22.x), the log now says so once and names the key generation, instead of silently using the built-in version.
- "No Companion" set to Off or App is now honoured when a frontend such as CocoonFE, ES-DE or Daijishō starts the game: the second-screen window is closed once the game is known, so the frontend's own artwork stays on that screen. Black already worked; Icon is unchanged.
- The in-game overlay names a release "Eden Duo 1.3.1" instead of showing a commit id; betas and dev builds keep the commit id.
- The second screen keeps its own brightness on AYN devices (Thor, Odin 2 Portal): Eden Duo no longer copies the main screen's brightness onto it, so the device's own second-screen slider works again while a game runs (eden-duo #8).
- Games started from ES-DE (and any frontend that hands the file over through its own FileProvider rather than the system document provider) load again. Since 1.3.0 they failed with "No bootable game present" because the file-exists check asked for a column such providers do not return.
- The scan for update and DLC files next to a game no longer asks the storage provider for every file's name a second time (the folder listing already carries it): one round trip per file less at launch. The same files are found as before.

## 1.3.0

**New**

- For companion makers (see the [Package Format](https://github.com/igawa6/eden-duo-companions/blob/main/docs/PACKAGE_FORMAT.md)):
  - Runtime 21: companions can show the device's battery level and charging state and the game's FPS, emulation speed and frame time. Module ABI 1 stays compatible with existing companions.
  - Runtime 22: a map widget can turn with the game's camera (`view_rotate_bind`, degrees clockwise): the map picture and its tiles rotate about the widget centre; markers, labels and pins keep their place but stay upright; a marker heading (`marker_rotate_bind`) turns with the map; taps, drags and pinches work on the turned map. Older Eden Duo versions ignore the key and show the map north-up.
  - Scroll regions can scroll on both axes (`axes`: `"x"`, `"y"` or `"xy"`, with `lock_axis`), take their content size from a value (`content_w`/`content_h`, `content_w_bind`/`content_h_bind`) so one large module-drawn picture scrolls natively with the finger, let a widget follow only one axis (`scroll_follow`: a frozen header row or label column), fling horizontally, show a horizontal scrollbar, and hand a tap's content-relative point to an action (`"payload": "$tap"`). Older runtimes ignore the keys and scroll vertically as before.
  - Text can lean like a game's italic menu text: `slant_deg` on any text widget (or `italic: true` for the common 9° lean) shears the glyphs about their baseline, in the game font and the built-in one. Older runtimes ignore the key and draw upright.
  - Map markers can animate on their own clock: `frames` + `frame_ms` cycle a marker's picture (a flickering pin flame), `pulse` breathes its size and alpha or grows a ring (the quest marker), on pan/zoom map pages too. Older runtimes draw the static icon.
  - `preload`: a list of picture sources the runtime decodes right after the package loads, so a page that uses many never-seen textures no longer pops them in one by one on its first visit.
  - Image widgets: `pan_bounds` limits pan/zoom to part of the picture (a padded picture can no longer be dragged to show its padding; the default view and the Reset button follow the same bounds). Older runtimes ignore the key.

**Fixed**

- For companion makers: a pan/zoom picture's view and a map's last-drawn view (including its turn) are now published for pages and readable by a native module from its periodic sample, so an overlay such as a player marker can stay in place while the picture or map is panned, zoomed or turned (`@view_zoom/pan_x/pan_y:<id>`, `@map_view_*:<area>` in the Package Format).
- Companion pictures inside a compressed game archive (Breath of the Wild's `Common.sblarc`) no longer cost ~70 ms each to decode: the archive is unpacked once and kept, so icons appear at once instead of one by one over a second.
- Images turned by exactly 90, 180 or 270 degrees draw several times faster (an integer axis swap instead of per-pixel trigonometry), which cuts the cost of a full-page rotated gradient.
- Starting another game while the previous one was still loading or closing no longer leaves the previous game's emulator (renderer, GPU resources, memory) alive in the background.
- A game no longer crashes in its first seconds when the device's motion sensor sends an event before the game has set up its controllers (seen on devices with a gyro, intermittently, in Crash Team Racing and Diablo III).
- Controller rumble no longer keeps one CPU core at 100% in the background (a rumble worker thread could spin forever once several threads sent rumble at the same time), which heated the device and caused throttling.
- Press-and-hold actions on the second screen now receive the widget's value (captured when the hold starts), the same way taps do; companions whose hold actions use `$payload` work again.
- **Trails through Daybreak II**: character and environment textures no longer go stale, flicker or disappear after the game remaps GPU memory. Thanks to tapionallanaire for the investigation and the fix (eden-duo PR #12).
- Games that draw to integer render targets now render correctly on all Vulkan drivers, not only on macOS (tapionallanaire, PR #12).
- Compute shaders that need more shared memory than the GPU provides now run in full instead of being cut short (seen in Trails through Daybreak II on Adreno; tapionallanaire, PR #12).

## 1.2.1

**Fixed**

- Pokémon Brilliant Diamond / Shining Pearl with Luminescent Platinum no longer stays on a black screen at start-up when the Pokémon companion is installed.
- Games now start normally when an installed companion doesn't support the game version; the companion shows its notice instead of the game failing to load ("Your ROM is encrypted").

Update: install over your current Eden Duo. Your games and settings are kept, and existing companions keep working.

- Requires Android 13+ (arm64) and a device with a second display.
- Companion packages for supported games: https://github.com/igawa6/eden-duo-companions

## 1.2.0

**New**

- **Eden Nightly.** Eden Duo now follows Eden's nightly builds, so upstream fixes and improvements reach you sooner, with an Eden Duo update at least every week. This release is based on Eden Nightly of October 8, 2026.
- **Pokémon Brilliant Diamond / Shining Pearl (including Luminescent Platinum)**, **Crash Team Racing Nitro-Fueled**, **Dragon Quest III HD-2D Remake** and **Chained Echoes** companion support.
- For companion makers (see the [Package Format](https://github.com/igawa6/eden-duo-companions/blob/main/docs/PACKAGE_FORMAT.md)):
  - Runtime 19: larger companion manifests and guest helpers, with module lifecycle corrections. Module ABI 1 stays compatible with existing companions.
  - Game-file/image decoding and map and scroll rendering corrections.

**Faster**

- Includes Eden's latest presentation and Vulkan improvements.

**Fixed**

- Large companions, such as Pokémon, now load completely.
- A damaged or incomplete companion package no longer replaces the companion you are using.

**Maybe fixed**

- Graphics glitches on some non-NVIDIA GPUs, thanks to an upstream Eden fix.

Update: install over your current Eden Duo. Your games and settings are kept, and existing companions keep working.

- Requires Android 13+ (arm64) and a device with a second display.
- Companion packages for supported games: https://github.com/igawa6/eden-duo-companions

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
