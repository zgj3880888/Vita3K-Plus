package org.vita3k.emulator.data

// One `*.psv` / `*.txt` cheat file found in the cheats folder, or one game section of a combined
// database. Built by NativeLib.getCheatFiles() from cheat::list_cheat_games().
data class NativeCheatFile(
    val titleId: String,
    val name: String,
    val path: String,
    val total: Int,
    val enabled: Int,
    val broken: Int
)

// One `_V0`/`_V1` block of a cheat file. `preview` is its first code line, so the list can show
// what the cheat patches without a second native call.
data class NativeCheatEntry(
    val name: String,
    val enabled: Boolean,
    val broken: Boolean,
    val lineCount: Int,
    val preview: String
)
