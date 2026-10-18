// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/android/android_common.h"
#include "common/android/id_cache.h"
#include "common/assert.h"
#include "common/fs/fs_android.h"
#include "common/string_util.h"

namespace Common::FS::Android {

void RegisterCallbacks(JNIEnv* env, jclass clazz) {
    env->GetJavaVM(&g_jvm);
    native_library = clazz;

    s_get_parent_directory = env->GetStaticMethodID(native_library, "getParentDirectory",
                                                    "(Ljava/lang/String;)Ljava/lang/String;");
    s_get_filename = env->GetStaticMethodID(native_library, "getFilename",
                                            "(Ljava/lang/String;)Ljava/lang/String;");
    s_get_size = env->GetStaticMethodID(native_library, "getSize", "(Ljava/lang/String;)J");
    s_is_directory = env->GetStaticMethodID(native_library, "isDirectory", "(Ljava/lang/String;)Z");
    s_file_exists = env->GetStaticMethodID(native_library, "exists", "(Ljava/lang/String;)Z");
    s_open_content_uri = env->GetStaticMethodID(native_library, "openContentUri",
                                                "(Ljava/lang/String;Ljava/lang/String;)I");
    RegisterDocumentCallbacks(env, native_library); // Eden Duo: fs_android_documents.h
}

void UnRegisterCallbacks() {
    UnRegisterDocumentCallbacks(); // Eden Duo
    s_get_parent_directory = nullptr;
    s_get_filename = nullptr;

    s_get_size = nullptr;
    s_is_directory = nullptr;
    s_file_exists = nullptr;

    s_open_content_uri = nullptr;
}

bool IsContentUri(const std::string& path) {
    constexpr std::string_view prefix = "content://";
    if (path.size() < prefix.size()) [[unlikely]] {
        return false;
    }

    return path.find(prefix) == 0;
}

s32 OpenContentUri(const std::string& filepath, OpenMode openmode) {
    if (s_open_content_uri == nullptr)
        return -1;

    const char* mode = "";
    switch (openmode) {
    case OpenMode::Read:
        mode = "r";
        break;
    case OpenMode::Write:
    case OpenMode::WriteTruncate: mode = "wt"; break;
    case OpenMode::ReadWrite: mode = "rw"; break;
    case OpenMode::WriteAppend: mode = "wa"; break;
    case OpenMode::ReadWriteAppend: mode = "rw"; break;
    case OpenMode::ReadWriteTruncate: mode = "rwt"; break;
    default: return -1;
    }
    return WithJni([&](JNIEnv* env) {
        jstring j_filepath = Common::Android::ToJString(env, filepath);
        jstring j_mode = Common::Android::ToJString(env, mode);
        const auto result =
            env->CallStaticIntMethod(native_library, s_open_content_uri, j_filepath, j_mode);
        env->DeleteLocalRef(j_filepath);
        env->DeleteLocalRef(j_mode);
        return result;
    });
}

u64 GetSize(const std::string& filepath) {
    if (s_get_size == nullptr) {
        return 0;
    }
    return WithJni([&](JNIEnv* env) {
        const auto path = Common::Android::ToJString(env, filepath);
        const auto result = env->CallStaticLongMethod(native_library, s_get_size, path);
        env->DeleteLocalRef(path);
        return static_cast<u64>(result);
    });
}

bool IsDirectory(const std::string& filepath) {
    if (s_is_directory == nullptr) {
        return 0;
    }
    return WithJni([&](JNIEnv* env) {
        const auto path = Common::Android::ToJString(env, filepath);
        const auto result = env->CallStaticBooleanMethod(native_library, s_is_directory, path);
        env->DeleteLocalRef(path);
        return static_cast<bool>(result);
    });
}

bool Exists(const std::string& filepath) {
    if (s_file_exists == nullptr) {
        return 0;
    }
    return WithJni([&](JNIEnv* env) {
        const auto path = Common::Android::ToJString(env, filepath);
        const auto result = env->CallStaticBooleanMethod(native_library, s_file_exists, path);
        env->DeleteLocalRef(path);
        return static_cast<bool>(result);
    });
}

std::string GetParentDirectory(const std::string& filepath) {
    if (!s_get_parent_directory) return {};
    return WithJni([&](JNIEnv* env) -> std::string {
        const auto path = Common::Android::ToJString(env, filepath);
        const auto value = static_cast<jstring>(
            env->CallStaticObjectMethod(native_library, s_get_parent_directory, path));
        env->DeleteLocalRef(path);
        if (!value) return {};
        const auto result = Common::Android::GetJString(env, value);
        env->DeleteLocalRef(value);
        return result;
    });
}

std::string GetFilename(const std::string& filepath) {
    if (!s_get_filename) return {};
    return WithJni([&](JNIEnv* env) -> std::string {
        const auto path = Common::Android::ToJString(env, filepath);
        const auto value = static_cast<jstring>(
            env->CallStaticObjectMethod(native_library, s_get_filename, path));
        env->DeleteLocalRef(path);
        if (!value) return {};
        const auto result = Common::Android::GetJString(env, value);
        env->DeleteLocalRef(value);
        return result;
    });
}

} // namespace Common::FS::Android
