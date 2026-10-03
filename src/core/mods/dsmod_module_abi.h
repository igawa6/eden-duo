// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Stable C ABI between Eden's generic dual-screen host and a title-specific module.
//
// This header intentionally has no Eden/Core dependency.  A module may be built as a
// standalone shared object and loaded by dlopen()/LoadLibrary().  Pointers passed to a
// callback are borrowed for the duration of that callback only; a module must copy anything
// it needs to retain.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDEN_DSMOD_MODULE_ABI_VERSION 1u
#define EDEN_DSMOD_MODULE_ABI_HASH UINT64_C(0x8d3f5b1e6a70c429)

typedef struct EdenDsmodHostApi EdenDsmodHostApi;
typedef struct EdenDsmodModuleApi EdenDsmodModuleApi;

typedef uint32_t EdenDsmodBool;
enum {
    EDEN_DSMOD_FALSE = 0u,
    EDEN_DSMOD_TRUE = 1u,
};

enum {
    EDEN_DSMOD_LOG_DEBUG = 0u,
    EDEN_DSMOD_LOG_INFO = 1u,
    EDEN_DSMOD_LOG_WARNING = 2u,
    EDEN_DSMOD_LOG_ERROR = 3u,
};

enum {
    EDEN_DSMOD_CAP_WRITE_MEMORY = UINT64_C(1) << 0,
    EDEN_DSMOD_CAP_GUEST_CALL = UINT64_C(1) << 1,
    EDEN_DSMOD_CAP_ROMFS_READ = UINT64_C(1) << 2,
    EDEN_DSMOD_CAP_MAP_OUTPUT = UINT64_C(1) << 3,
    // (1 << 4) is EDEN_DSMOD_CAP_EXTENSIONS, dsmod_module_extensions.h.
    // Module behaviour flags (runtime 15+ hosts advertise both; an older host refuses a module
    // that sets either). While the second screen is hidden the host normally ticks a module only
    // when it exports the base extensions' on_action (it may have a guest mailbox to retire).
    // TICK_WHEN_HIDDEN asks for that hidden tick regardless; NO_TICK_WHEN_HIDDEN declines it
    // (a module whose tick is a full scan). A manifest "module_tick_hidden" overrides both.
    EDEN_DSMOD_CAP_TICK_WHEN_HIDDEN = UINT64_C(1) << 5,
    EDEN_DSMOD_CAP_NO_TICK_WHEN_HIDDEN = UINT64_C(1) << 6,
    // Asset sources (host capabilities). SOURCE_PREFIXES: read_romfs resolves "<prefix>:<path>"
    // through the host's source registry -- a path without a prefix is still a romfs path, an
    // unknown prefix returns 0 (never a romfs read of "<prefix>:..."), and
    // get_i64("__source:<prefix>") answers 1 (available), 0 (known but unavailable, e.g. no DLC
    // installed) or the fallback (not a source this host knows). SOURCE_BASE / SOURCE_AOC are
    // set while the "base:" (the unpatched program romfs) / "aoc:" (the title's add-on content
    // romfs) sources are registered.
    EDEN_DSMOD_CAP_SOURCE_PREFIXES = UINT64_C(1) << 7,
    EDEN_DSMOD_CAP_SOURCE_BASE = UINT64_C(1) << 8,
    EDEN_DSMOD_CAP_SOURCE_AOC = UINT64_C(1) << 9,
    // Runtime 17: set while the "user:" source is registered -- files the player puts in
    // <Eden data>/dualscreen/user/<TITLEID>/ (read-only; no "..", 32 MiB per file).
    EDEN_DSMOD_CAP_SOURCE_USER = UINT64_C(1) << 10,
    // Runtime 18 (host capability only -- a module must not set it in its own
    // capabilities, or older hosts refuse it): the host watches the module's published integer
    // "__font_epoch" (publish_i64, missing = 0) and calls decode_font again whenever it differs
    // from its value when decode_font last ran (a fresh bounded retry; the font in use stays
    // until the new one is decoded), then reloads the font atlas image and repaints. For a module
    // whose glyph set depends on state it learns late (the game language).
    EDEN_DSMOD_CAP_FONT_EPOCH = UINT64_C(1) << 11,
};

enum {
    EDEN_DSMOD_MAP_UNLOCKED = UINT32_C(1) << 0,
    EDEN_DSMOD_MAP_ZONE_INACTIVE = UINT32_C(1) << 1,
    EDEN_DSMOD_MAP_ZONE_ALERT = UINT32_C(1) << 2,
    EDEN_DSMOD_MAP_UPDATE_VISIBILITY = UINT32_C(1) << 8,
    EDEN_DSMOD_MAP_UPDATE_WATER = UINT32_C(1) << 9,
    EDEN_DSMOD_MAP_UPDATE_WALLS = UINT32_C(1) << 10,
    EDEN_DSMOD_MAP_UPDATE_POINTS = UINT32_C(1) << 11,
    EDEN_DSMOD_MAP_UPDATE_ZONE = UINT32_C(1) << 12,
};

enum {
    EDEN_DSMOD_POINT_PLAYER = UINT32_C(1) << 0,
    EDEN_DSMOD_POINT_ACTOR = UINT32_C(1) << 1,
    EDEN_DSMOD_POINT_CUSTOM = UINT32_C(1) << 2,
};

typedef struct EdenDsmodGuestCall {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t target;
    uint64_t args[8];
    uint64_t request_id;
} EdenDsmodGuestCall;

typedef struct EdenDsmodGuestResult {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t request_id;
    int64_t integer;
    double number;
    uint64_t address;
    const char* text;
} EdenDsmodGuestResult;

typedef struct EdenDsmodMapRect {
    float min_x;
    float min_y;
    float max_x;
    float max_y;
    uint32_t color;
    uint32_t flags;
} EdenDsmodMapRect;

typedef struct EdenDsmodMapPoint {
    float x;
    float y;
    uint32_t color;
    uint32_t flags;
    const char* icon;
    const char* name;
} EdenDsmodMapPoint;

typedef struct EdenDsmodMapTile {
    float x;
    float y;
    uint32_t type;
    uint32_t color;
} EdenDsmodMapTile;

// Dynamic state for one map publication.  All arrays and strings are borrowed until the
// publish_map callback returns.  Visibility uses the generic 0=unexplored, 1=revealed,
// 2=visited convention. ABI v1 uses the host's logical 650x300 fog grid; a module maps its
// game's cells into that grid using the package's world-space area bounds.
typedef struct EdenDsmodMapFrame {
    uint32_t struct_size;
    uint32_t flags;
    const char* area;
    uint32_t visibility_columns;
    uint32_t visibility_rows;
    const uint8_t* visibility;
    size_t visibility_count;
    const uint8_t* visibility_previous;
    const uint32_t* visibility_change_ticks;
    const EdenDsmodMapRect* water;
    size_t water_count;
    const EdenDsmodMapTile* walls;
    size_t wall_count;
    const EdenDsmodMapRect* occluders;
    size_t occluder_count;
    const EdenDsmodMapPoint* points;
    size_t point_count;
} EdenDsmodMapFrame;

struct EdenDsmodHostApi {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t reserved;
    uint64_t abi_hash;
    uint64_t capabilities;
    void* userdata;
    uint64_t title_id;
    uint8_t build_id[0x20];
    uint64_t main_base;
    uint64_t main_size;

    uint64_t (*get_tick)(void* userdata);
    uint64_t (*get_heap_begin)(void* userdata);
    uint64_t (*get_heap_end)(void* userdata);
    EdenDsmodBool (*is_mapped)(void* userdata, uint64_t address, uint64_t size);
    EdenDsmodBool (*read_memory)(void* userdata, uint64_t address, void* output, size_t size);
    const uint8_t* (*get_read_pointer)(void* userdata, uint64_t address, size_t size);
    EdenDsmodBool (*write_memory)(void* userdata, uint64_t address, const void* input, size_t size);
    void (*log)(void* userdata, uint32_t level, const char* message);

    // begin_output/end_output bracket a module's publish_* calls. The host currently provides
    // both as no-ops: each publish_* writes straight into the snapshot being sampled.
    void (*begin_output)(void* userdata);
    void (*publish_i64)(void* userdata, const char* name, int64_t value);
    void (*publish_f64)(void* userdata, const char* name, double value);
    void (*publish_text)(void* userdata, const char* name, const char* value);
    void (*publish_address)(void* userdata, const char* name, uint64_t value);
    void (*publish_map)(void* userdata, const EdenDsmodMapFrame* frame);
    void (*end_output)(void* userdata);

    // queue_guest_call/poll_guest_result belong to EDEN_DSMOD_CAP_GUEST_CALL. The host does not
    // provide them today: both are null and the capability is not advertised, so a module that
    // declares it is refused at load.
    uint64_t (*queue_guest_call)(void* userdata, const EdenDsmodGuestCall* call);
    EdenDsmodBool (*poll_guest_result)(void* userdata, EdenDsmodGuestResult* result);
    size_t (*read_romfs)(void* userdata, const char* path, uint64_t offset, void* output,
                         size_t size);
    int64_t (*get_i64)(void* userdata, const char* name, int64_t fallback);
    double (*get_f64)(void* userdata, const char* name, double fallback);
    const char* (*get_text)(void* userdata, const char* name);
};

struct EdenDsmodModuleApi {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t reserved;
    uint64_t abi_hash;
    uint64_t title_id;
    const char* name;
    uint64_t capabilities;

    EdenDsmodBool (*supports_build)(const char* build_id);
    // `host` is fully set up when create() runs: read_romfs (every registered source) and the
    // other readers work from inside it (runtime 16 guarantees it; a romfs that is not ready yet
    // is opened again on a later read instead of staying unavailable for the session).
    void* (*create)(const EdenDsmodHostApi* host, const char* config_json);
    void (*destroy)(void* instance);
    void (*sample)(void* instance, const EdenDsmodHostApi* host);
    void (*tick)(void* instance, const EdenDsmodHostApi* host);
};

typedef const EdenDsmodModuleApi* (*EdenDsmodGetModuleFn)(uint32_t host_abi_version,
                                                          uint64_t host_abi_hash);

// Every shared object exports this exact symbol.  Returning null rejects the module.
const EdenDsmodModuleApi* eden_dsmod_get_module(uint32_t host_abi_version, uint64_t host_abi_hash);

#ifdef __cplusplus
} // extern "C"
#endif
