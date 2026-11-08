package org.vita3k.emulator.ui.screens.settings

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.runtime.Composable
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import org.vita3k.emulator.R
import org.vita3k.emulator.overlay.OverlayConfig

/**
 * Rear panel mapping. The Vita has a second touch panel on the back of the console, and a fair
 * number of games put real buttons there; a pad player has no finger to spare back there, so the
 * triggers stand in for it.
 *
 * One switch, because that is all there is to decide: the places the fingers land are mapping points
 * 5 and 6, which the controls editor already lets the player drag around, and those two always
 * report on the back panel whatever the front/back switch says.
 */
@Composable
internal fun RearTouchSettingsSection(
    overlayConfig: OverlayConfig,
    onOverlayConfigChange: (OverlayConfig) -> Unit,
    onShowHelp: (SettingsHelpEntry) -> Unit
) {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        SettingsToggleRow(
            title = stringResource(R.string.settings_rear_touch_l2r2),
            checked = overlayConfig.padRearTouchMapping,
            onCheckedChange = { onOverlayConfigChange(overlayConfig.withPadRearTouchMapping(it)) },
            // never greyed out with the on-screen controls: this is for playing with the pad alone,
            // which is exactly when nothing is drawn on the screen
            enabled = true,
            summary = stringResource(R.string.settings_rear_touch_l2r2_summary),
            help = SettingsHelpEntry(
                title = stringResource(R.string.settings_rear_touch_l2r2),
                body = stringResource(R.string.settings_rear_touch_l2r2_desc),
                scope = SettingsScope.Both
            ),
            onShowHelp = onShowHelp
        )
        SettingsNote(text = stringResource(R.string.settings_rear_touch_note))
    }
}
