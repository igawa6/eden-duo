---
name: tuning-switch-game-settings
description: Use when a Switch game needs its best settings found or changed for the AYN Thor, when adding or editing a game's "Apply optimal settings for this game" entry, or when a game runs slow, flickers, shows wrong colours or crashes under its current settings in Calamansi or Duet Plus.
---

# Tuning a Switch game's optimal settings

## Overview
Tuned settings live in one C++ table per Eden branch. At boot they fill only settings the game leaves on global. "Apply optimal settings for this game" writes them into the game's own config, so explicit per-game values get replaced too. Change one setting at a time, and prove each change on the device with **testing-switch-games-on-thor**.

## Where things are
| | |
|---|---|
| Table | `src/core/game_settings.cpp` → `TitleSettings(program_id, force)`; title IDs in `game_settings.h` (`TitleID`). A new title also goes in `HasOptimalSettings`. |
| Non-switchable per-title hacks | `LoadOverrides` in the same file (e.g. `fix_fp16_denormals_by_title`); these aren't user settings. |
| Calamansi engine branch | Eden Duo `calamansi` branch, worktree `C:\Users\slash\Development\eden-duo-calamansi` |
| Duet Plus branch | Eden Duo `duet-plus` line, checkout `C:\Users\slash\Development\eden-duo`. Its tables can differ from `calamansi`; port each change on purpose. |
| Calamansi host | `C:\Users\slash\Development\calamansi-hd-textures-20261021`; engine staged from `externals/nintendo/eden-duo` by `src/android/engines/switch/prepare.py` |
| Menu item | Calamansi: Properties → Game settings → Advanced → Apply optimal settings. Duet Plus: game properties → Apply optimal settings. It refuses while a game is running. |

## Steps
1. Measure the current state first (FPS, flicker, memory) with **testing-switch-games-on-thor**.
2. Edit the title's case in `TitleSettings` with `DefaultForTitle(Settings::values.<setting>, <value>);`, plus a comment giving the reason (the symptom it fixes).
3. Commit on `calamansi` (no AI attribution, no `--no-verify`).
4. Calamansi:
   - `git -C externals/nintendo/eden-duo fetch <local eden-duo checkout> calamansi`, then check out the commit.
   - Set `PIN` in `prepare.py` to the same hash. The pin and the gitlink move together.
   - Run `python prepare_check.py` (in `engines/switch`), then build `assembleVanillaFatesFix` in the background (see the repo's CLAUDE.md for the environment).
5. Duet Plus: cherry-pick onto its branch, resolving table conflicts deliberately, then build `assembleMainlineRelWithDebInfo`.
6. `adb install -r`. Apply the optimal settings from the menu, launch, and re-measure.
7. Don't push or release unless asked. No GitHub Actions for releases.

## Judging a setting
- Correct picture first. Never trade it for speed: lower resolution, blinking, a missing HUD or wrong colours are failures, not fixes.
- "Locked 30" means sustained in battles, at 1x, in both handheld and docked.
- Async GPU emulation gives the most FPS on Adreno. If it shows single-frame defects, look for an ordering bug (buffers reused before their GPU work ran) before going back to sync.
- Bloom hack, FXAA, accuracy and docked mode each change both looks and cost. Test them one at a time.

## Common mistakes
- Expecting a new table value to apply to a game that already has an explicit value: boot defaults only fill globals. Use the menu item.
- Kotlin "Unresolved reference" after a branch or pin change: delete that variant's `build/kotlin/compile*Kotlin` and `build/tmp/kotlin-classes/*` caches.
- CRLF files: Eden sources mix line endings, so check edits applied (`git diff --stat`).
- Forgetting `HasOptimalSettings`: the menu item stays hidden.
