package org.vita3k.emulator.ui.screens.settings

import android.content.Intent
import android.provider.DocumentsContract
import android.widget.Toast
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

    // Reading the folder parses every cheat file, so it never runs on the composition thread.
    suspend fun refreshFiles() {
        loading = true
        val loaded = withContext(Dispatchers.IO) {
            runCatching { NativeLib.getCheatFiles().toList() }.getOrDefault(emptyList())
        }
        val path = withContext(Dispatchers.IO) {
            runCatching { NativeLib.getCheatDirectory() }.getOrDefault("")
        }
        files = loaded
        directory = path
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

        if (loading && files.isEmpty()) {
            SettingsLoadingState()
        } else if (files.isEmpty()) {
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
