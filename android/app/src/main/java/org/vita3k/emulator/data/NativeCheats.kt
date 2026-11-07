package org.vita3k.emulator.data

import java.nio.charset.Charset

// One `*.psv` / `*.txt` cheat file found in the cheats folder, or one game section of a combined
// database. Built by NativeLib.getCheatFiles() from cheat::list_cheat_games().
//
// `name` is the raw bytes of the file's first comment line rather than a String: the collections in
// circulation were saved in whatever encoding their author used, GBK for most of the older Chinese
// ones, so the bytes are only turned into text once the charset can still be guessed here.
data class NativeCheatFile(
    val titleId: String,
    val name: ByteArray?,
    val path: String,
    val total: Int,
    val enabled: Int,
    val broken: Int
)

// One `_V0`/`_V1` block of a cheat file. `preview` is its first code line, so the list can show
// what the cheat patches without a second native call. That line is hex, so it stays a String.
data class NativeCheatEntry(
    val name: ByteArray?,
    val enabled: Boolean,
    val broken: Boolean,
    val lineCount: Int,
    val preview: String
)

// UTF-8 first because that is what current databases use; GBK covers the large older Chinese sets,
// Shift_JIS the Japanese ones. Anything that still cannot be decoded falls back to UTF-8, which at
// least keeps the ASCII half of the name readable.
private val cheatTextCharsets: List<Charset> = listOfNotNull(
    Charsets.UTF_8,
    runCatching { Charset.forName("GBK") }.getOrNull(),
    runCatching { Charset.forName("Shift_JIS") }.getOrNull()
)

/**
 * Text of a cheat name taken from its raw file bytes. A charset is accepted when decoding produces no
 * replacement character, which is how a GBK byte string is told apart from a UTF-8 one.
 */
fun ByteArray?.decodeCheatText(): String {
    val bytes = this ?: return ""
    if (bytes.isEmpty())
        return ""

    for (charset in cheatTextCharsets) {
        val text = runCatching { bytes.toString(charset) }.getOrNull() ?: continue
        if ('\uFFFD' !in text)
            return text.trim()
    }

    return bytes.toString(Charsets.UTF_8).trim()
}

/** The game name of a cheat file, falling back to the title id when the file names no game. */
val NativeCheatFile.displayName: String
    get() = name.decodeCheatText().ifBlank { titleId }

/** The name of one cheat, falling back to a generic label when the declaration carries none. */
val NativeCheatEntry.displayName: String
    get() = name.decodeCheatText().ifBlank { "Cheat" }
