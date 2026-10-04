# Shared module SDK

This directory retains `dsmod_module_sdk.h`, the generic header-only companion SDK.
The base ABI and optional extension headers remain one directory above.

Game-specific modules, tests and build targets now live in
[igawa6/eden-duo-companions/native](https://github.com/igawa6/eden-duo-companions/tree/main/native).
Follow that repository's native build guide and select this checkout with `EDEN_SOURCE_ROOT`.

Source for releases before the move remains available through their immutable release tags.
No game module is linked into the emulator or APK.
