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
// Presses/releases a mapping point from a pad button (L3 -> slot 0, R3 -> slot 1).
int set_mapped_touch_pad(TouchState &state, int slot, bool active);
void set_pad_touch_mapping(TouchState &state, bool enabled);
void clear_mapped_touch(TouchState &state);
int toggle_touchscreen(TouchState &state);
int touch_get(const SceUID thread_id, EmuEnvState &emuenv, const SceUInt32 &port, SceTouchData *pData, SceUInt32 count, bool is_peek);
void touch_set_force_mode(TouchState &state, int port, bool mode);
#ifdef __ANDROID__
void set_touch_mode(TouchState &state, int mode); // 0 = front, 1 = back, 2 = front + back mirrored, 3 = tilt
#endif
