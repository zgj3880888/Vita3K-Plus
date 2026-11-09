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

#include <motion/event_handler.h>
#include <motion/functions.h>
#include <motion/state.h>

#include <ctrl/state.h>

// the settings are read here directly: whether the stick stands in for the sensors is a property of
// the game's configuration, not of the motion state it feeds
#include <config/state.h>

#include <util/log.h>

#include <SDL3/SDL_gamepad.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>

void set_display_rotation(MotionState &state, int rotation) {
    state.device_native_rotation = static_cast<DeviceRotation>(rotation);
}

static void detect_device_motion_support(MotionState &state) {
    state.device_accel_id = 0;
    state.device_gyro_id = 0;

    int num_sensors;
    auto sensors_id = SDL_GetSensors(&num_sensors);
    if (!sensors_id || (num_sensors <= 0)) {
        state.has_device_motion_support = false;
        return;
    }

    for (int idx = 0; idx < num_sensors; idx++) {
        switch (SDL_GetSensorTypeForID(sensors_id[idx])) {
        case SDL_SENSOR_ACCEL:
            state.device_accel_id = sensors_id[idx];
            break;
        case SDL_SENSOR_GYRO:
            state.device_gyro_id = sensors_id[idx];
            break;
        default:
            break;
        }

        if ((state.device_accel_id != 0) && (state.device_gyro_id != 0))
            break;
    }

    SDL_free(sensors_id);

    state.has_device_motion_support = ((state.device_accel_id != 0) && (state.device_gyro_id != 0));
}

static std::string device_rotation_to_string(DeviceRotation rotation) {
    switch (rotation) {
    case ROTATION_0:
        return "ROTATION_0";
    case ROTATION_90:
        return "ROTATION_90";
    case ROTATION_180:
        return "ROTATION_180";
    case ROTATION_270:
        return "ROTATION_270";
    default:
        return "UNKNOWN_ROTATION";
    }
}

void MotionState::init() {
    reset_runtime();
    has_device_motion_support = false;
    device_accel_id = 0;
    device_gyro_id = 0;
}

void MotionState::clear_device_motion_support() {
    stop_sensor_sampling();
    has_device_motion_support = false;
    device_accel_id = 0;
    device_gyro_id = 0;
}

void MotionState::refresh_device_motion_support() {
    clear_device_motion_support();
    detect_device_motion_support(*this);

    if (has_device_motion_support)
        LOG_INFO("Device has a built-in accelerometer and gyroscope (native display rotation: {}).", device_rotation_to_string(device_native_rotation));
}

void MotionState::stop_sensor_sampling() {
    is_sampling = false;
    device_accel.reset();
    device_gyro.reset();
}

void MotionState::start_sensor_sampling() {
    is_sampling = true;
    if (!has_device_motion_support)
        return;

    const auto open_sensor = [](SDL_SensorPtr &sensor, uint32_t sensor_id, const char *name) {
        if (sensor_id == 0) {
            LOG_ERROR("No {} sensor ID available to open.", name);
            return false;
        }

        SDL_Sensor *raw_sensor = SDL_OpenSensor(sensor_id);
        if (!raw_sensor) {
            LOG_ERROR("Failed to open {} sensor with ID {}: {}", name, sensor_id, SDL_GetError());
            return false;
        }

        sensor.reset(raw_sensor);
        return true;
    };

    if (!open_sensor(device_accel, device_accel_id, "accelerometer") || !open_sensor(device_gyro, device_gyro_id, "gyroscope"))
        clear_device_motion_support();
}

void MotionState::reset_runtime() {
    stop_sensor_sampling();
    synthetic_tilt = false;
    stick_motion = false;
    stick_balance_mode = false;
    gamepad_motion_seen = false;
    // has_gamepad_motion_support is left alone: whether the pad has sensors is a fact about the
    // hardware, not about this session, and a game that asks before the first sample arrives still
    // needs the answer from the last one.
    synthetic_rotation = {};
    synthetic_rotation_rate = {};
    synthetic_turn = 0.0f;
    synthetic_turn_rate = 0.0f;
    synthetic_gyro = {};
    synthetic_yaw = 0.0f;
    synthetic_timestamp = 0;
    motion_data.ResetQuaternion();
    motion_data.ResetRotations();
    last_counter = 0;
    last_gyro_timestamp = 0;
    last_accel_timestamp = 0;
    last_updated_gyro_timestamp = 0;
    last_updated_accel_timestamp = 0;
}

static Util::Quaternion<SceFloat> tilt_quaternion(const Util::Vec3f &rotation) {
    const float angle = rotation.Length();
    if (angle <= 0.0f)
        return { {}, 1.0f };
    return Util::MakeQuaternion(rotation / angle, angle);
}

static Util::Quaternion<SceFloat> pose_quaternion(const Util::Vec3f &rotation, const float turn) {
    return Util::MakeQuaternion({ 0.0f, 0.0f, 1.0f }, turn) * tilt_quaternion(rotation);
}

static Util::Quaternion<SceFloat> synthetic_orientation(const MotionState &state) {
    return Util::MakeQuaternion({ 0.0f, 0.0f, 1.0f }, state.synthetic_yaw) * pose_quaternion(state.synthetic_rotation, state.synthetic_turn);
}

// A pad reporting its own sensors owns the gravity half of the pose: it measured the real one, and
// the stick must not write over it with an invented one. With no such pad the stick has to make the
// whole pose up on its own, gravity included, which is what a balancing section reads.
static bool pad_is_reporting(const MotionState &state) {
    return state.gamepad_motion_seen;
}

SceFVector3 get_acceleration(const MotionState &state) {
    Util::Vec3f accelerometer = state.motion_data.GetAcceleration();
    return {
        accelerometer.x,
        accelerometer.y,
        accelerometer.z,
    };
}

SceFVector3 get_gyroscope(const MotionState &state) {
    if (state.synthetic_tilt) {
        const Util::Vec3f stick = state.synthetic_gyro;
        if (!pad_is_reporting(state))
            return { stick.x, stick.y, stick.z };

        // Both are turning the same pad: the game should see one rate, not whichever of the two
        // wrote last.
        const Util::Vec3f pad = state.motion_data.GetGyroscope() * 2.f * std::numbers::pi_v<float>;
        return { pad.x + stick.x, pad.y + stick.y, pad.z + stick.z };
    }

    Util::Vec3f gyroscope = state.motion_data.GetGyroscope() * 2.f * std::numbers::pi_v<float>;
    return {
        gyroscope.x,
        gyroscope.y,
        gyroscope.z,
    };
}

Util::Quaternion<SceFloat> get_orientation(const MotionState &state) {
    // With a pad reporting, the pose is the pad's own: it is the one really being held at an angle,
    // and mixing an invented pose into it would tilt a wire the player is standing straight on. The
    // stick still gets its say through the rate, further up.
    if (state.synthetic_tilt && !pad_is_reporting(state))
        return synthetic_orientation(state);

    auto quat = state.motion_data.GetOrientation();
    return {
        { -quat.xyz[1], -quat.w, quat.xyz[0] },
        -quat.xyz[2],
    };
}

SceBool get_gyro_bias_correction(const MotionState &state) {
    return state.motion_data.IsGyroBiasEnabled();
}

void set_gyro_bias_correction(MotionState &state, SceBool setValue) {
    state.motion_data.EnableGyroBias(setValue);
}

SceBool get_tilt_correction(MotionState &state) {
    return state.motion_data.IsTiltCorrectionEnabled();
}

void set_tilt_correction(MotionState &state, SceBool setValue) {
    state.motion_data.EnableTiltCorrection(setValue);
}

SceBool get_deadband(MotionState &state) {
    return state.motion_data.IsDeadbandEnabled();
}

void set_deadband(MotionState &state, SceBool setValue) {
    state.motion_data.EnableDeadband(setValue);
}

SceFloat get_angle_threshold(const MotionState &state) {
    return state.motion_data.GetAngleThreshold();
}

void set_angle_threshold(MotionState &state, SceFloat setValue) {
    state.motion_data.SetAngleThreshold(setValue);
}

SceFVector3 get_basic_orientation(const MotionState &state) {
    return state.motion_data.GetBasicOrientation();
}

constexpr uint64_t to_microseconds(uint64_t ns) {
    return ns / 1000;
}

template <typename SensorEvent>
static void handle_motion_event(EmuEnvState &emuenv, int32_t sensor_type, const SensorEvent &sensor) {
    if (!emuenv.motion.is_sampling)
        return;

    const bool from_gamepad = sensor.type == SDL_EVENT_GAMEPAD_SENSOR_UPDATE;

    // The stick stands in for a device that has no sensors of its own, so its samples would be a
    // second writer of the pose it holds. A pad is a different matter: its sensors are exactly what
    // a wiring or balancing section wants, and the stick is busy turning the view, so the two work
    // together instead of one displacing the other.
    if (emuenv.cfg.motion_stick_sim && !from_gamepad)
        return;

    if (!emuenv.ctrl.has_motion_support && !emuenv.motion.has_device_motion_support && !from_gamepad)
        return;

    // A pad and the device both write the same pose, and the device keeps reporting even when it is
    // lying still on a table, so its zeros would overwrite every sample the pad sends. Once a pad has
    // reported, it owns the pose: it is the one the player is actually turning.
    if (from_gamepad) {
        if (!emuenv.motion.gamepad_motion_seen)
            LOG_INFO("[MOTION] the pad is reporting its own sensors, so its samples drive the pose from here on");
        emuenv.motion.gamepad_motion_seen = true;
    } else if (emuenv.motion.gamepad_motion_seen)
        return;

    const auto get_processed_sensor_data = [&]() {
        Util::Vec3f data = { sensor.data[0], sensor.data[1], sensor.data[2] };
        if (!from_gamepad) {
            switch (emuenv.motion.device_native_rotation) {
            case ROTATION_90: // portrait -> landscape left
                std::tie(data.x, data.y, data.z) = std::make_tuple(-data.y, data.x, data.z);
                break;
            case ROTATION_180: // upside-down
                data.x *= -1.f;
                data.y *= -1.f;
                break;
            case ROTATION_270: // portrait -> landscape right
                std::tie(data.x, data.y, data.z) = std::make_tuple(data.y, -data.x, data.z);
                break;
            default: // ROTATION_0 or unknown: no transform
                break;
            }
        } else
            std::tie(data.x, data.y, data.z) = std::make_tuple(data.x, -data.z, data.y);

        return data;
    };

    const uint64_t sensor_timestamp = (sensor.sensor_timestamp > 0)
        ? to_microseconds(sensor.sensor_timestamp) // convert ns -> us
        : std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();

    auto sensor_data = get_processed_sensor_data();

    if (sensor_type == SDL_SENSOR_ACCEL) {
        sensor_data /= -SDL_STANDARD_GRAVITY;
        emuenv.motion.motion_data.SetAcceleration(sensor_data);
        emuenv.motion.last_updated_accel_timestamp = sensor_timestamp;
    } else if (sensor_type == SDL_SENSOR_GYRO) {
        sensor_data /= 2.f * std::numbers::pi_v<float>;
        emuenv.motion.motion_data.SetGyroscope(sensor_data);
        emuenv.motion.last_updated_gyro_timestamp = sensor_timestamp;
    }
}

void handle_motion_event(EmuEnvState &emuenv, int32_t sensor_type, const SDL_SensorEvent &sensor) {
    handle_motion_event<SDL_SensorEvent>(emuenv, sensor_type, sensor);
}

void handle_motion_event(EmuEnvState &emuenv, int32_t sensor_type, const SDL_GamepadSensorEvent &sensor) {
    handle_motion_event<SDL_GamepadSensorEvent>(emuenv, sensor_type, sensor);
}

void set_android_gamepad_motion(MotionState &state, const int sensor_type, const float x, const float y, const float z, const uint64_t timestamp_ns) {
    if (!state.is_sampling)
        return;

    // An Android sensor speaks the same axes a Vita does - +x to the right, +y up, +z out of the
    // face - which is why the device's own sensors are stored untransformed above. A pad read
    // through its own sensor manager comes in on those same axes, so it goes in as it arrives; the
    // swap the SDL branch applies belongs to the frame SDL reports a pad's sensors in, not to this
    // one, and reusing it here would trade the lean for a roll.
    const Util::Vec3f data = { x, y, z };
    const uint64_t timestamp_us = (timestamp_ns > 0)
        ? timestamp_ns / 1000
        : std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();

    if (sensor_type == SDL_SENSOR_ACCEL) {
        // gravity included, in metres per second squared; the pose keeps it normalized to g
        state.motion_data.SetAcceleration(data / -SDL_STANDARD_GRAVITY);
        state.last_updated_accel_timestamp = timestamp_us;
    } else if (sensor_type == SDL_SENSOR_GYRO) {
        // radians per second here, turns per second in the pose
        state.motion_data.SetGyroscope(data / (2.f * std::numbers::pi_v<float>));
        state.last_updated_gyro_timestamp = timestamp_us;
    } else {
        return;
    }

    // The pad owns the pose from the first sample on. The tablet keeps reporting its own zeros while
    // it lies still, and those would overwrite these one for one.
    if (!state.gamepad_motion_seen) {
        state.gamepad_motion_seen = true;
        LOG_INFO("[MOTION] the pad's own sensors are reporting: the pose leans with the pad, and the stick keeps turning the view");
    }
    state.has_gamepad_motion_support = true;
}

// Builds the pose the synthetic tilt machinery holds. The touch panel and the right stick both end
// up here, so the smoothing, the acceleration the pose reports and the gyro derived from how fast it
// moves are the same whichever one is driving. scale widens the tilt for a player who wants a full
// lean from less travel; turn_share decides how much of the sideways push also turns the view, which
// the touch panel has always done and the stick must not, or the view would swing every time the
// stick leaned.
static void apply_synthetic_pose(MotionState &state, const bool active, const float x, const float y, const float scale, const float turn_share) {
    constexpr float max_tilt = std::numbers::pi_v<float> / 4.0f;
    constexpr float max_turn = 2.0f * std::numbers::pi_v<float> / 3.0f;
    constexpr float response = 15.0f;
    const float reach = std::sqrt(x * x + y * y);
    Util::Vec3f target;
    if (active && reach > 0.0f) {
        // the widened tilt stops short of a right angle, where the orientation would be ambiguous
        const float angle = std::min(std::min(reach, 1.0f) * max_tilt * scale, 1.45f);
        target = { y / reach * angle, x / reach * angle, 0.0f };
    }
    const float target_turn = (active && turn_share > 0.0f)
        ? -std::clamp(x, -1.0f, 1.0f) * max_turn * turn_share
        : 0.0f;
    const uint64_t now = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();

    const std::lock_guard<std::mutex> guard(state.mutex);
    float dt = 1.0f / 60.0f;
    if (!state.synthetic_tilt) {
        state.synthetic_tilt = true;
        state.synthetic_rotation = {};
        state.synthetic_rotation_rate = {};
        state.synthetic_turn = 0.0f;
        state.synthetic_turn_rate = 0.0f;
    } else if (now > state.synthetic_timestamp) {
        dt = std::min(static_cast<float>(now - state.synthetic_timestamp) / 1000000.0f, 0.1f);
    }
    state.synthetic_timestamp = now;

    const Util::Vec3f error = state.synthetic_rotation - target;
    const Util::Vec3f slope = state.synthetic_rotation_rate + error * response;
    const float decay = std::exp(-response * dt);
    Util::Vec3f next_error = (error + slope * dt) * decay;
    Util::Vec3f next_rate = (state.synthetic_rotation_rate - slope * (response * dt)) * decay;
    if (next_error.Length() < 1e-5f && next_rate.Length() < 1e-4f) {
        next_error = {};
        next_rate = {};
    }

    const float turn_error = state.synthetic_turn - target_turn;
    const float turn_slope = state.synthetic_turn_rate + turn_error * response;
    float next_turn_error = (turn_error + turn_slope * dt) * decay;
    float next_turn_rate = (state.synthetic_turn_rate - turn_slope * (response * dt)) * decay;
    if (std::abs(next_turn_error) < 1e-5f && std::abs(next_turn_rate) < 1e-4f) {
        next_turn_error = 0.0f;
        next_turn_rate = 0.0f;
    }

    const auto previous = pose_quaternion(state.synthetic_rotation, state.synthetic_turn);
    state.synthetic_rotation = target + next_error;
    state.synthetic_rotation_rate = next_rate;
    state.synthetic_turn = target_turn + next_turn_error;
    state.synthetic_turn_rate = next_turn_rate;
    const auto current = pose_quaternion(state.synthetic_rotation, state.synthetic_turn);

    auto step = previous.Inverse() * current;
    if (step.w < 0.0f)
        step = { -step.xyz, -step.w };
    const float half_sine = step.xyz.Length();
    state.synthetic_gyro = half_sine > 0.0f ? step.xyz * (2.0f * std::atan2(half_sine, step.w) / half_sine / dt) : Util::Vec3f{};

    // Only when the pose is entirely this code's own: a pad that reports has a real gravity reading,
    // and writing an invented one over it would put the lean a balancing section reads out of joint.
    if (!pad_is_reporting(state)) {
        state.motion_data.SetAcceleration(Util::QuaternionRotate(current.Inverse(), Util::Vec3f{ 0.0f, 0.0f, -1.0f }));
        state.last_updated_accel_timestamp = now;
    }
}

void set_touch_tilt(MotionState &state, const CtrlState &ctrl_state, const bool active, const float x, const float y) {
    if (!state.is_sampling)
        return;

    // the stick is working the pose this frame; the panel must not fight it for the same fields
    if (state.stick_motion)
        return;

    if (ctrl_state.has_motion_support || state.has_device_motion_support) {
        if (state.synthetic_tilt) {
            const std::lock_guard<std::mutex> guard(state.mutex);
            state.synthetic_tilt = false;
            state.synthetic_rotation = {};
            state.synthetic_rotation_rate = {};
            state.synthetic_turn = 0.0f;
            state.synthetic_turn_rate = 0.0f;
            state.synthetic_gyro = {};
        }
        return;
    }

    if (!active && !state.synthetic_tilt)
        return;

    apply_synthetic_pose(state, active, x, y, 1.0f, 1.0f);
}

// Aiming mode. Releasing the stick has to stop the view dead, so what the stick reports is a rate
// and not an angle: nothing pulls the pose back to level, so there is no return journey for the game
// to read as more turning. That return journey is exactly what made aiming drift on after release.
//
// The axis order is the one the pad's own samples arrive in, after the swap applied in
// set_android_gamepad_motion: x turns the view up and down, z turns it left and right. Writing the
// rate in that order is what lets it be added to a pad's own rate and mean the same thing.
static void apply_stick_turn(MotionState &state, const float sensitivity, const float x, const float y) {
    // radians per second at full deflection and 1.0 sensitivity, with a deadzone wide enough that a
    // resting stick with a little play in it does not slowly turn the view all session long
    constexpr float max_rate = 2.5f;
    constexpr float deadzone = 0.10f;

    const float turn_x = std::fabs(x) > deadzone ? x : 0.0f;
    const float turn_y = std::fabs(y) > deadzone ? y : 0.0f;

    const float yaw_rate = -turn_x * max_rate * sensitivity;
    const float pitch_rate = turn_y * max_rate * sensitivity;

    const std::lock_guard<std::mutex> guard(state.mutex);
    if (!state.synthetic_tilt) {
        state.synthetic_tilt = true;
        state.synthetic_rotation = {};
        state.synthetic_rotation_rate = {};
        state.synthetic_turn = 0.0f;
        state.synthetic_turn_rate = 0.0f;
    }
    state.synthetic_gyro = { pitch_rate, 0.0f, yaw_rate };

    // A pad with sensors of its own already holds the pose and the gravity the game reads; all this
    // adds is the rate the stick is asking for, which is summed in on the way out.
    if (pad_is_reporting(state)) {
        state.synthetic_rotation = {};
        state.synthetic_turn = 0.0f;
        return;
    }

    // No pad behind it, so the stick has to carry the pose as well as the rate: turn it, and let the
    // gravity the game reads follow the turning.
    const uint64_t now = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    float dt = 1.0f / 60.0f;
    if (now > state.synthetic_timestamp)
        dt = std::min(static_cast<float>(now - state.synthetic_timestamp) / 1000000.0f, 0.1f);
    state.synthetic_timestamp = now;

    state.synthetic_turn = std::remainder(state.synthetic_turn + yaw_rate * dt, 2.0f * std::numbers::pi_v<float>);
    // kept short of a right angle, where the orientation stops saying which way is up
    state.synthetic_rotation.x = std::clamp(state.synthetic_rotation.x + pitch_rate * dt, -1.4f, 1.4f);

    const auto current = pose_quaternion(state.synthetic_rotation, state.synthetic_turn);
    state.motion_data.SetAcceleration(Util::QuaternionRotate(current.Inverse(), Util::Vec3f{ 0.0f, 0.0f, -1.0f }));
    state.last_updated_accel_timestamp = now;
}

void update_stick_motion(MotionState &state, const bool enabled, const bool balance_mode, const float sensitivity, const float x, const float y) {
    // Sampling stops whenever no game holds the sensors open, and a simulation that kept running
    // would leave a pose behind for the next game to find. Give the pose back when it does.
    if (!enabled || !state.is_sampling) {
        if (!state.stick_motion)
            return;

        state.stick_motion = false;
        if (!state.synthetic_tilt)
            return;

        const std::lock_guard<std::mutex> guard(state.mutex);
        state.synthetic_tilt = false;
        state.synthetic_rotation = {};
        state.synthetic_rotation_rate = {};
        state.synthetic_turn = 0.0f;
        state.synthetic_turn_rate = 0.0f;
        state.synthetic_gyro = {};
        return;
    }

    // The two modes build the pose in different ways, so a switch between them starts from level
    // rather than from whatever the other one had accumulated. A pad with sensors of its own leans
    // for real, so the stick is left to turn the view whatever the balance switch says - that is
    // what lets a wire be walked by waving the pad and a shot aimed by pushing the stick, at once.
    const bool lean = balance_mode && !pad_is_reporting(state);

    if (!state.stick_motion) {
        state.stick_motion = true;
        LOG_INFO("[MOTION] the right stick is standing in for the six-axis sensors ({} mode, sensitivity {:.2f})",
            lean ? "balance" : "aim", sensitivity);
    }

    if (state.stick_balance_mode != lean) {
        state.stick_balance_mode = lean;
        const std::lock_guard<std::mutex> guard(state.mutex);
        state.synthetic_tilt = false;
    }

    // Always active, a centred stick included: stopping on a centred stick would hand the pose back
    // to sensors the pad may not even have, which is the very thing this mode exists to avoid.
    if (lean)
        apply_synthetic_pose(state, true, x, y, sensitivity, 0.0f);
    else
        apply_stick_turn(state, sensitivity, x, y);
}

void refresh_motion(MotionState &state, CtrlState &ctrl_state) {
    if (!state.is_sampling)
        return;

    // With only the stick behind the pose there is no sensor to integrate: the pose was built whole
    // by the code above. With a pad reporting, that path has to keep running underneath, because the
    // lean the stick is not allowed to invent comes from there.
    if (state.synthetic_tilt && !pad_is_reporting(state)) {
        state.motion_data.UpdateBasicOrientation();
        state.last_accel_timestamp = state.last_updated_accel_timestamp;
        state.last_counter++;
        return;
    }

    if (!ctrl_state.has_motion_support && !state.has_device_motion_support && !state.has_gamepad_motion_support)
        return;

    state.motion_data.UpdateOrientation(state.last_updated_accel_timestamp - state.last_accel_timestamp);
    state.motion_data.UpdateBasicOrientation();
    state.motion_data.UpdateRotation(state.last_updated_gyro_timestamp - state.last_gyro_timestamp);

    state.last_accel_timestamp = state.last_updated_accel_timestamp;
    state.last_gyro_timestamp = state.last_updated_gyro_timestamp;

    state.last_counter++;
}
