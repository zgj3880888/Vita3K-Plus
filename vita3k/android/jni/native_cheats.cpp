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
#include <config/functions.h>
#include <io/state.h>
#include <kernel/state.h>
#include <mem/functions.h>
#include <mem/ptr.h>
#include <mem/state.h>
#include <util/log.h>

#include <algorithm>
#include <cstddef>
#include <mutex>
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

cheat::JitInvalidate make_invalidate(EmuEnvState &emuenv) {
    // Captured by value on purpose: the reference parameter is gone by the time this runs.
    EmuEnvState *state = &emuenv;

    return [state](uint32_t address, size_t size) {
        state->kernel.invalidate_jit_cache(address, size);
    };
}

// The loaded copy is the truth while its game runs: it is what the display thread replays on every
// vblank, so flipping a flag there is enough to start or stop a cheat without touching the file
// loader. It may only be addressed by index when it came from the same file this screen shows,
// otherwise a file swapped on disk while the game runs would map the indices onto other cheats.
bool live_matches(const EmuEnvState &emuenv, const std::string &title_id, const fs::path &path, const cheat::CheatFile &live) {
    return is_running_title(emuenv, title_id) && !path.empty() && (live.path == path) && !live.cheats.empty();
}

struct CheatCounts {
    jint total = 0;
    jint enabled = 0;
    jint broken = 0;
};

// `enabled` is the live flag when the game runs, `enabled_on_boot` (what `_V1` says) otherwise.
CheatCounts count_cheats(const cheat::CheatFile &file, bool live) {
    CheatCounts counts;
    counts.total = static_cast<jint>(file.cheats.size());
    for (const auto &code : file.cheats) {
        if (live ? code.enabled : code.enabled_on_boot)
            ++counts.enabled;
        if (code.broken)
            ++counts.broken;
    }

    return counts;
}

// The first comment line of a file, which in the databases almost always starts with the title id.
std::string header_name(const std::string &header, const std::string &title_id) {
    if ((header.size() <= title_id.size()) || (header.compare(0, title_id.size(), title_id) != 0))
        return header;

    size_t start = title_id.size();
    while ((start < header.size()) && ((header[start] == ' ') || (header[start] == '\t') || (header[start] == '-') || (header[start] == ':')))
        ++start;

    return header.substr(start);
}

jstring new_string(JNIEnv *env, const std::string &value) {
    return env->NewStringUTF(value.c_str());
}

// Cheat names are whatever the database author saved: UTF-8 today, GBK in the older Chinese
// collections. NewStringUTF would read GBK bytes as UTF-8 and turn every name into mojibake, so the
// bytes are handed over untouched and decoded by the caller, which can still try other charsets.
jbyteArray new_bytes(JNIEnv *env, const std::string &value) {
    jbyteArray array = env->NewByteArray(static_cast<jsize>(value.size()));
    if (!array)
        return nullptr;

    if (!value.empty())
        env->SetByteArrayRegion(array, 0, static_cast<jsize>(value.size()), reinterpret_cast<const jbyte *>(value.data()));

    return array;
}

jobjectArray make_empty_array(JNIEnv *env, jclass cls) {
    return env->NewObjectArray(0, cls, nullptr);
}

// One `NativeCheatFile`, built here so that the folder listing and the single game query stay in
// step: both hand the constructor the same six values in the same order.
jobject make_file_object(JNIEnv *env, jclass cls, jmethodID ctor, const std::string &title_id, const std::string &name, const fs::path &path, const CheatCounts &counts) {
    jstring title = new_string(env, title_id);
    jbyteArray game_name = new_bytes(env, name);
    jstring file_path = new_string(env, path.generic_string());
    jobject entry = env->NewObject(cls, ctor, title, game_name, file_path, counts.total, counts.enabled, counts.broken);

    env->DeleteLocalRef(title);
    env->DeleteLocalRef(game_name);
    env->DeleteLocalRef(file_path);

    return entry;
}

// Both result types are only ever instantiated from here, so R8 strips them unless proguard-rules.pro
// keeps them. That used to fail silently: the list came back empty however many cheat files were in
// the folder. Report it loudly instead, the message ends up in the emulator log.
jclass find_result_class(JNIEnv *env, const char *name) {
    jclass cls = env->FindClass(name);
    if (cls)
        return cls;

    env->ExceptionClear();
    LOG_ERROR("Cheat screen: class {} is missing from the APK (see proguard-rules.pro)", name);
    return nullptr;
}

jmethodID find_constructor(JNIEnv *env, jclass cls, const char *signature) {
    const jmethodID ctor = env->GetMethodID(cls, "<init>", signature);
    if (ctor)
        return ctor;

    env->ExceptionClear();
    LOG_ERROR("Cheat screen: the result class has no constructor {} (kept but stripped?)", signature);
    return nullptr;
}

std::string hex_bytes(uint32_t value, uint32_t size) {
    static const char digits[] = "0123456789ABCDEF";

    std::string text = "0x";
    for (int i = static_cast<int>(size) - 1; i >= 0; --i) {
        const auto byte = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
        text += digits[byte >> 4];
        text += digits[byte & 0xF];
    }

    return text;
}

// The same test the interpreter makes before every write; when it says no the write is dropped
// without a log line, which is why a cheat can look "on" and change nothing at all.
bool is_guest_readable(const MemState &mem, uint32_t address, uint32_t size) {
    return (address >= 0x10000) && is_valid_addr_range(mem, address, address + size);
}

bool read_guest(MemState &mem, uint32_t address, uint32_t size, uint32_t &value) {
    if (!is_guest_readable(mem, address, size))
        return false;

    value = 0;
    for (uint32_t i = 0; i < size; ++i) {
        const uint8_t *byte = Ptr<uint8_t>(address + i).get(mem);
        if (!byte)
            return false;
        value |= static_cast<uint32_t>(*byte) << (i * 8);
    }

    return true;
}

uint32_t width_of(const cheat::CodeLine &line) {
    switch (line.op()) {
    case 0: return 1;
    case 1: return 2;
    default: return 4;
    }
}

uint32_t mask_of(const cheat::CodeLine &line) {
    switch (width_of(line)) {
    case 1: return 0xFFu;
    case 2: return 0xFFFFu;
    default: return 0xFFFFFFFFu;
    }
}

const char *type_name(cheat::CodeType type) {
    switch (type) {
    case cheat::CodeType::write: return "write";
    case cheat::CodeType::pointer_write: return "pointer";
    case cheat::CodeType::compression: return "range";
    case cheat::CodeType::mov: return "copy";
    case cheat::CodeType::pointer_compression: return "pointer range";
    case cheat::CodeType::pointer_mov: return "pointer copy";
    case cheat::CodeType::arm_write: return "arm patch";
    case cheat::CodeType::relative_base: return "module base";
    case cheat::CodeType::button_pad: return "button";
    case cheat::CodeType::condition: return "condition";
    }
    return "unknown";
}

} // namespace

extern "C" {

JNIEXPORT jobjectArray JNICALL
Java_org_vita3k_emulator_NativeLib_getCheatFiles(JNIEnv *env, jclass) {
    jclass cls = find_result_class(env, "org/vita3k/emulator/data/NativeCheatFile");
    if (!cls)
        return nullptr;

    const jmethodID ctor = find_constructor(env, cls, "(Ljava/lang/String;[BLjava/lang/String;III)V");
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

        // The comment line is the friendliest name, the `_S` line one is the fallback.
        const std::string name = game.name.empty() ? file.header : game.name;

        jobject entry = make_file_object(env, cls, ctor, game.title_id, name, game.path, count_cheats(file, false));
        if (entry) {
            env->SetObjectArrayElement(result, i, entry);
            env->DeleteLocalRef(entry);
        }
    }

    return result;
}

// One game instead of the whole folder. The in-game panel asks for this: a phone whose cheats
// folder holds a 700 file database would otherwise parse every file on each visit, while a game is
// running. Returns null when the folder holds nothing for that title.
JNIEXPORT jobject JNICALL
Java_org_vita3k_emulator_NativeLib_getCheatFile(JNIEnv *env, jclass, jstring title_id_str) {
    jclass cls = find_result_class(env, "org/vita3k/emulator/data/NativeCheatFile");
    if (!cls)
        return nullptr;

    const jmethodID ctor = find_constructor(env, cls, "(Ljava/lang/String;[BLjava/lang/String;III)V");
    if (!ctor)
        return nullptr;

    auto *emuenv = get_emuenv();
    if (!emuenv)
        return nullptr;

    const std::string title_id = jstring_to_string(env, title_id_str);
    const fs::path path = cheat::find_cheat_file(emuenv->cheat_path, title_id);
    if (path.empty())
        return nullptr;

    const cheat::CheatFile live = cheat::snapshot(emuenv->cheat);
    const bool is_live = live_matches(*emuenv, title_id, path, live);
    const cheat::CheatFile file = is_live ? live : cheat::parse_cheat_file(path, title_id);

    return make_file_object(env, cls, ctor, title_id, header_name(file.header, title_id), path, count_cheats(file, is_live));
}

JNIEXPORT jobjectArray JNICALL
Java_org_vita3k_emulator_NativeLib_getCheats(JNIEnv *env, jclass, jstring title_id_str) {
    jclass cls = find_result_class(env, "org/vita3k/emulator/data/NativeCheatEntry");
    if (!cls)
        return nullptr;

    const jmethodID ctor = find_constructor(env, cls, "([BZZILjava/lang/String;)V");
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

    // While the game runs only the loaded copy knows which switches are on: this screen rewrites
    // that copy, the file on disk still carries what was there at boot until it is saved.
    const cheat::CheatFile live = cheat::snapshot(emuenv->cheat);
    const bool is_live = live_matches(*emuenv, title_id, path, live);
    const cheat::CheatFile file = is_live ? live : cheat::parse_cheat_file(path, title_id);

    jobjectArray result = env->NewObjectArray(static_cast<jsize>(file.cheats.size()), cls, nullptr);
    if (!result)
        return nullptr;

    for (jsize i = 0; i < static_cast<jsize>(file.cheats.size()); ++i) {
        const cheat::Cheat &code = file.cheats[static_cast<size_t>(i)];

        // The first code line tells the user what the cheat actually patches.
        std::string preview;
        if (!code.lines.empty())
            preview = cheat::format_code_line(code.lines.front());

        jbyteArray name = new_bytes(env, code.name);
        jstring preview_str = new_string(env, preview);
        jobject entry = env->NewObject(cls, ctor,
            name,
            static_cast<jboolean>(is_live ? code.enabled : code.enabled_on_boot),
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

    const cheat::CheatFile live = cheat::snapshot(emuenv->cheat);
    if (live_matches(*emuenv, title_id, path, live)) {
        if ((index < 0) || (index >= static_cast<jint>(live.cheats.size())))
            return JNI_FALSE;

        // Flipping the flag in the loaded file is what takes effect: the display thread replays it
        // on the next vblank and undoes the ARM writes of a cheat that was switched off. `save`
        // only rewrites the `_V0`/`_V1` digit, so the choice survives to the next boot.
        cheat::set_cheat_enabled(emuenv->cheat, static_cast<size_t>(index), enabled != JNI_FALSE, emuenv->mem, make_invalidate(*emuenv));
        if (!cheat::save(emuenv->cheat))
            LOG_WARN("Cheat screen: switched live, but could not write {}", path.generic_string());

        return JNI_TRUE;
    }

    cheat::CheatFile file = parse_for_edit(path, title_id);
    if ((index < 0) || (index >= static_cast<jint>(file.cheats.size())))
        return JNI_FALSE;

    file.cheats[static_cast<size_t>(index)].enabled = enabled != JNI_FALSE;
    return cheat::save_cheat_file(file) ? JNI_TRUE : JNI_FALSE;
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

    const cheat::CheatFile live = cheat::snapshot(emuenv->cheat);
    if (live_matches(*emuenv, title_id, path, live)) {
        const cheat::JitInvalidate invalidate = make_invalidate(*emuenv);
        for (size_t i = 0; i < live.cheats.size(); ++i) {
            // A broken cheat would only write garbage, so it never gets a `_V1` behind the user's back.
            const bool on = (enabled != JNI_FALSE) && !live.cheats[i].broken;
            cheat::set_cheat_enabled(emuenv->cheat, i, on, emuenv->mem, invalidate);
        }

        if (!cheat::save(emuenv->cheat))
            LOG_WARN("Cheat screen: switched live, but could not write {}", path.generic_string());

        return JNI_TRUE;
    }

    cheat::CheatFile file = parse_for_edit(path, title_id);
    if (file.cheats.empty())
        return JNI_FALSE;

    for (auto &code : file.cheats) {
        // A broken cheat would only write garbage, so it never gets a `_V1` behind the user's back.
        code.enabled = (enabled != JNI_FALSE) && !code.broken;
    }

    return cheat::save_cheat_file(file) ? JNI_TRUE : JNI_FALSE;
}

// The master switch, the same one the desktop build has: with it off `apply` writes nothing at
// all, whatever the per cheat switches say. Kept in the config so the next boot agrees with it.
JNIEXPORT jboolean JNICALL
Java_org_vita3k_emulator_NativeLib_getCheatsEnabled(JNIEnv *, jclass) {
    auto *emuenv = get_emuenv();

    return (emuenv && emuenv->cfg.enable_cheats) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_org_vita3k_emulator_NativeLib_setCheatsEnabled(JNIEnv *, jclass, jboolean enabled) {
    auto *emuenv = get_emuenv();
    if (!emuenv)
        return JNI_FALSE;

    emuenv->cfg.enable_cheats = enabled != JNI_FALSE;
    cheat::set_enabled(emuenv->cheat, emuenv->cfg.enable_cheats, emuenv->mem, make_invalidate(*emuenv));
    config::serialize_config(emuenv->cfg, emuenv->cfg.config_path);

    return JNI_TRUE;
}

JNIEXPORT jstring JNICALL
Java_org_vita3k_emulator_NativeLib_getCheatDirectory(JNIEnv *env, jclass) {
    auto *emuenv = get_emuenv();
    if (!emuenv)
        return new_string(env, std::string());

    return new_string(env, emuenv->cheat_path.generic_string());
}

// One line the cheat screen shows under the folder path, so that "I put a file there and the list
// stays empty" can be told apart from "the file is there but its name carries no title id".
JNIEXPORT jstring JNICALL
Java_org_vita3k_emulator_NativeLib_getCheatScan(JNIEnv *env, jclass) {
    auto *emuenv = get_emuenv();
    if (!emuenv)
        return new_string(env, std::string());

    const cheat::CheatScan scan = cheat::scan_cheat_folder(emuenv->cheat_path);

    std::string summary = std::to_string(scan.candidates) + " file(s) found, "
        + std::to_string(scan.recognised) + " named after a title id";
    if (!scan.unrecognised.empty()) {
        summary += ". Ignored: ";
        for (size_t i = 0; i < scan.unrecognised.size(); ++i) {
            if (i != 0)
                summary += ", ";
            summary += scan.unrecognised[i];
        }
        summary += " (the name has to be the nine character title id, like PCSF00403.psv)";
    }

    return new_string(env, summary);
}

// Everything the engine knows about the running game, in one block the user can copy back to us.
// A cheat that is on but does nothing fails silently inside write_bytes(), so the only way to tell
// "the engine never ran it" from "it ran and the address was not mapped" is to look at the numbers.
JNIEXPORT jstring JNICALL
Java_org_vita3k_emulator_NativeLib_getCheatStatus(JNIEnv *env, jclass) {
    auto *emuenv = get_emuenv();
    if (!emuenv)
        return new_string(env, "No emulator session.");

    bool master_enabled = false;
    uint32_t launch_delay = 0;
    {
        const std::lock_guard<std::mutex> lock(emuenv->cheat.mutex);
        master_enabled = emuenv->cheat.enabled;
        launch_delay = emuenv->cheat.launch_delay_frames;
    }

    const uint32_t frames = emuenv->cheat.frames_presented.load(std::memory_order_relaxed);
    const std::string running = emuenv->io.title_id;
    const std::string loaded = cheat::loaded_title_id(emuenv->cheat);
    const cheat::CheatFile file = cheat::snapshot(emuenv->cheat);

    std::string report;
    report += "enable-cheats: " + std::string(emuenv->cfg.enable_cheats ? "on" : "off") + "\n";
    report += "master switch: " + std::string(master_enabled ? "on" : "off") + "\n";
    report += "running game: " + (running.empty() ? std::string("<none>") : running) + "\n";
    report += "cheat state: " + (loaded.empty() ? std::string("<empty>") : loaded) + "\n";
    report += "frames: " + std::to_string(frames) + " of " + std::to_string(launch_delay)
        + (frames < launch_delay ? " (still holding back)\n" : " (writing)\n");

    if (!running.empty() && (loaded != running))
        report += "MISMATCH: cheats were loaded for '" + loaded + "', not for the running game\n";

    if (file.cheats.empty()) {
        report += "no cheat file loaded for this game\n";
        return new_string(env, report);
    }

    size_t on = 0;
    size_t broken = 0;
    for (const auto &code : file.cheats) {
        if (code.enabled)
            ++on;
        if (code.broken)
            ++broken;
    }

    report += "file: " + file.path.generic_string() + "\n";
    report += std::to_string(file.cheats.size()) + " cheats, " + std::to_string(on) + " on, " + std::to_string(broken) + " broken\n";

    size_t shown = 0;
    for (size_t i = 0; (i < file.cheats.size()) && (shown < 12); ++i) {
        const cheat::Cheat &code = file.cheats[i];
        if (code.broken || !code.enabled)
            continue;
        ++shown;

        report += "[" + std::to_string(i) + "] " + code.name + "\n";
        if (code.lines.empty()) {
            report += "    no code line\n";
            continue;
        }

        const cheat::CodeLine &line = code.lines.front();
        report += "    " + cheat::format_code_line(line) + "  (" + type_name(line.type()) + ")\n";

        // Only a plain write can be checked from here, the pointer codes need the game to fill them.
        if (line.type() != cheat::CodeType::write)
            continue;

        uint32_t current = 0;
        const uint32_t wanted = line.second & mask_of(line);
        if (!read_guest(emuenv->mem, line.first, width_of(line), current)) {
            report += "    at " + hex_bytes(line.first, 4) + " NOT MAPPED, the write is dropped\n";
            continue;
        }

        report += "    at " + hex_bytes(line.first, 4) + " now " + hex_bytes(current, width_of(line));
        if (current == wanted)
            report += ", already the wanted value\n";
        else
            report += ", wanted " + hex_bytes(wanted, width_of(line)) + "\n";
    }

    return new_string(env, report);
}

} // extern "C"
