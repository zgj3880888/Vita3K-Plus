package org.vita3k.emulator.ui.screens.settings

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.runtime.Composable
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import org.vita3k.emulator.R
import org.vita3k.emulator.overlay.OverlayConfig
import org.vita3k.emulator.overlay.TouchGesture

/**
 * What a mapping point does once a pad button holds it down.
 *
 * A tap reports one finger sitting still. Games that use the panel as a button are happy with that,
 * and it is all every earlier build could do; games that read the panel as a gesture compare where
 * the finger was with where it went, and a finger that never moves never goes anywhere. So each
 * point can be told to run a stroke instead: a swipe in one of the four directions, or a rub that
 * goes back and forth for the games that ask to be wiped.
 *
 * The list only covers the points a pad can actually press, because a stroke nobody can trigger is
 * a menu entry that does nothing. Which button presses which point is not decided here: it is the
 * same arrangement the switches on the rear touch page describe, and this page only says what
 * happens afterwards.
 */
@Composable
internal fun TouchGestureSettingsSection(
    overlayConfig: OverlayConfig,
    onOverlayConfigChange: (OverlayConfig) -> Unit,
    onShowHelp: (SettingsHelpEntry) -> Unit
) {
    val options = listOf(
        stringResource(R.string.settings_touch_gesture_none),
        stringResource(R.string.settings_touch_gesture_up),
        stringResource(R.string.settings_touch_gesture_down),
        stringResource(R.string.settings_touch_gesture_left),
        stringResource(R.string.settings_touch_gesture_right),
        stringResource(R.string.settings_touch_gesture_rub_h),
        stringResource(R.string.settings_touch_gesture_rub_v)
    )

    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        SettingsNote(text = stringResource(R.string.settings_touch_gesture_intro))
        TouchGesturePoints.forEach { point ->
            val gesture = overlayConfig.gestureFor(point.slot)
            SettingsChoiceSelector(
                title = if (point.rear) {
                    stringResource(R.string.settings_touch_gesture_point_rear, point.slot + 1, point.button)
                } else {
                    stringResource(R.string.settings_touch_gesture_point, point.slot + 1, point.button)
                },
                options = options,
                selectedIndex = TouchGesture.sanitize(gesture),
                onSelected = { onOverlayConfigChange(overlayConfig.withPadTouchGesture(point.slot, it)) },
                // greyed out exactly when the switch that lets the pad press this point is off, so the
                // page never offers a choice that cannot happen
                enabled = if (point.rear) overlayConfig.padRearTouchMapping else overlayConfig.padTouchMapping,
                help = SettingsHelpEntry(
                    title = stringResource(R.string.settings_tab_touch_gesture),
                    body = stringResource(R.string.settings_touch_gesture_desc),
                    scope = SettingsScope.Both
                ),
                onShowHelp = onShowHelp
            )
        }
        SettingsNote(text = stringResource(R.string.settings_touch_gesture_note))
    }
}

private data class TouchGesturePoint(val slot: Int, val button: String, val rear: Boolean)

// Slots 4 and 5 are the first two rear points, the ones L2/R2 hold; slots 0 and 1 are the first two
// front points, the ones L3/R3 hold. Points 3 and 4 have no button behind them, so they are left out
// rather than listed as settings that would never fire. The options above are listed in the order of
// the TouchGesture values, which is what lets the stored value double as the selected index.
private val TouchGesturePoints = listOf(
    TouchGesturePoint(slot = 0, button = "L3", rear = false),
    TouchGesturePoint(slot = 1, button = "R3", rear = false),
    TouchGesturePoint(slot = 4, button = "L2", rear = true),
    TouchGesturePoint(slot = 5, button = "R2", rear = true)
)
