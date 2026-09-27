// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"

namespace {
constexpr uint64_t kTitleId = UINT64_C(0x0100000000000001);

EdenDsmodBool SupportsBuild(const char* build_id) {
    return build_id != nullptr && std::strcmp(build_id, "AAAAAAAAAAAAAAAA") == 0 ? EDEN_DSMOD_TRUE
                                                                                 : EDEN_DSMOD_FALSE;
}

void* Create(const EdenDsmodHostApi*, const char*) {
    return nullptr;
}

void Destroy(void*) {}
void Sample(void*, const EdenDsmodHostApi*) {}
void Tick(void*, const EdenDsmodHostApi*) {}

const EdenDsmodModuleApi kApi{
#ifdef DSMOD_BAD_INTERFACE
    .abi_version = EDEN_DSMOD_MODULE_ABI_VERSION + 1,
#else
    .abi_version = EDEN_DSMOD_MODULE_ABI_VERSION,
#endif
    .struct_size = sizeof(EdenDsmodModuleApi),
    .reserved = 0,
    .abi_hash = EDEN_DSMOD_MODULE_ABI_HASH,
    .title_id = kTitleId,
    .name = "DSMod loader test module",
    .capabilities = 0,
    .supports_build = SupportsBuild,
    .create = Create,
    .destroy = Destroy,
    .sample = Sample,
    .tick = Tick,
};
EdenDsmodBool LoadData(void*, const EdenDsmodHostApi*, const char* key, void* receiver,
                       EdenDsmodDataSink sink) {
    static constexpr char Blob[] = "module-data";
    if (key == nullptr || std::strcmp(key, "module:test:blob") != 0 || sink == nullptr) {
        return EDEN_DSMOD_FALSE;
    }
    sink(receiver, reinterpret_cast<const uint8_t*>(Blob), sizeof(Blob) - 1);
    return EDEN_DSMOD_TRUE;
}

const EdenDsmodModuleDataExtensions kData{
    .version = EDEN_DSMOD_DATA_EXT_VERSION,
    .struct_size = sizeof(EdenDsmodModuleDataExtensions),
    .abi_hash = EDEN_DSMOD_DATA_EXT_HASH,
    .load_data = LoadData,
};
} // namespace

// Runtime 12's optional data extension (the loader negotiates it like font/save/write).
extern "C" const EdenDsmodModuleDataExtensions* eden_dsmod_get_data_extensions(uint32_t version,
                                                                              uint64_t hash) {
    return version == EDEN_DSMOD_DATA_EXT_VERSION && hash == EDEN_DSMOD_DATA_EXT_HASH ? &kData
                                                                                       : nullptr;
}

extern "C" const EdenDsmodModuleApi* eden_dsmod_get_module(uint32_t host_abi_version,
                                                           uint64_t host_abi_hash) {
    if (host_abi_version != EDEN_DSMOD_MODULE_ABI_VERSION ||
        host_abi_hash != EDEN_DSMOD_MODULE_ABI_HASH) {
        return nullptr;
    }
    return &kApi;
}
