---
name: testing-switch-games-on-thor
description: Use when checking a Switch game's rendering, flicker, FPS, crashes or memory on the AYN Thor in Calamansi or Duet Plus / Eden Duo Dev, after installing a build or changing a game's settings, or when a game "crashed", "blinks", "flickers" or "is red/wrong colour" on the device.
---

# Testing Switch games on the AYN Thor

## Overview
Device evidence or it didn't happen. Single-frame defects (a HUD that vanishes for one frame) are invisible in screenshots: use a screen recording and check every frame. "Crashed" usually means Android's low-memory killer, not a native crash: read the kill records before touching renderer code.

## Device facts
| | |
|---|---|
| adb | `C:\Users\slash\AppData\Local\Android\Sdk\platform-tools\adb.exe`, serial `d884f526`; from Git Bash prefix device-path commands with `MSYS_NO_PATHCONV=1` |
| Top screen | input display `0`; screenshot `adb exec-out screencap -p -d 4630946441858561667` |
| Bottom screen | input display `4`; screenshot `-d 4630946482288158084` |
| Calamansi | `org.azahar_emu.azahar.fatesfix` (vanillaFatesFix build); Switch runs in `:engine_switch` |
| Duet Plus Dev | `com.tapionallanaire.duetplus.dev` (mainlineRelWithDebInfo build) |
| Controller | `/dev/input/event9`, A = key 304, D-pad ABS16/ABS17 (always release axes to 0) |

## Rules
- Install with `adb install -r` only. Never `pm clear`, uninstall, force-stop a running game, or touch saves.
- The user plays unless they have authorized scripted input for this session. Never install while a game is running.
- Release builds aren't debuggable: no `run-as`. Eden's log still reaches logcat as tag `YuzuNative` (boot lines like `Title defaults:` show which settings a game actually booted with).
- Battery: USB charges at ~900 mA, less than a game draws. Under ~20% the GPU clock drops (615 → ~400 MHz) and FPS numbers stop being comparable. Record the battery level with every measurement.

## Checks
| Question | How |
|---|---|
| What's on screen | screenshot both displays (ids above) |
| FPS | Eden's FPS overlay in the screenshot/recording; sample during battle, not idle |
| Flicker / missing HUD | `adb shell screenrecord --display-id 4630946441858561667 --time-limit 20 /sdcard/Download/rec.mp4`, pull it, then `python hud_check.py rec.mp4 out_dir` (this folder). It reports "isolated dropouts": frames missing the HUD with it shown on both sides. Other games: pass `--region name,x0,y0,x1,y1` per HUD panel (480x270 space). |
| Memory, GPU load/clock/temp, battery over time | `bash record_engine.sh org.azahar_emu.azahar.fatesfix:engine_switch out.tsv 3600` (background; read-only) |
| Why it died | `adb logcat -b crash -d` (native crash) and `adb logcat -d \| grep "lowmemorykiller.*Kill '"` plus `adb shell dumpsys activity exit-info org.azahar_emu.azahar.fatesfix` (`reason=3 (LOW_MEMORY)` with pss/rss) |
| GPU driver exit | logcat `TU:` / "impossible to have enough concurrent waves" (Turnip calls exit()) |

## Reporting
Numbers with conditions and evidence paths: mode (handheld/docked), resolution, async GPU on/off, battery %, FPS samples, dropout count out of total frames, peak RSS. Keep evidence under `C:\Users\slash\Development\reports\<topic>-<date>\`.

## Common mistakes
- Calling a fix good from a few screenshots: the HUD bug showed in 21 of 466 recorded frames and in none of the screenshots.
- Treating a low-memory kill as a renderer crash.
- Comparing FPS across battery levels or after the device got hot.
- Testing handheld while the game's tuned settings force Docked: set Docked off in the game's own settings.
