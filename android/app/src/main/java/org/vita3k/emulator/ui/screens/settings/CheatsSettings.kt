package org.vita3k.emulator.ui.screens.settings

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.vita3k.emulator.NativeLib
import org.vita3k.emulator.R
import org.vita3k.emulator.data.NativeCheatEntry
import org.vita3k.emulator.data.NativeCheatFile
import org.vita3k.emulator.data.VitaDocumentsProvider
import java.io.File

/** The nine characters that identify a game, anywhere in a file name. */
private val titleIdInName = Regex("([A-Za-z]{4}[0-9]{5})")

/**
 * Name to store an imported file under. A collection keeps its own name because `.txt` and `.db`
 * may hold every game behind `_S` lines, so there is no single title id to rename it after;
 * anything else is renamed after the title id found in its name, which is what the loader matches on.
 */
private fun importedCheatName(source: String): String? {
    val extension = source.substringAfterLast('.', "").lowercase()
    if (extension == "txt" || extension == "db")
        return source

    val titleId = titleIdInName.find(source)?.groupValues?.get(1)?.uppercase() ?: return null
    return "$titleId.psv"
}

/**
 * The same walk the native side does, kept on the Kotlin side so the screen can tell "the folder is
 * empty" apart from "the folder has files but the emulator failed to report them". Everything here is
 * plain java.io on the app's own external files directory, so it cannot fail for permission reasons.
 */
private fun listCheatFilesOnDisk(directory: String, depth: Int = 0): List<String> {
    if (depth > 4 || directory.isBlank())
        return emptyList()

    val entries = File(directory).listFiles() ?: return emptyList()
    val found = mutableListOf<String>()
    for (entry in entries) {
        if (entry.isDirectory) {
            if (!entry.name.startsWith("."))
                found += listCheatFilesOnDisk(entry.path, depth + 1)
            continue
        }
        if (isCheatFileName(entry.name))
            found += entry.name
    }
    return found
}

private fun isCheatFileName(name: String): Boolean {
    val extension = name.substringAfterLast('.', "").lowercase()
    return extension == "psv" || extension == "txt" || extension == "db"
}

private fun displayNameOf(context: Context, uri: Uri): String? {
    val projection = arrayOf(OpenableColumns.DISPLAY_NAME)
    context.contentResolver.query(uri, projection, null, null, null)?.use { cursor ->
        if (cursor.moveToFirst()) {
            val index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
            if (index >= 0) {
                val name = cursor.getString(index)
                if (!name.isNullOrBlank())
                    return name
            }
        }
    }
    return uri.lastPathSegment?.substringAfterLast('/')?.takeIf { it.isNotBlank() }
}

// The picker returns a content:// uri anywhere on the device, which is how a file gets into the
// cheats folder on Android 11 and later: Android/data is not reachable from a file manager.
private fun importCheatFile(context: Context, uri: Uri, directory: String): String? {
    if (directory.isBlank())
        return null

    val source = displayNameOf(context, uri) ?: return null
    val target = importedCheatName(source) ?: return null

    val folder = File(directory)
    if (!folder.isDirectory && !folder.mkdirs())
        return null

    return runCatching {
        val input = context.contentResolver.openInputStream(uri) ?: return@runCatching null
        input.use { sourceStream ->
            File(folder, target).outputStream().use { sink -> sourceStream.copyTo(sink) }
        }
        target
    }.getOrNull()
}

/**
 * Cheat manager shown in the Android settings screen.
 *
 * The Qt build toggles cheats through a live `CheatState`; here the screen is normally open with no
 * game running, so every switch rewrites the `_V0`/`_V1` marker of the `*.psv` file on disk and the
 * native side re-applies it immediately when that game happens to be the running one.
 */
@Composable
internal fun CheatsSettingsSection(
    titleId: String?,
    onShowHelp: (SettingsHelpEntry) -> Unit
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    var directory by remember { mutableStateOf("") }
    var files by remember { mutableStateOf<List<NativeCheatFile>>(emptyList()) }
    var loading by remember { mutableStateOf(true) }
    var busy by remember { mutableStateOf(false) }
    var expanded by rememberSaveable { mutableStateOf<String?>(null) }
    var entries by remember { mutableStateOf<List<NativeCheatEntry>>(emptyList()) }
    var entriesLoading by remember { mutableStateOf(false) }
    var failure by remember { mutableStateOf<String?>(null) }
    var scan by remember { mutableStateOf("") }
    // Only filled when the emulator reports nothing, as the proof that the files are really there.
    var diskFiles by remember { mutableStateOf<List<String>>(emptyList()) }

    // Reading the folder parses every cheat file, so it never runs on the composition thread.
    suspend fun refreshFiles() {
        loading = true
        val loaded = withContext(Dispatchers.IO) {
            runCatching { NativeLib.getCheatFiles().toList() }.getOrDefault(emptyList())
        }
        val path = withContext(Dispatchers.IO) {
            runCatching { NativeLib.getCheatDirectory() }.getOrDefault("")
        }
        val summary = withContext(Dispatchers.IO) {
            runCatching { NativeLib.getCheatScan() }.getOrDefault("")
        }
        val onDisk = withContext(Dispatchers.IO) {
            if (loaded.isEmpty()) {
                runCatching { listCheatFilesOnDisk(path) }.getOrDefault(emptyList())
            } else {
                emptyList()
            }
        }
        files = loaded
        directory = path
        scan = summary
        diskFiles = onDisk
        loading = false
        if (expanded != null && loaded.none { it.titleId == expanded }) {
            expanded = null
            entries = emptyList()
        }
    }

    suspend fun refreshEntries(id: String) {
        entriesLoading = true
        entries = withContext(Dispatchers.IO) {
            runCatching { NativeLib.getCheats(id).toList() }.getOrDefault(emptyList())
        }
        entriesLoading = false
    }

    LaunchedEffect(Unit) { refreshFiles() }
    LaunchedEffect(expanded) {
        val id = expanded
        if (id == null) entries = emptyList() else refreshEntries(id)
    }

    fun applyChange(id: String, enabled: Boolean, index: Int?, applyAll: Boolean): Unit {
        if (busy)
            return
        busy = true
        failure = null
        scope.launch {
            val ok = withContext(Dispatchers.IO) {
                if (applyAll) {
                    NativeLib.setAllCheatsEnabled(id, enabled)
                } else {
                    NativeLib.setCheatEnabled(id, index ?: 0, enabled)
                }
            }
            if (ok) {
                refreshEntries(id)
                refreshFiles()
            } else {
                failure = context.getString(R.string.settings_cheats_write_failed)
            }
            busy = false
        }
    }

    val enabledTotal = files.sumOf { it.enabled }
    val brokenTotal = files.sumOf { it.broken }
    val title = stringResource(R.string.settings_cheats_title)

    // Android/data cannot be pasted into from a file manager since Android 11, so the picker is the
    // only way in on most devices: the file is copied straight into the cheats folder.
    val importFailed = stringResource(R.string.settings_cheats_import_failed)
    val importPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri == null)
            return@rememberLauncherForActivityResult

        busy = true
        failure = null
        scope.launch {
            val imported = withContext(Dispatchers.IO) { importCheatFile(context, uri, directory) }
            busy = false
            if (imported == null) {
                Toast.makeText(context, importFailed, Toast.LENGTH_LONG).show()
            } else {
                Toast.makeText(context, context.getString(R.string.settings_cheats_imported, imported), Toast.LENGTH_LONG).show()
                refreshFiles()
            }
        }
    }

    SettingsSectionCard(
        title = title,
        summary = if (files.isEmpty()) {
            stringResource(R.string.settings_cheats_summary_none)
        } else {
            stringResource(R.string.settings_cheats_summary, files.size, enabledTotal)
        },
        help = SettingsHelpEntry(
            title = title,
            body = stringResource(R.string.settings_cheats_help_body),
            scope = SettingsScope.Global
        ),
        onShowHelp = onShowHelp
    ) {
        SettingsNote(
            text = if (directory.isBlank()) {
                stringResource(R.string.settings_cheats_folder_unknown)
            } else {
                stringResource(R.string.settings_cheats_folder, directory)
            }
        )

        if (scan.isNotBlank())
            SettingsNote(text = stringResource(R.string.settings_cheats_scan, scan))

        val browseTitle = stringResource(R.string.settings_cheats_browse)
        val browseUnavailable = stringResource(R.string.settings_cheats_browse_unavailable)
        val refreshLabel = stringResource(R.string.settings_cheats_refresh)
        SettingsActionRow(
            title = browseTitle,
            value = stringResource(R.string.settings_cheats_browse_value),
            onClick = {
                val authority = VitaDocumentsProvider.authority(context)
                val candidates = listOf(
                    Intent(Intent.ACTION_VIEW)
                        .setDataAndType(
                            DocumentsContract.buildRootUri(authority, VitaDocumentsProvider.ROOT_ID),
                            DocumentsContract.Root.MIME_TYPE_ITEM
                        )
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                    Intent(Intent.ACTION_OPEN_DOCUMENT_TREE)
                        .putExtra(
                            DocumentsContract.EXTRA_INITIAL_URI,
                            DocumentsContract.buildDocumentUri(authority, VitaDocumentsProvider.DOC_ROOT)
                        )
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                )
                val launched = candidates.any { intent ->
                    runCatching { context.startActivity(intent) }.isSuccess
                }
                if (!launched)
                    Toast.makeText(context, browseUnavailable, Toast.LENGTH_LONG).show()
            },
            actionLabel = refreshLabel,
            onActionClick = { scope.launch { refreshFiles() } },
            actionEnabled = !loading && !busy,
            onShowHelp = onShowHelp
        )

        SettingsActionRow(
            title = stringResource(R.string.settings_cheats_import),
            value = stringResource(R.string.settings_cheats_import_value),
            onClick = { importPicker.launch(arrayOf("*/*")) },
            enabled = !busy && directory.isNotBlank(),
            onShowHelp = onShowHelp
        )

        if (loading && files.isEmpty()) {
            SettingsLoadingState()
        } else if (files.isEmpty()) {
            if (diskFiles.isNotEmpty()) {
                SettingsNote(
                    text = stringResource(
                        R.string.settings_cheats_disk_mismatch,
                        diskFiles.size,
                        diskFiles.take(4).joinToString(", ")
                    ),
                    color = MaterialTheme.colorScheme.error
                )
            }
            SettingsNote(text = stringResource(R.string.settings_cheats_empty))
            SettingsNote(text = stringResource(R.string.settings_cheats_empty_hint))
        } else {
            files.forEach { file ->
                val isOpen = expanded == file.titleId
                val isRunning = titleId != null && titleId == file.titleId
                val counts = stringResource(R.string.settings_cheats_counts, file.enabled, file.total)
                val brokenCount = if (file.broken > 0) {
                    " · " + stringResource(R.string.settings_cheats_broken_count, file.broken)
                } else {
                    ""
                }
                val runningPrefix = if (isRunning) {
                    stringResource(R.string.settings_cheats_running) + " · "
                } else {
                    ""
                }
                SettingsActionRow(
                    title = file.name.ifBlank { file.titleId },
                    value = "$runningPrefix${file.titleId} · $counts$brokenCount",
                    onClick = { expanded = if (isOpen) null else file.titleId },
                    onShowHelp = onShowHelp
                )

                if (isOpen) {
                    Column(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(start = 12.dp),
                        verticalArrangement = Arrangement.spacedBy(4.dp)
                    ) {
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.spacedBy(4.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            TextButton(
                                onClick = { applyChange(file.titleId, true, null, applyAll = true) },
                                enabled = !busy && !entriesLoading && entries.any { !it.broken && !it.enabled }
                            ) {
                                Text(text = stringResource(R.string.settings_cheats_enable_all))
                            }
                            TextButton(
                                onClick = { applyChange(file.titleId, false, null, applyAll = true) },
                                enabled = !busy && !entriesLoading && entries.any { it.enabled }
                            ) {
                                Text(text = stringResource(R.string.settings_cheats_disable_all))
                            }
                        }

                        if (entriesLoading) {
                            SettingsLoadingState()
                        } else if (entries.isEmpty()) {
                            SettingsNote(text = stringResource(R.string.settings_cheats_no_entries))
                        } else {
                            entries.forEachIndexed { index, entry ->
                                SettingsToggleRow(
                                    title = entry.name,
                                    checked = entry.enabled && !entry.broken,
                                    summary = when {
                                        entry.broken -> stringResource(R.string.settings_cheats_broken)
                                        entry.preview.isBlank() -> stringResource(R.string.settings_cheats_no_code)
                                        else -> entry.preview
                                    },
                                    enabled = !busy && !entry.broken,
                                    onCheckedChange = { checked ->
                                        applyChange(file.titleId, checked, index, applyAll = false)
                                    },
                                    help = SettingsHelpEntry(
                                        title = entry.name,
                                        body = if (entry.broken) {
                                            stringResource(R.string.settings_cheats_broken_desc)
                                        } else {
                                            stringResource(R.string.settings_cheats_entry_desc, entry.lineCount)
                                        },
                                        scope = SettingsScope.Global
                                    ),
                                    onShowHelp = onShowHelp
                                )
                            }
                        }

                        SettingsNote(text = stringResource(R.string.settings_cheats_takes_effect))
                    }
                }
            }

            if (brokenTotal > 0) {
                SettingsNote(
                    text = stringResource(R.string.settings_cheats_broken_total, brokenTotal),
                    color = MaterialTheme.colorScheme.error
                )
            }
        }

        failure?.let { message ->
            SettingsNote(text = message, color = MaterialTheme.colorScheme.error)
        }
    }
}
