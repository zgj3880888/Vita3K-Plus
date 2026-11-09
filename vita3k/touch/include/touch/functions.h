#pragma once

#include <touch/touch.h>

#include <vector>

struct EmuEnvState;
struct TouchState;

std::vector<SceFVector2> get_touchpad_fingers_pos(const TouchState &state, SceTouchPortType &port);
int handle_touchpad_event(TouchState &state, SDL_GamepadTouchpadEvent &touchpad);
void touch_vsync_update(EmuEnvState &emuenv);
void pinch_modifier(TouchState &state, bool isHold);
void pinch_move(TouchState &state, float velocity);
void pinch_automove(TouchState &state, float velocity);
int handle_touch_event(TouchState &state, SDL_TouchFingerEvent &finger);
int set_mapped_touch(TouchState &state, int slot, float x, float y, bool active);
// Moves a mapping point without touching its pressed state, so a pad button can hold it later even
// when the overlay ring is hidden.
int set_mapped_touch_position(TouchState &state, int slot, float x, float y);
// Presses/releases a mapping point from a pad button (L3 -> slot 0, R3 -> slot 1, L2/R2 -> the rear
// points). The caller decides whether the mapping that drives the slot is on, because the same call
// serves both switches.
int set_mapped_touch_pad(TouchState &state, int slot, bool active);
// Turns on the pad's hold of the front points (first switch) or of the rear points (second one).
// Turning one off lifts whatever that switch was holding, so no finger is left on the panel.
void set_pad_touch_mapping(TouchState &state, bool enabled);
void set_pad_rear_touch_mapping(TouchState &state, bool enabled);
// Picks how a rear point held by L2/R2 reaches the guest: a finger parked on the spot (off) or a
// stroke down the panel (on). Games that read the back panel as a gesture do not react to the
// former, which is the whole reason this exists.
void set_pad_rear_touch_swipe(TouchState &state, bool enabled);
// Sends every mapping point to the screen instead of the back panel, whatever its slot. A game that
// only reads the front panel never sees a rear point otherwise, which makes the triggers useless in
// exactly the games a pad player would want them for.
void set_pad_touch_front(TouchState &state, bool enabled);
// Picks what the finger does while a pad button holds this point down: stay put, or run one of the
// strokes in MappedTouchGesture. Staying put is what every earlier build did and what a game that
// uses the panel as a button wants; a stroke is for the games that compare where the finger was
// with where it went. Stored per point and kept across a restart.
void set_mapped_touch_gesture(TouchState &state, int slot, int gesture);
void clear_mapped_touch(TouchState &state);
int toggle_touchscreen(TouchState &state);
int touch_get(const SceUID thread_id, EmuEnvState &emuenv, const SceUInt32 &port, SceTouchData *pData, SceUInt32 count, bool is_peek);
void touch_set_force_mode(TouchState &state, int port, bool mode);
#ifdef __ANDROID__
void set_touch_mode(TouchState &state, int mode); // 0 = front, 1 = back, 2 = front + back mirrored, 3 = tilt
#endif
