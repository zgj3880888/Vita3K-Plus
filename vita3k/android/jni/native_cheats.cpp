// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

// Cheat manager for the Android settings screen. The Qt build uses gui-qt/cheats_dialog.cpp
// against a live CheatState; here the settings screen is usually open with no game running, so
// the edits go straight to the `_V0`/`_V1` markers of the `*.psv` files and only touch the live
// state when the game that owns the file is the one currently running.

#include "android_state.h"

#include <cheat/cheat.h>
#include <cheat/functions.h>
#include <cheat/state.h>
#include <kernel/state.h>
#include <util/log.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace {

// `parse_cheat_file` leaves `enabled` false and only reports the marker through `enabled_on_boot`.
// `save_cheat_file` writes `enabled`, so it has to be seeded first or every `_V1` in the file is
// cleared as a side effect of toggling a single line.
cheat::CheatFile parse_for_edit(const fs::path &path, const std::string &title_id) {
    cheat::CheatFile file = cheat::parse_cheat_file(path, title_id);
    for (auto &code : file.cheats)
        code.enabled = code.enabled_on_boot;
    return file;
}

bool is_running_title(const EmuEnvState &emuenv, const std::string &title_id) {
    return !title_id.empty() && (emuenv.cheat.title_id == title_id);
}

// Only safe while that title is the loaded one: `reload` first undoes every ARM write of the
// cheats it is about to replace, which would stomp on another game's patched code.
void reload_if_running(EmuEnvState &emuenv, const std::string &title_id) {
    if (!is_running_title(emuenv, title_id))
        return;

    const cheat::JitInvalidate invalidate_jit = [&emuenv](uint32_t address, size_t size) {
        emuenv.kernel.invalidate_jit_cache(address, size);
    };
    cheat::reload(emuenv.cheat, emuenv.cheat_path, title_id, emuenv.mem, invalidate_jit);
}

jstring new_string(JNIEnv *env, const std::string &value) {
    return env->NewStringUTF(value.c_str());
}

jobjectArray make_empty_array(JNIEnv *env, jclass cls) {
    return env->NewObjectArray(0, cls, nullptr);
}

} // namespace

extern "C" {

JNIEXPORT jobjectArray JNICALL
Java_org_vita3k_emulator_NativeLib_getCheatFiles(JNIEnv *env, jclass) {
    jclass cls = env->FindClass("org/vita3k/emulator/data/NativeCheatFile");
    if (!cls)
        return nullptr;

    const jmethodID ctor = env->GetMethodID(cls, "<init>", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;III)V");
    if (!ctor) {
        return make_empty_array(env, cls);
    }

    auto *emuenv = get_emuenv();
    if (!emuenv) {
        return make_empty_array(env, cls);
    }

    const std::vector<cheat::CheatGame> games = cheat::list_cheat_games(emuenv->cheat_path);
    jobjectArray result = env->NewObjectArray(static_cast<jsize>(games.size()), cls, nullptr);
    if (!result)
        return nullptr;

    for (jsize i = 0; i < static_cast<jsize>(games.size()); ++i) {
        const cheat::CheatGame &game = games[static_cast<size_t>(i)];
        const cheat::CheatFile file = cheat::parse_cheat_file(game.path, game.title_id);

        jint total = static_cast<jint>(file.cheats.size());
        jint enabled = 0;
        jint broken = 0;
        for (const auto &code : file.cheats) {
            if (code.enabled_on_boot)
                ++enabled;
            if (code.broken)
                ++broken;
        }

        // The comment line is the friendliest name, the `_S` line one is the fallback.
        std::string name = game.name;
        if (name.empty())
            name = file.header;

        jstring title_id = new_string(env, game.title_id);
        jstring game_name = new_string(env, name);
        jstring path = new_string(env, game.path.generic_string());
        jobject entry = env->NewObject(cls, ctor, title_id, game_name, path, total, enabled, broken);
        if (entry)
            env->SetObjectArrayElement(result, i, entry);

        env->DeleteLocalRef(title_id);
        env->DeleteLocalRef(game_name);
        env->DeleteLocalRef(path);
        if (entry)
            env->DeleteLocalRef(entry);
    }

    return result;
}

JNIEXPORT jobjectArray JNICALL
Java_org_vita3k_emulator_NativeLib_getCheats(JNIEnv *env, jclass, jstring title_id_str) {
    jclass cls = env->FindClass("org/vita3k/emulator/data/NativeCheatEntry");
    if (!cls)
        return nullptr;

    const jmethodID ctor = env->GetMethodID(cls, "<init>", "(Ljava/lang/String;ZZILjava/lang/String;)V");
    if (!ctor) {
        return make_empty_array(env, cls);
    }

    auto *emuenv = get_emuenv();
    if (!emuenv) {
        return make_empty_array(env, cls);
    }

    const std::string title_id = jstring_to_string(env, title_id_str);
    const fs::path path = cheat::find_cheat_file(emuenv->cheat_path, title_id);
    if (path.empty()) {
        return make_empty_array(env, cls);
    }

    const cheat::CheatFile file = cheat::parse_cheat_file(path, title_id);
    jobjectArray result = env->NewObjectArray(static_cast<jsize>(file.cheats.size()), cls, nullptr);
    if (!result)
        return nullptr;

    for (jsize i = 0; i < static_cast<jsize>(file.cheats.size()); ++i) {
        const cheat::Cheat &code = file.cheats[static_cast<size_t>(i)];

        // The first code line tells the user what the cheat actually patches.
        std::string preview;
        if (!code.lines.empty())
            preview = cheat::format_code_line(code.lines.front());

        jstring name = new_string(env, code.name);
        jstring preview_str = new_string(env, preview);
        jobject entry = env->NewObject(cls, ctor,
            name,
            static_cast<jboolean>(code.enabled_on_boot),
            static_cast<jboolean>(code.broken),
            static_cast<jint>(code.lines.size()),
            preview_str);
        if (entry)
            env->SetObjectArrayElement(result, i, entry);

        env->DeleteLocalRef(name);
        env->DeleteLocalRef(preview_str);
        if (entry)
            env->DeleteLocalRef(entry);
    }

    return result;
}

JNIEXPORT jboolean JNICALL
Java_org_vita3k_emulator_NativeLib_setCheatEnabled(JNIEnv *env, jclass, jstring title_id_str, jint index, jboolean enabled) {
    auto *emuenv = get_emuenv();
    if (!emuenv)
        return JNI_FALSE;

    const std::string title_id = jstring_to_string(env, title_id_str);
    const fs::path path = cheat::find_cheat_file(emuenv->cheat_path, title_id);
    if (path.empty())
        return JNI_FALSE;

    cheat::CheatFile file = parse_for_edit(path, title_id);
    if ((index < 0) || (index >= static_cast<jint>(file.cheats.size())))
        return JNI_FALSE;

    file.cheats[static_cast<size_t>(index)].enabled = enabled != JNI_FALSE;
    if (!cheat::save_cheat_file(file))
        return JNI_FALSE;

    reload_if_running(*emuenv, title_id);

    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_org_vita3k_emulator_NativeLib_setAllCheatsEnabled(JNIEnv *env, jclass, jstring title_id_str, jboolean enabled) {
    auto *emuenv = get_emuenv();
    if (!emuenv)
        return JNI_FALSE;

    const std::string title_id = jstring_to_string(env, title_id_str);
    const fs::path path = cheat::find_cheat_file(emuenv->cheat_path, title_id);
    if (path.empty())
        return JNI_FALSE;

    cheat::CheatFile file = parse_for_edit(path, title_id);
    if (file.cheats.empty())
        return JNI_FALSE;

    for (auto &code : file.cheats) {
        // A broken cheat would only write garbage, so it never gets a `_V1` behind the user's back.
        code.enabled = (enabled != JNI_FALSE) && !code.broken;
    }

    if (!cheat::save_cheat_file(file))
        return JNI_FALSE;

    reload_if_running(*emuenv, title_id);

    return JNI_TRUE;
}

JNIEXPORT jstring JNICALL
Java_org_vita3k_emulator_NativeLib_getCheatDirectory(JNIEnv *env, jclass) {
    auto *emuenv = get_emuenv();
    if (!emuenv)
        return new_string(env, std::string());

    return new_string(env, emuenv->cheat_path.generic_string());
}

} // extern "C"
