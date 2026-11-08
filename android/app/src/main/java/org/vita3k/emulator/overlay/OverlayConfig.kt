package org.vita3k.emulator.overlay

import android.content.Context

val DEFAULT_OVERLAY_MASK: Int =
    InputOverlay.OVERLAY_MASK_BASIC or InputOverlay.OVERLAY_MASK_TOUCH_SCREEN_SWITCH

// What a finger does while a pad button holds a mapping point down. The numbers are the order of
// MappedTouchGesture on the native side and must stay in step with it. NONE is the tap every
// earlier build produced: one finger, always in the same place, which is all a game that treats the
// panel as a button asks for. Everything below it is a stroke for the games that compare where the
// finger was with where it went.
object TouchGesture {
    const val NONE = 0
    const val UP = 1
    const val DOWN = 2
    const val LEFT = 3
    const val RIGHT = 4
    const val RUB_H = 5
    const val RUB_V = 6

    val ALL = listOf(NONE, UP, DOWN, LEFT, RIGHT, RUB_H, RUB_V)

    fun sanitize(gesture: Int): Int = if (gesture in ALL) gesture else NONE

    fun slots(): Int = InputOverlay.ButtonType.TOUCH_MAP_COUNT
}

data class OverlayConfig(
    val overlayMask: Int = DEFAULT_OVERLAY_MASK,
    val overlayScale: Int = 100,
    val overlayOpacity: Int = 100,
    val hideOverlayWhenControllerConnected: Boolean = true,
    val touchSwitchTilt: Boolean = false,
    // L3 holds mapping point 1 and R3 holds point 2, so a pad can tap the panel on its own
    val padTouchMapping: Boolean = true,
    // L2 holds rear point 5 and R2 holds rear point 6, so a pad can use the back panel on its own
    val padRearTouchMapping: Boolean = true,
    // false taps the rear point, true drags it from the top of the panel to the bottom; games that
    // read the back panel as a gesture ignore a finger that never moves
    val padRearTouchSwipe: Boolean = false,
    // One entry per mapping point, in the order the points are numbered: the stroke that point runs
    // while a pad button holds it. Separate from the switches above on purpose, because what a point
    // does once pressed has nothing to do with whether the pad is allowed to press it.
    val padTouchGestures: List<Int> = List(TouchGesture.slots()) { TouchGesture.NONE }
) {
    val controlsVisible: Boolean
        get() = overlayMask != 0

    val l2r2Visible: Boolean
        get() = overlayMask and InputOverlay.OVERLAY_MASK_L2R2 != 0

    val l3r3Visible: Boolean
        get() = overlayMask and InputOverlay.OVERLAY_MASK_L3R3 != 0

    val touchSwitchVisible: Boolean
        get() = overlayMask and InputOverlay.OVERLAY_MASK_TOUCH_SCREEN_SWITCH != 0

    val hideToggleVisible: Boolean
        get() = overlayMask and InputOverlay.OVERLAY_MASK_HIDE_TOGGLE != 0

    val touchMapVisible: Boolean
        get() = overlayMask and InputOverlay.OVERLAY_MASK_TOUCH_MAP != 0

    fun activeMask(): Int = if (overlayMask == 0) DEFAULT_OVERLAY_MASK else overlayMask

    fun withControlsVisible(visible: Boolean): OverlayConfig {
        return copy(overlayMask = if (visible) activeMask() else 0).normalized()
    }

    fun withL2R2Visible(visible: Boolean): OverlayConfig {
        val baseMask = activeMask()
        val updatedMask = if (visible) {
            baseMask or InputOverlay.OVERLAY_MASK_L2R2
        } else {
            baseMask and InputOverlay.OVERLAY_MASK_L2R2.inv()
        }
        return copy(overlayMask = updatedMask).normalized()
    }

    fun withL3R3Visible(visible: Boolean): OverlayConfig {
        val baseMask = activeMask()
        val updatedMask = if (visible) {
            baseMask or InputOverlay.OVERLAY_MASK_L3R3
        } else {
            baseMask and InputOverlay.OVERLAY_MASK_L3R3.inv()
        }
        return copy(overlayMask = updatedMask).normalized()
    }

    fun withTouchSwitchVisible(visible: Boolean): OverlayConfig {
        val baseMask = activeMask()
        val updatedMask = if (visible) {
            baseMask or InputOverlay.OVERLAY_MASK_TOUCH_SCREEN_SWITCH
        } else {
            baseMask and InputOverlay.OVERLAY_MASK_TOUCH_SCREEN_SWITCH.inv()
        }
        return copy(overlayMask = updatedMask).normalized()
    }

    fun withHideToggleVisible(visible: Boolean): OverlayConfig {
        val baseMask = activeMask()
        val updatedMask = if (visible) {
            baseMask or InputOverlay.OVERLAY_MASK_HIDE_TOGGLE
        } else {
            baseMask and InputOverlay.OVERLAY_MASK_HIDE_TOGGLE.inv()
        }
        return copy(overlayMask = updatedMask).normalized()
    }

    fun withTouchMapVisible(visible: Boolean): OverlayConfig {
        val baseMask = activeMask()
        val updatedMask = if (visible) {
            baseMask or InputOverlay.OVERLAY_MASK_TOUCH_MAP
        } else {
            baseMask and InputOverlay.OVERLAY_MASK_TOUCH_MAP.inv()
        }
        return copy(overlayMask = updatedMask).normalized()
    }

    fun withPadTouchMapping(enabled: Boolean): OverlayConfig =
        copy(padTouchMapping = enabled).normalized()

    fun withPadRearTouchMapping(enabled: Boolean): OverlayConfig =
        copy(padRearTouchMapping = enabled).normalized()

    fun withPadRearTouchSwipe(enabled: Boolean): OverlayConfig =
        copy(padRearTouchSwipe = enabled).normalized()

    fun withPadTouchGesture(index: Int, gesture: Int): OverlayConfig {
        if (index < 0 || index >= TouchGesture.slots()) return this
        val updated = padTouchGestures.toMutableList()
        updated[index] = TouchGesture.sanitize(gesture)
        return copy(padTouchGestures = updated).normalized()
    }

    fun gestureFor(index: Int): Int =
        padTouchGestures.getOrElse(index) { TouchGesture.NONE }

    // The list is persisted as a joined string, so it can arrive short, long or full of leftovers
    // from a build that knew more gestures than this one: pad it, trim it, and drop anything that is
    // not a gesture this build can run.
    private fun normalizedGestures(): List<Int> {
        val slots = TouchGesture.slots()
        return List(slots) { index ->
            TouchGesture.sanitize(padTouchGestures.getOrElse(index) { TouchGesture.NONE })
        }
    }

    fun normalized(): OverlayConfig {
        val knownMask = InputOverlay.OVERLAY_MASK_BASIC or
            InputOverlay.OVERLAY_MASK_L2R2 or
            InputOverlay.OVERLAY_MASK_L3R3 or
            InputOverlay.OVERLAY_MASK_TOUCH_SCREEN_SWITCH or
            InputOverlay.OVERLAY_MASK_HIDE_TOGGLE or
            InputOverlay.OVERLAY_MASK_TOUCH_MAP
        return copy(
            overlayMask = overlayMask.coerceAtLeast(0) and knownMask,
            overlayScale = overlayScale.coerceIn(50, 150),
            overlayOpacity = overlayOpacity.coerceIn(20, 100),
            padTouchGestures = normalizedGestures()
        )
    }
}

object OverlayConfigStore {
    fun load(
        context: Context,
        scopeId: String? = null,
        allowGameOverride: Boolean = false
    ): OverlayConfig = OverlayStore.loadConfig(context, scopeId, allowGameOverride)

    fun save(
        context: Context,
        config: OverlayConfig,
        scopeId: String? = null,
        useGameOverride: Boolean = false
    ) {
        OverlayStore.saveConfig(context, config, scopeId, useGameOverride)
    }
}
