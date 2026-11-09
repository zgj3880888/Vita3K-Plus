// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#pragma once

#include <emuenv/state.h>
#include <motion/state.h>

SceFVector3 get_acceleration(const MotionState &state);
SceFVector3 get_gyroscope(const MotionState &state);
Util::Quaternion<SceFloat> get_orientation(const MotionState &state);
SceBool get_gyro_bias_correction(const MotionState &state);
void set_gyro_bias_correction(MotionState &state, SceBool setValue);
SceBool get_tilt_correction(MotionState &state);
void set_tilt_correction(MotionState &state, SceBool setValue);
SceBool get_deadband(MotionState &state);
#ifdef __ANDROID__
void set_display_rotation(MotionState &state, int rotation);
#endif
void set_deadband(MotionState &state, SceBool setValue);
SceFloat get_angle_threshold(const MotionState &state);
void set_angle_threshold(MotionState &state, SceFloat setValue);
SceFVector3 get_basic_orientation(const MotionState &state);

void set_touch_tilt(MotionState &state, const CtrlState &ctrl_state, bool active, float x, float y);
// Runs the six-axis pose from the right stick for pads that have no sensors of their own, or for a
// player who would rather aim with the stick than wave the pad about. Takes over from both the touch
// panel tilt and the real sensors while enabled, and hands the pose back when switched off.
void update_stick_motion(MotionState &state, bool enabled, float sensitivity, float x, float y);
// A pad's own sensors, read through the Android input device instead of SDL. The platform hands out
// a sensor manager per input device from API 31, which is the only way to see a Bluetooth pad's gyro
// on Android: SDL's hidapi cannot reach it there. sensor_type is SDL_SENSOR_ACCEL or SDL_SENSOR_GYRO.
void set_android_gamepad_motion(MotionState &state, bool stick_sim, int sensor_type, float x, float y, float z, uint64_t timestamp_ns);
void refresh_motion(MotionState &state, CtrlState &ctrl_state);
