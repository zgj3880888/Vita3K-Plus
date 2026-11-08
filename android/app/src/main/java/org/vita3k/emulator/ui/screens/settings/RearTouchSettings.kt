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
 * Two switches: whether L2/R2 drive the rear points at all, and how the finger they stand for
 * behaves. A tap reports one finger sitting still, which is what a game that treats the back panel
 * as a button wants; a stroke drags that finger from the top of the panel to the bottom, which is
 * what a game that reads the panel as a gesture needs. Plenty of games only understand one of the
 * two, so the choice is the player's.
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
        SettingsToggleRow(
            title = stringResource(R.string.settings_rear_touch_swipe),
            checked = overlayConfig.padRearTouchSwipe,
            onCheckedChange = { onOverlayConfigChange(overlayConfig.withPadRearTouchSwipe(it)) },
            // meaningless while the triggers do not drive the panel at all
            enabled = overlayConfig.padRearTouchMapping,
            summary = stringResource(R.string.settings_rear_touch_swipe_summary),
            help = SettingsHelpEntry(
                title = stringResource(R.string.settings_rear_touch_swipe),
                body = stringResource(R.string.settings_rear_touch_swipe_desc),
                scope = SettingsScope.Both
            ),
            onShowHelp = onShowHelp
        )
        SettingsNote(text = stringResource(R.string.settings_rear_touch_note))
    }
}
