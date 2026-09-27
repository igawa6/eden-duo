# Launching Eden Duo from a frontend

Ready-made configuration files that add Eden Duo (`dev.igawa6.edenduo`) and the Eden Duo Dev build
(`dev.igawa6.edenduo.dev`) as Nintendo Switch emulators. Both launch a game directly into
`org.yuzu.yuzu_emu.activities.EmulationActivity`, the same way the stock Eden entries do.

## CocoonFE

`cocoonfe/NintendoSwitch.json` is the Nintendo Switch platform file from
[CocoonFE](https://github.com/inssekt/CocoonFE/blob/main/platforms/NintendoSwitch.json), with
"Eden Duo" and "Eden Duo Dev" added at the top of the player list. Import it as the Nintendo Switch
platform in CocoonFE, then pick **Eden Duo** as the player.

## ES-DE (Android)

`es-de/es_systems.xml` and `es-de/es_find_rules.xml` are the custom-system files from
[es-de-android-custom-systems](https://github.com/GlazedBelmont/es-de-android-custom-systems), with an
`EDEN-DUO` / `EDEN-DUO-DEV` emulator added to the find rules and "Eden Duo (Standalone)" as the first
(default) command of the `switch` system.

Copy both files into `ES-DE/custom_systems/` on the device and restart ES-DE. To pick the emulator per
system or per game, open the system's **Alternative emulator** option.
