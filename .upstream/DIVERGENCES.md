# Eden Duo: deliberate divergences from Eden upstream

Base: the Eden nightly commit in `.upstream/eden.json` (`source_commit`).
Ledger of every upstream file Duo touches: `.upstream/duo-touched.tsv`
(regenerate and check with `tools/duo/duo-touched-ledger.sh`).

This file lists the places where Duo intentionally differs from upstream in code
upstream owns, beyond the dual-screen hooks themselves. A sync that silently "fixes"
one of these breaks Duo, and a sync that keeps one after upstream fixed the same
problem keeps dead code. On every upstream sync, go through the list:

* still needed: keep it, update the location if it moved;
* upstream now does the same: drop the Duo change and remove the entry;
* upstream changed the surrounding code: re-check that the divergence still applies.

Line numbers are as of the Eden Duo 1.3.0 betas and drift; the symbols are the
stable reference.

## Kept on purpose

| # | Divergence | Where | Why | Sync risk |
|---|---|---|---|---|
| 1 | Upstream `STBI_WRITE_NO_STDIO` (#4282) not adopted; Duo defines `STBI_ONLY_PNG` there instead | `src/common/stb.h` (line ~10) | Duo still writes PNG files with `stbi_write_png` to files (`src/core/mods/`, eden-cli screenshots in `src/yuzu_cmd/dsmod_harness.cpp`, golden tests) | Conflicts whenever upstream edits `stb.h` |
| 2 | `unordered_dense` kept after upstream dropped it (#4326) | `cpmfile.json` (`unordered_dense` entry), `externals/CMakeLists.txt` (`AddJsonPackage(unordered-dense)`), `.patch/unordered-dense/`, `src/core/mods/duo_core.cmake` (`target_link_libraries(core PUBLIC unordered_dense::unordered_dense)`) | The dual-screen runtime's per-tick snapshots use the pinned dense map | Low for the link (no longer on upstream's `core` link line); `cpmfile.json` and `externals/CMakeLists.txt` are busy upstream files |
| 3 | eden-cli keeps its own `getopt_long` table and `PrintHelp` instead of upstream's unified parser (#4424, `Core::ParseLaunchParams`); about 117 lines upstream removed are restored | `src/yuzu_cmd/dsmod_harness.cpp` (`PrintHelp`, `ParseCommandLine`); `src/yuzu_cmd/yuzu.cpp` only calls `DSModHarness::ParseCommandLine` where upstream calls `ParseLaunchParams` | eden-cli needs Duo-only harness flags (dump, install, scripted input, aux window), and harness scripts rely on the pre-#4424 semantics that upstream's parser changed. Layering the Duo flags on `ParseLaunchParams` was evaluated for 1.3.0 and rejected, see "eden-cli option parser" below | Medium: the parser no longer conflicts (it is in a Duo file), but upstream parser changes and new flags never reach eden-cli. Re-check on each sync whether upstream fixed the items below |
| 4 | Kernel `application_process` guarded by a lock plus an epoch counter | `src/core/hle/kernel/kernel.{h,cpp}` (`SetApplicationProcess`, `RetainApplicationProcess`, `IsApplicationProcess`, `application_process_epoch`) | The companion runtime reads the application process from other threads and must detect a process swap; also closes an upstream race | Medium |
| 16 | `PerfStats` keeps non-resetting totals (game frames, system frames, accumulated frametime) next to its interval counters, read with `PerfStats::GetTotals` | `src/core/perf_stats.{h,cpp}` (`Totals`, `GetTotals`, `total_*` members bumped in `EndSystemFrame` / `EndGameFrame`; marked "Eden Duo") | Runtime 21 `@perf.*` (`src/core/mods/mod_host_values.*`) samples FPS, speed and frametime from deltas without calling `GetAndResetStats`, which the Android overlay, the SDL title and the harness reset | Low; additive hunks. If upstream adds its own non-destructive accessor, switch the sampler to it and drop these |

## Android resource and Gradle overlays

Upstream's Android branding files are not edited; Duo overrides them instead, so a
sync never conflicts on them but also never shows when upstream changes them.

* `src/android/app/build.gradle.kts` ends with `apply(from = "duo.gradle")`.
  `duo.gradle` (Groovy, because a Kotlin script plugin cannot see the Android Gradle
  plugin classes) sets the application id, version name, CMake flags
  (`ENABLE_UPDATE_CHECKER=OFF`, release `EDEN_DSMOD_BUILD_DEV_TOOLS=OFF`,
  `YUZU_DOWNLOAD_ANDROID_VVL=OFF`, file-prefix maps), the relWithDebInfo "Dev"
  identity, the mainline "Eden Duo" name, the release VVL packaging exclude, test
  dependencies and the lint baseline. On a sync, check that upstream's
  `defaultConfig`, `release`/`relWithDebInfo` build types, flavor names and
  `productFlavors.all` app-name block still exist under the same names.
* `src/android/app/src/duo/res` is added to every product flavor's source set and
  overrides `src/main/res`: launcher/splash/about images, both `fragment_about.xml`
  layouts, `ic_launcher_background`, and `addon_notice_description` /
  `invalid_directory_description`. Sync risk: when upstream changes one of these
  files (new about-page views or ids, reworded add-on strings), compare and carry the
  change into the overlay by hand.
* Duo-added strings and arrays live in `values/duo_strings.xml` and
  `values/duo_arrays.xml` (English only, `tools:ignore="MissingTranslation"`).
* `src/android/app/lint-baseline.xml` lists the lint errors in upstream-owned code
  and generated SDL sources only. Regenerate it after a sync (delete it, run
  `:app:lintMainlineRelease` twice, keep only the error entries) and never baseline
  an error in a Duo-owned file.

## Generic fixes carried on top of upstream

Not Duo features. Each should stay a separate, self-contained change so a sync can
drop it as soon as upstream fixes the same problem.

| # | Fix | Where | Sync risk |
|---|---|---|---|
| 5 | Screenshot request race: renderers claim a pending request via `RendererBase::TakePendingScreenshot` | `src/video_core/renderer_base.{h,cpp}`, `renderer_opengl.cpp`, `renderer_vulkan.cpp` (`RenderScreenshot`) | Medium; the aux window depends on it |
| 6 | ThreadSanitizer fiber annotations (`EDEN_HAS_TSAN`) | `src/common/fiber.cpp` | Low |
| 7 | Debugger `ConnectionState` constructs the Boost.Process signal pipe in place (copying the v1 pipe could assign invalid descriptors) | `src/core/debugger/debugger.cpp` (`ConnectionState`) | Medium |
| 8 | `HostMemory::Unmap` range check as an overflow-safe `ASSERT_MSG` with the offending range | `src/common/host_memory.cpp` | Low |
| 9 | Test fixes | `src/tests/core/core_timing.cpp`, `src/tests/video_core/memory_tracker.cpp` | Low |
| 10 | Dynarmic code-page cache invalidated through an atomic `code_gen` counter (bumped by `ClearInstructionCache`/`InvalidateCacheRange`, possibly from another thread) instead of a cross-thread write to `last_code_addr` | `src/core/arm/dynarmic/arm_dynarmic_{32,64}.{h,cpp}` (`DynarmicCallbacks*::MemoryReadCode`) | Medium; regenerate the ledger, the headers become upstream hooks |
| 11 | SDL frontend lifetime fixes: `*appstate = state`, `SDL_APP_CONTINUE` from `SDL_AppInit`/`SDL_AppEvent`, the input subsystem kept in `SdlState` (upstream's is a local that dies when `SDL_AppInit` returns) and passed to `System::SetInputSubsystem`, the shader-reload callback capturing `state` by value, and `SDL_AppQuit` tolerating early exits (`IsPoweredOn` guard) | `src/yuzu_cmd/yuzu.cpp` (`SdlState`, `SDL_AppInit`, `SDL_AppEvent`, `SDL_AppQuit`) | Low; drop each part once upstream fixes it |
| 12 | Android `EmulationSession` shader-cache reload race: `m_pending_shader_cache_title` cleared on init/shutdown/halt, `RequestDiskShaderCacheReload` ignored while not running; "Emulation stopped" log line | `src/android/app/src/main/jni/native.cpp` | Low |
| 13 | Android SAF callbacks release their JNI local references; `GetParentDirectory`/`GetFilename` return `{}` instead of constructing a string from `0` | `src/common/fs/fs_android.cpp` | Low |
| 14 | Launch speed with large linked update/DLC folders: `ExternalContentProvider` scans on first lookup instead of in its constructor (UI thread); `MainActivity`'s startup key check uses `areKeysLoaded()` instead of `areKeysPresent()` (which rebuilds every factory); persistent container index (Duo-owned `file_sys/duo_external_content_index.*`); SAF listings carry file sizes | `src/core/file_sys/registered_cache.{h,cpp}`, `src/core/file_sys/vfs/vfs_real.cpp`, `src/common/fs/fs_android_documents.h`, `ui/main/MainActivity.kt` | Medium; keep the hooks thin |
| 15 | PR #12 (tapionallanaire) rendering fixes: invalidate GPU remaps to a different backing address (`MemoryManager::PageTableOp`/`BigPageTableOp`), recreate sampled views of remapped images (`TextureCache::VisitImageView`), fragment output numeric types for integer render targets on all Vulkan drivers (`MakeRuntimeInfo`), graphics+compute stages in upload barriers (`CopyBufferToImage`), and `PrivatizeInvocationSharedMemory` (gated on `HostTranslateInfo::max_compute_shared_memory_size`) | `src/video_core/memory_manager.cpp`, `texture_cache/texture_cache.h`, `renderer_vulkan/vk_pipeline_cache.cpp`, `vk_texture_cache.cpp`, `renderer_opengl/gl_shader_cache.cpp`, `src/shader_recompiler/frontend/maxwell/translate_program.cpp`, `host_translate_info.h` | Medium; candidates for upstream only via a human-written resubmission (Eden does not take AI-assisted code) |
| 17 | Android JNI attach off fiber stacks: CPU core threads attach to the JVM in `CpuManager::RunThread` before `Fiber::ThreadToFiber`, and `GetEnvForThread` refuses (logs) an attach from outside the thread's own stack. ART takes the current stack pointer as the stack top on attach and `madvise(MADV_DONTNEED)`s up to it, so the first JNI call of a core thread from an HLE service fiber (SAF reopen of a lazily opened update/DLC container, e.g. `aoc:u` building its DLC list on a cold front-end launch) zeroed every thread stack above and killed the process without a tombstone | `src/core/cpu_manager.cpp` (`RunThread`), `src/common/android/id_cache.cpp` (`IsOnOwnThreadStack`, `GetEnvForThread`) | Low; additive. Keep while any HLE code makes JNI calls on fibers |
| 18 | NCE host-fault forwarding: `ArmNce::Initialize` now saves the previous SIGBUS action into `g_orig_bus_action` (it passed `nullptr` and left it unset), and `HandleHostAlignmentFault`/`HandleHostAccessFault` route through a shared helper that falls back to restoring `SIG_DFL` and re-raising when no real previous handler was captured, instead of calling through a null `sa_sigaction` | `src/core/arm/nce/arm_nce.cpp` (`ForwardHostFaultOrCrash`, `ArmNce::HandleHostAlignmentFault`, `ArmNce::HandleHostAccessFault`, `ArmNce::Initialize`) | Low; additive. Drop once upstream fills in the SIGBUS old-action itself |
| 19 | `scm_rev.cpp`'s git commit/branch is only resolved when CMake (re)configures (`include(GenerateSCMRev)` in `src/common/CMakeLists.txt`, a plain `configure_file`); an incremental build after new commits never reconfigures, so the in-game overlay/about page's build id goes stale. `CMAKE_CONFIGURE_DEPENDS` is appended with the resolved per-worktree `HEAD` file and the common dir's branch ref / `packed-refs`, so a build notices HEAD moved and regenerates `scm_rev.cpp` (a worktree's `.git` is a file pointing elsewhere, resolved via `git rev-parse --git-dir`/`--git-common-dir`) | `CMakeModules/GenerateSCMRev.cmake` | Low; additive, guarded by `if (GIT_FOUND)`. Drop if upstream starts watching the git ref files itself |
| 20 | UI-thread stall on a cold intent launch (a frontend/shortcut starting `EmulationActivity` directly, bypassing the library's own background scan): `EmulationFragment.onCreate()` used to call `GameHelper.getGame()` inline, which can trigger `ExternalContentProvider::EnsureScanned` (see #14) on this thread. Resolved off the UI thread (`resolveIntentGameAsync`, `continueGameSetupAfterIntentResolved`); `surfaceChanged`'s first-surface start was factored out to `startEmulationForNewlyValidSurface` so a late-registered `SurfaceHolder` callback (the surface can turn valid while the resolve is in flight) still starts emulation (`completeViewSetupCatchUpSurface`) | `src/android/app/src/main/java/org/yuzu/yuzu_emu/fragments/EmulationFragment.kt` | Low; refactor-only for the library-launch path, additive for the intent-launch path |
| 21 | Android vibration worker: `vibration_queue` is an `MPSCQueue` (several host threads push through `SetVibration`; the single-producer queue got corrupted and `PopWait` stopped blocking, spinning one core at 100%), `SendVibrations` returns on stop without acting, and identical consecutive intensities per device are coalesced | `src/input_common/drivers/android.{h,cpp}` | Low |
| 22 | Android `EmulationSession` run cycles are serialised: `BeginEmulationCycle` (static `RunEmulation`) waits up to 60 s for the previous cycle's `ShutdownEmulation`, `EndEmulationCycle` runs after it. Without it a game started while the previous one was still loading or shutting down reused the shared `m_is_running`, stranded the old `NativeEmulation` thread in its wait loop and never tore down the old renderer/GPU/process (duplicate `VulkanPresent`/`VkPipelineBuild` threads, leaked VRAM) | `src/android/app/src/main/jni/native.{h,cpp}` | Low |
| 23 | `NPad::ControllerUpdate` returns when the HID applet resource does not exist yet: a frontend input event (Android's motion sensor, fed from `EmulationActivity.onSensorChanged` as soon as emulation runs) could reach it during the first seconds of a boot and dereference a null `applet_resource` (SIGSEGV at launch on a device with a gyro; seen on Crash Team Racing and Diablo III) | `src/hid_core/resources/npad/npad.cpp` | Low |
| 24 | `RomFSBuildContext` unlinks its directory/file contexts on destruction (`Unlink()`): the contexts link parent <-> child/sibling/file with `shared_ptr`, so every `CreateRomFS` build (system archive synthesis, LayeredFS) left its context tree alive, 288 bytes per build. A game polling `set:sys GetFirmwareVersion` at kHz rates with unreadable firmware grew ~60 MB/min until Scudo aborted (user report, 1.3.1) | `src/core/file_sys/fsmitm_romfsbuild.{h,cpp}` (`~RomFSBuildContext`, `Unlink`); tests `src/tests/core/mods/firmware_version_leak.cpp`, `firmware_version_nca_path.cpp` | Upstream has the same cycle (byte-identical file as of 2026-10-16 master). Drop when upstream breaks the cycle (weak parent links or an unlink) | Low: a Duo-only destructor body in a rarely touched file |
| 25 | `ISystemSettingsServer` caches the resolved `FirmwareVersionFormat` per service instance (`GetCachedFirmwareVersion`, `m_firmware_version`) instead of re-reading the firmware NCA or synthesizing the SystemVersion archive on every `GetFirmwareVersion[2]`; `GetFirmwareVersionImpl` logs once when the installed firmware's SystemVersion NCA is unreadable (status + key generation) | `src/core/hle/service/set/system_settings_server.{h,cpp}` | Games poll the version thousands of times a second (VA-11 Hall-A: 1.2-3.2k/s); upstream rebuilds per call. Drop the cache if upstream adds one; keep the warning unless upstream reports the key mismatch | Medium: `system_settings_server.cpp` is a busy upstream file (function table style already differs) |
| 26 | Android import crashes and data loss: `TaskViewModel.runTask` runs the task through `runGuardedTask` (a throwing task ends with a `DuoTaskFailure`, which `ProgressDialogFragment` shows as a dialog; upstream lets it crash the app); the per-game save import (`GamePropertiesFragment.importSaves`) no longer toasts from the IO thread (crash on every error), finds the title folder anywhere in the zip case-insensitively, unpacks only that folder to a staging dir and swaps it in afterwards (`duo/SaveZipImport.kt`); "Import user data" (`InstallableActions.importUserData`) checks and unpacks into a staging dir next to the user folder before it unloads the config and replaces the user folder, instead of deleting the user folder first; the save-import confirmation names the profile the saves go to (`duo_save_import_profile`) | `model/TaskViewModel.kt`, `fragments/ProgressDialogFragment.kt`, `fragments/GamePropertiesFragment.kt` (`importSaves`), `utils/InstallableActions.kt` (`importUserData`) under `src/android/app/src/main/java/org/yuzu/yuzu_emu/` | Same bugs in upstream stable v0.2.1 and nightly `de7e6e01d4`. Medium: `importSaves`/`importUserData` bodies are replaced; on a sync keep the Duo bodies unless upstream rewrote the import, then re-check it for the three crash paths |
| 27 | In-game companion toggle (1.3.2): `InputOverlay` gets a `touchscreenFilter` hook so the toggle gesture (`CompanionToggle.GameTouch`) is kept away from the emulated touchscreen, and the in-game menu gets a hide/show companion item (`menu_toggle_companion`); `EmulationFragment` handles that item, renames it (`updateCompanionMenuEntry`) and wires `touchscreenFilter` to `dualScreen.gameTouch` in `onResume` | `src/android/app/src/main/java/org/yuzu/yuzu_emu/overlay/InputOverlay.kt`, `src/android/app/src/main/res/menu/menu_in_game.xml`, `src/android/app/src/main/java/org/yuzu/yuzu_emu/fragments/EmulationFragment.kt` (3 hooks) | Low: one nullable hook field + one condition, and one menu item; on a sync re-apply both if upstream rewrote the touch path or the menu |
| 28 | Selected-profile consistency (1.3.2.2): `ProfileManager::ResetUserSaveFile` re-opens the selected user like the constructor (a System manager built before the Android user folder was known kept a random last-opened user, so the companion save reader read the wrong account); Android `getSavePath` / `getDefaultProfileSaveDataRoot` use the selected profile instead of profile 0 | `src/core/hle/service/acc/profile_manager.cpp`, `src/android/app/src/main/jni/native.cpp` | Low: one block after ParseUserSaveFile, two user lookups; on a sync re-apply if upstream rewrote profile loading or the save-path JNI |
| 29 | Carousel with the not-in-library section (1.3.2.2): `CarouselRecyclerView` finds the GameAdapter inside the library's ConcatAdapter (`gameAdapterOf`) for card size/padding/setup and uses `absoluteAdapterPosition` for per-card scale | `src/android/app/src/main/java/org/yuzu/yuzu_emu/views/CarouselRecyclerView.kt` | Low: one helper replacing six `adapter as? GameAdapter` casts and one position call; on a sync re-apply if upstream rewrote the carousel |

## eden-cli option parser

The eden-cli harness (option parsing, `--dump-il2cpp`/`--dump-romfs`/`--install`, screenshots,
scripted input, aux window) lives in `src/yuzu_cmd/dsmod_harness.{h,cpp}`. `yuzu.cpp` keeps
upstream's layout and calls `DSModHarness::ParseCommandLine`, `ResolveSelectedUser`,
`OnGameLoaded`, `Iterate` and `HandleEvent`.

Putting the Duo flags in front of upstream's `Core::ParseLaunchParams` (Duo flags consumed
first, the rest passed through) would change flags the harness scripts use. In upstream's
parser at the recorded base:

* `-d/--debug` is `no_argument` but reads `optarg`: `-d <port>` (the BOTW harness) passes a
  null pointer to `atoi`.
* `-i/--input-profile` is `no_argument` but builds a `std::string` from `optarg` (null).
* `-x/--filter` is `no_argument` and takes the next `argv` entry; `--filter=<spec>` is rejected.
* `-p/--program` is `optional_argument` and also takes the next `argv` entry, so
  `--program=<args>` swallows the following argument.
* `-h/--help` and `-v/--version` only log and do not exit; the run then fails with "No ROM
  specified" (the `cli_help`/`cli_version` tests expect exit 0 and the text on stdout).
* `-u <name>` is looked up before the config is loaded; Duo resolves it after `SdlConfig`.
* A malformed `--multiplayer` is ignored instead of printing the usage and failing.

Fixing those means editing upstream's `src/core/launch_params.cpp`, which is shared with the Qt
frontend; that is a separate upstream fix, not a Duo change. Revisit when upstream's parser
handles the cases above; then `ParseCommandLine` can strip the Duo-only flags (`-a`, `-V`,
`-D`, `-R`, `-I`, `-S`) and pass the rest to `ParseLaunchParams`.

## Android hook layout

Upstream Android files keep one-line hooks; the Duo logic sits in Duo-owned files:

* JNI: `jni/dsmod_native.{h,cpp}` (`DSModSession::*` hooks called from `native.cpp`, and the
  Duo `Java_..._NativeLibrary_*` / `NativeInput_onAuxTouchEvent` entry points). The Kotlin
  declarations stay in `NativeLibrary.kt` (one block at the end), so JNI names are unchanged.
* SAF document operations: `src/common/fs/fs_android_documents.h` (header-only, included by
  `fs_android.h`, registered from `RegisterCallbacks`).
* Kotlin: `org.yuzu.yuzu_emu.duo.*` controllers, owned by the upstream fragment, view model or
  helper that calls them.

When a sync conflicts in one of these upstream files, keep the hook line and re-check that it
still runs at the same point of the upstream flow.

## Accidental drift (to revert in a source change)

Formatting-only differences with identical tokens. They have no purpose and cause
conflicts; restore upstream's formatting the next time these files are edited.

None currently known.

Rule: format only Duo-owned paths; never run `clang-format` over whole upstream files.

## Duo build lists live in Duo-owned CMake files

Upstream CMake files carry one `include()` line each (commented "Eden Duo"); the
source lists, test targets and Duo link libraries are in the included file. When a
sync conflicts near one of these lines, keep the include and take upstream's side
of everything else.

| Upstream file | Duo file |
|---|---|
| `src/core/CMakeLists.txt` | `src/core/mods/duo_core.cmake` |
| `src/tests/CMakeLists.txt` | `src/tests/core/mods/duo_tests.cmake` |
| `src/video_core/CMakeLists.txt` | `src/video_core/dsmod/duo_video_core.cmake` |
| `src/video_core/host_shaders/CMakeLists.txt` | `src/video_core/host_shaders/duo_shaders.cmake` |
| `src/yuzu_cmd/CMakeLists.txt` | `src/yuzu_cmd/duo_cmd.cmake` |

## Known upstream API changes to handle at the next sync

* Upstream #4480 (`218c670`, after base `4dc69c06`) replaced
  `static const FunctionInfo functions[]` + `RegisterHandlers(functions)` with a
  per-service `FindRequest` override over a `CreateStaticMap` table.
  `src/core/hle/service/dsm/dsm.cpp` (Duo-only) carries both forms: the
  `FindRequest` form is compiled when `DUO_HLE_STATIC_DISPATCH` is defined, which
  `src/core/mods/duo_core.cmake` does for that one file when
  `src/core/hle/service/service.h` contains `CreateStaticMap` (re-checked on every
  configure after `service.h` changes). Verified: builds on the base and on a trial
  merge of upstream `a594c8d`. Once `.upstream/eden.json` points at a nightly that
  includes `218c670`, delete the `#else` `RegisterHandlers` branch in `dsm.cpp` and the
  detection block in `duo_core.cmake` (define nothing, keep the `FindRequest` form).
  Upstream reverted #4480 in #4532 (`e255c20`, nightly `de7e6e01d4`), so both forms are
  kept again: drop one only after upstream has settled on a form for more than one nightly.
