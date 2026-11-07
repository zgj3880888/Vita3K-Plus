package org.vita3k.emulator.ui.screens.settings

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import android.widget.Toast
import androidx.activity.compose.LocalActivityResultRegistryOwner
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
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.vita3k.emulator.NativeLib
import org.vita3k.emulator.R
import org.vita3k.emulator.data.NativeCheatEntry
import org.vita3k.emulator.data.NativeCheatFile
import org.vita3k.emulator.data.VitaDocumentsProvider
import org.vita3k.emulator.data.displayName
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
 * Cheat manager. Shown in two very different places, which is why it asks the native side for
 * different things in each:
 *
 * - from the settings screen, no game runs: every cheat file of the folder is listed, and a switch
 *   only rewrites the `_V0`/`_V1` marker for the next boot;
 * - from the pause menu, the game runs: only that game is listed, and a switch flips the flag of the
 *   copy the display thread replays every vblank, so it takes effect at the next frame while the
 *   file is still updated so the choice survives a restart.
 *
 * The master switch is the engine wide gate the desktop build has. With it off, no code is written
 * to memory at all, which is the fastest way to compare "the game is broken" against "the cheat did
 * it" without leaving the game.
 */
@Composable
internal fun CheatsSettingsSection(
    titleId: String?,
    onShowHelp: (SettingsHelpEntry) -> Unit
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    // Non-null means this panel knows a game. Only a live session makes it the panel of that game:
    // the settings screen also carries a title id when it edits one game's config, and then nothing
    // runs to apply a switch to.
    val runningId = titleId?.takeIf { it.isNotBlank() }
    val inGame = remember(runningId) {
        runningId != null && runCatching { NativeLib.isAppRunning() }.getOrDefault(false)
    }

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
    var diagnosis by remember { mutableStateOf("") }
    var diagnosing by remember { mutableStateOf(false) }
    var masterEnabled by remember(inGame) { mutableStateOf(true) }
    var masterKnown by remember(inGame) { mutableStateOf(false) }

    // Reading the folder parses every cheat file, so it never runs on the composition thread. In the
    // pause menu only the running game is asked for: a phone with a 700 file database would parse
    // all of it on every visit otherwise, with the game still running.
    suspend fun refreshFiles() {
        loading = true
        val loaded = withContext(Dispatchers.IO) {
            runCatching {
                val id = if (inGame) runningId else null
                if (id != null) {
                    listOfNotNull(NativeLib.getCheatFile(id))
                } else {
                    NativeLib.getCheatFiles().toList()
                }
            }.getOrDefault(emptyList())
        }
        val path = withContext(Dispatchers.IO) {
            runCatching { NativeLib.getCheatDirectory() }.getOrDefault("")
        }
        val summary = withContext(Dispatchers.IO) {
            // Only the folder view shows it, and it walks every file name of the folder.
            if (inGame) "" else runCatching { NativeLib.getCheatScan() }.getOrDefault("")
        }
        val onDisk = withContext(Dispatchers.IO) {
            if (loaded.isEmpty() && !inGame) {
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

    suspend fun refreshMaster() {
        masterEnabled = withContext(Dispatchers.IO) {
            runCatching { NativeLib.getCheatsEnabled() }.getOrDefault(true)
        }
        masterKnown = true
    }

    // Reads the live engine state. Every failure in there is silent, so it has to be visible.
    suspend fun diagnose() {
        if (diagnosing)
            return
        diagnosing = true
        val report = withContext(Dispatchers.IO) {
            runCatching { NativeLib.getCheatStatus() }.getOrDefault("")
        }
        diagnosis = report
        diagnosing = false
    }

    LaunchedEffect(Unit) {
        refreshFiles()
        refreshMaster()
    }

    // The pause menu is opened to change something now, so the running game starts unfolded.
    LaunchedEffect(files, runningId) {
        val id = if (inGame) runningId else null
        if (id != null && expanded == null && files.any { it.titleId == id })
            expanded = id
    }

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

    fun applyMaster(enabled: Boolean) {
        if (busy)
            return
        busy = true
        failure = null
        scope.launch {
            val ok = withContext(Dispatchers.IO) {
                runCatching { NativeLib.setCheatsEnabled(enabled) }.getOrDefault(false)
            }
            busy = false
            if (ok) {
                masterEnabled = enabled
                diagnosis = ""
            } else {
                failure = context.getString(R.string.settings_cheats_master_failed)
            }
        }
    }

    val enabledTotal = files.sumOf { it.enabled }
    val brokenTotal = files.sumOf { it.broken }
    val title = stringResource(R.string.settings_cheats_title)

    // Android/data cannot be pasted into from a file manager since Android 11, so the picker is the
    // only way in on most devices: the file is copied straight into the cheats folder. The pause menu
    // hides it, a game is running there and pulling another activity in front of it is not wanted.
    // The owner check keeps this screen from failing its composition in a host that provides no
    // result registry at all, which is what the emulator overlay used to be.
    val importFailed = stringResource(R.string.settings_cheats_import_failed)
    val canImport = !inGame && LocalActivityResultRegistryOwner.current != null
    val importPicker = if (canImport) {
        rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
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
    } else {
        null
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
        SettingsToggleRow(
            title = stringResource(R.string.settings_cheats_master),
            checked = masterEnabled,
            enabled = masterKnown && !busy,
            summary = if (masterEnabled) {
                stringResource(R.string.settings_cheats_master_on)
            } else {
                stringResource(R.string.settings_cheats_master_off)
            },
            onCheckedChange = { applyMaster(it) },
            help = SettingsHelpEntry(
                title = stringResource(R.string.settings_cheats_master),
                body = stringResource(R.string.settings_cheats_master_desc),
                scope = SettingsScope.Global
            ),
            onShowHelp = onShowHelp
        )

        if (!masterEnabled && files.any { it.enabled > 0 })
            SettingsNote(
                text = stringResource(R.string.settings_cheats_master_off_note),
                color = MaterialTheme.colorScheme.error
            )

        if (!inGame) {
            SettingsNote(
                text = if (directory.isBlank()) {
                    stringResource(R.string.settings_cheats_folder_unknown)
                } else {
                    stringResource(R.string.settings_cheats_folder, directory)
                }
            )

            if (scan.isNotBlank())
                SettingsNote(text = stringResource(R.string.settings_cheats_scan, scan))
        }

        val browseTitle = stringResource(R.string.settings_cheats_browse)
        val browseUnavailable = stringResource(R.string.settings_cheats_browse_unavailable)
        val refreshLabel = stringResource(R.string.settings_cheats_refresh)
        if (!inGame) {
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
                onClick = { importPicker?.launch(arrayOf("*/*")) },
                enabled = !busy && directory.isNotBlank() && importPicker != null,
                onShowHelp = onShowHelp
            )
        }

        SettingsActionRow(
            title = stringResource(R.string.settings_cheats_diagnose),
            value = stringResource(R.string.settings_cheats_diagnose_value),
            onClick = { scope.launch { diagnose() } },
            actionLabel = stringResource(R.string.settings_cheats_diagnose_run),
            onActionClick = { scope.launch { diagnose() } },
            actionEnabled = !diagnosing,
            onShowHelp = onShowHelp
        )

        if (diagnosing)
            SettingsLoadingState()

        if (diagnosis.isNotBlank()) {
            Text(
                text = diagnosis,
                style = MaterialTheme.typography.bodySmall,
                fontFamily = FontFamily.Monospace,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 4.dp)
            )
            TextButton(onClick = {
                val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                clipboard.setPrimaryClip(ClipData.newPlainText("cheat diagnosis", diagnosis))
                Toast.makeText(context, context.getString(R.string.settings_cheats_copied), Toast.LENGTH_SHORT).show()
            }) {
                Text(text = stringResource(R.string.settings_cheats_copy))
            }
        }

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
            SettingsNote(
                text = if (!inGame) {
                    stringResource(R.string.settings_cheats_empty)
                } else {
                    stringResource(R.string.settings_cheats_empty_game, runningId.orEmpty())
                }
            )
            SettingsNote(text = stringResource(R.string.settings_cheats_empty_hint))
        } else {
            files.forEach { file ->
                val isOpen = expanded == file.titleId
                val isRunning = inGame && runningId == file.titleId
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
                    title = file.displayName,
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
                                    title = entry.displayName,
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
                                        title = entry.displayName,
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

                        SettingsNote(
                            text = if (isRunning) {
                                stringResource(R.string.settings_cheats_takes_effect_live)
                            } else {
                                stringResource(R.string.settings_cheats_takes_effect)
                            }
                        )
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
