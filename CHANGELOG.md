# Changelog

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
