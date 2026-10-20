// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

// Eden Duo: SAF document operations for shared-folder import, on top of fs_android.h. Header-only
// (inline functions and variables), so the upstream build lists stay untouched; fs_android.cpp
// registers the callbacks with RegisterCallbacks.

#pragma once

#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <jni.h>

#include "common/android/android_common.h"
#include "common/common_types.h"

namespace Common::Android {
JNIEnv* GetEnvForThread();
/// Whether the caller runs on its thread's own stack rather than a fiber's (id_cache.cpp).
bool IsOnOwnThreadStack();
} // namespace Common::Android

namespace Common::FS::Android {

/// Runs [work] with a JNIEnv. ART tracks JNI frames against the thread's own stack, so calls made
/// from a fiber (emulated services run on fibers) corrupt the thread's local references and later
/// abort in unrelated Java code; from a fiber the call hops to a short-lived real thread instead.
template <typename Work>
auto WithJni(Work&& work) {
    if (Common::Android::IsOnOwnThreadStack()) {
        return work(Common::Android::GetEnvForThread());
    }
    return std::async(std::launch::async,
                      [&] { return work(Common::Android::GetEnvForThread()); })
        .get();
}

namespace DocumentCallbacks {
inline jclass native_library = nullptr;
inline jmethodID operate_document = nullptr;
inline jmethodID list_document_entries = nullptr;
} // namespace DocumentCallbacks

/// Looks up NativeLibrary.operateDocument and NativeLibrary.listDocumentEntries.
inline void RegisterDocumentCallbacks(JNIEnv* env, jclass clazz) {
    DocumentCallbacks::native_library = clazz;
    DocumentCallbacks::operate_document = env->GetStaticMethodID(
        clazz, "operateDocument", "(Ljava/lang/String;Ljava/lang/String;I)Z");
    DocumentCallbacks::list_document_entries = env->GetStaticMethodID(
        clazz, "listDocumentEntries", "(Ljava/lang/String;)[Ljava/lang/String;");
}

inline void UnRegisterDocumentCallbacks() {
    DocumentCallbacks::operate_document = nullptr;
    DocumentCallbacks::list_document_entries = nullptr;
}

/// Runs DocumentFileAccess operation [operation] on [path] (and [other], when it takes two).
inline bool OperateDocument(const std::string& path, const std::string& other, int operation) {
    if (!DocumentCallbacks::operate_document) return false;
    return WithJni([&](JNIEnv* env) {
        const auto source = Common::Android::ToJString(env, path);
        const auto target = Common::Android::ToJString(env, other);
        const bool result = env->CallStaticBooleanMethod(DocumentCallbacks::native_library,
                                                         DocumentCallbacks::operate_document,
                                                         source, target, operation);
        env->DeleteLocalRef(source);
        env->DeleteLocalRef(target);
        return result;
    });
}

struct DocumentEntry {
    std::string name;
    bool directory{};
    std::optional<u64> size; ///< a file's size from the folder listing, when the provider has it
};

/// The entries of the document folder [path], with file sizes from the same listing query.
inline std::vector<DocumentEntry> ListDocumentEntries(const std::string& path) {
    if (!DocumentCallbacks::list_document_entries) return {};
    return WithJni([&](JNIEnv* env) {
        std::vector<DocumentEntry> entries;
        const auto source = Common::Android::ToJString(env, path);
        const auto array = static_cast<jobjectArray>(env->CallStaticObjectMethod(
            DocumentCallbacks::native_library, DocumentCallbacks::list_document_entries, source));
        env->DeleteLocalRef(source);
        if (!array) return entries;
        for (jsize i = 0; i < env->GetArrayLength(array); ++i) {
            const auto item = static_cast<jstring>(env->GetObjectArrayElement(array, i));
            const auto text = Common::Android::GetJString(env, item);
            if (text.starts_with("d:") && text.size() > 2) {
                entries.push_back({text.substr(2), true, std::nullopt});
            } else if (text.starts_with("f:")) {
                const auto slash = text.find('/', 2);
                if (slash == std::string::npos) {
                    if (text.size() > 2) entries.push_back({text.substr(2), false, std::nullopt});
                } else if (slash + 1 < text.size()) {
                    std::optional<u64> size;
                    if (slash > 2) {
                        size = std::stoull(text.substr(2, slash - 2));
                    }
                    entries.push_back({text.substr(slash + 1), false, size});
                }
            }
            env->DeleteLocalRef(item);
        }
        env->DeleteLocalRef(array);
        return entries;
    });
}

} // namespace Common::FS::Android
