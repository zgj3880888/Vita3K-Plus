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

#include <ctrl/state.h>
#include <dialog/state.h>
#include <display/functions.h>
#include <display/state.h>
#include <emuenv/state.h>
#include <kernel/state.h>
#include <touch/functions.h>
#include <touch/state.h>
#include <touch/touch.h>

#include <util/log.h>

#include <SDL3/SDL_events.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

void set_touch_mode(TouchState &state, int mode) {
    state.touchscreen_port = (mode == 1) ? SCE_TOUCH_PORT_BACK : SCE_TOUCH_PORT_FRONT;
    state.touchscreen_both = (mode == 2);
    state.touchscreen_tilt = (mode == 3);
}

static void set_tilt_target(TouchState &state, const float x, const float y) {
    if (!std::isfinite(x) || !std::isfinite(y))
        return;
    state.tilt_active = true;
    state.tilt_x = std::clamp(x * 2.0f - 1.0f, -1.0f, 1.0f);
    state.tilt_y = std::clamp(y * 2.0f - 1.0f, -1.0f, 1.0f);
}

constexpr uint64_t TOUCH_LATCH_US = 100000;

static uint64_t touch_now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void remove_finger(TouchState &state, const int index) {
    for (int i = index; i + 1 < state.finger_count; i++) {
        state.finger_buffer[i] = state.finger_buffer[i + 1];
        state.finger_first_seq[i] = state.finger_first_seq[i + 1];
        state.finger_down_us[i] = state.finger_down_us[i + 1];
        state.finger_lifted[i] = state.finger_lifted[i + 1];
    }
    state.finger_count--;
}

static bool finger_read_by_game(const TouchState &state, const int index) {
    const uint64_t first_seq = state.finger_first_seq[index];
    if (!first_seq)
        return false;
    for (int port = 0; port < 2; port++) {
        if (!state.touchscreen_both && port != state.touchscreen_port)
            continue;
        if (state.last_read_seq[port].load(std::memory_order_acquire) >= first_seq)
            return true;
    }
    return false;
}

static void drop_lifted_fingers(TouchState &state, const uint64_t now_us, const bool all) {
    for (int i = state.finger_count - 1; i >= 0; i--) {
        if (state.finger_lifted[i] && (all || finger_read_by_game(state, i) || now_us - state.finger_down_us[i] >= TOUCH_LATCH_US))
            remove_finger(state, i);
    }
}

static int find_down_finger(const TouchState &state, const SDL_FingerID finger_id) {
    for (int i = 0; i < state.finger_count; i++) {
        if (!state.finger_lifted[i] && state.finger_buffer[i].fingerID == finger_id)
            return i;
    }
    return -1;
}

static void reset_pinch(TouchState &state) {
    state.pinch_dist = TouchState::initial_pinch_dist;
}

static bool is_common_dialog_running(const EmuEnvState &emuenv) {
    return emuenv.common_dialog.type != NO_DIALOG
        && emuenv.common_dialog.status == SCE_COMMON_DIALOG_STATUS_RUNNING;
}

static SceTouchData recover_touch_events(const EmuEnvState &emuenv, const int port) {
    const auto &touch = emuenv.touch;
    SceTouchData touch_data;
    memset(&touch_data, 0, sizeof(touch_data));

    for (uint8_t i = 0; i < touch.finger_count; i++) {
        touch_data.report[i].id = static_cast<uint8_t>(touch.finger_buffer[i].touchID);
        touch_data.report[i].force = touch.force_touch_enabled[port] ? 128 : 0;

        float x = (touch.finger_buffer[i].x * emuenv.display.viewport_drawable_w - emuenv.display.viewport_x) / emuenv.display.viewport_w;
        float y = (touch.finger_buffer[i].y * emuenv.display.viewport_drawable_h - emuenv.display.viewport_y) / emuenv.display.viewport_h;
        touch_data.report[i].x = static_cast<uint16_t>(x * 1920);

        if (port == SCE_TOUCH_PORT_FRONT) {
            touch_data.report[i].y = static_cast<uint16_t>(y * 1088);
        } else {
            touch_data.report[i].y = static_cast<uint16_t>(108 + y * 781);
        }
    }

    touch_data.reportNum = touch.finger_count;

    return touch_data;
}

static SceTouchData recover_touchpad_events(const EmuEnvState &emuenv, const int port) {
    const auto &touch = emuenv.touch;
    SceTouchData touch_data;
    memset(&touch_data, 0, sizeof(touch_data));

    for (uint8_t i = 0; i < touch.touchpad_finger_count; i++) {
        touch_data.report[i].id = static_cast<uint8_t>(touch.touchpad_buffer[i].which);
        touch_data.report[i].force = touch.force_touch_enabled[port] ? 128 : 0;

        touch_data.report[i].x = static_cast<uint16_t>(touch.touchpad_buffer[i].x * 1920);
        if (port == SCE_TOUCH_PORT_FRONT) {
            touch_data.report[i].y = static_cast<uint16_t>(touch.touchpad_buffer[i].y * 1088);
        } else {
            touch_data.report[i].y = static_cast<uint16_t>(108 + touch.touchpad_buffer[i].y * 781);
        }
    }

    touch_data.reportNum = touch.touchpad_finger_count;

    return touch_data;
}

void touch_vsync_update(EmuEnvState &emuenv) {
    auto &touch = emuenv.touch;
    std::chrono::time_point<std::chrono::steady_clock> ts = std::chrono::steady_clock::now();
    uint64_t timestamp = std::chrono::duration_cast<std::chrono::microseconds>(ts.time_since_epoch()).count();
    const std::lock_guard<std::mutex> lock(touch.finger_mutex);
    drop_lifted_fingers(touch, timestamp, false);
    const uint64_t seq = touch.sample_seq.load(std::memory_order_relaxed) + 1;
    for (int i = 0; i < touch.finger_count; i++) {
        if (!touch.finger_first_seq[i])
            touch.finger_first_seq[i] = seq;
    }

    // disable mouse support on android because the touchscreen is considered as a mouse, and this creates a mess
#ifdef __ANDROID__
    constexpr bool on_android = true;
#else
    constexpr bool on_android = false;
#endif
    touch.tilt_active = false;
    if (touch.finger_count > 0 || touch.touchpad_finger_count > 0 || on_android) {
        const bool fingers_tilt = touch.touchscreen_tilt && !touch.is_touchpad;
        if (fingers_tilt) {
            for (int i = 0; i < touch.finger_count; i++) {
                if (touch.finger_lifted[i])
                    continue;
                set_tilt_target(touch,
                    (touch.finger_buffer[i].x * emuenv.display.viewport_drawable_w - emuenv.display.viewport_x) / emuenv.display.viewport_w,
                    (touch.finger_buffer[i].y * emuenv.display.viewport_drawable_h - emuenv.display.viewport_y) / emuenv.display.viewport_h);
                break;
            }
        }
        SceTouchData *buffers = touch.touch_buffers[(touch.touch_buffer_idx + 1) % MAX_TOUCH_BUFFER_SAVED];
        for (int port = 0; port < 2; port++) {
            buffers[port].status = 0;
            buffers[port].reportNum = 0;
            buffers[port].timeStamp = timestamp;
        }
        for (int port = 0; port < 2; port++) {
            if (fingers_tilt || (!touch.touchscreen_both && port != touch.touchscreen_port))
                continue;
            SceTouchData touch_data = touch.is_touchpad ? recover_touchpad_events(emuenv, port) : recover_touch_events(emuenv, port);
            touch_data.timeStamp = timestamp;
            buffers[port] = touch_data;
        }

    } else {
        const auto &ts = emuenv.touch;
        const bool allow_mouse_touch = ts.renderer_focused && !is_common_dialog_running(emuenv);

        float mouse_x = ts.mouse_x;
        float mouse_y = ts.mouse_y;
        bool left = ts.mouse_button_left;
        bool right = ts.mouse_button_right;
        left = left || touch.pinch_modifier_enabled;

        if ((ts.mouse_button_middle || ts.tilt_key_held) && allow_mouse_touch)
            set_tilt_target(touch, (mouse_x - emuenv.display.viewport_x) / emuenv.display.viewport_w,
                (mouse_y - emuenv.display.viewport_y) / emuenv.display.viewport_h);

        for (int port = 0; port < 2; port++) {
            // do it for both the front and the back touchscreen
            SceTouchData *data = &touch.touch_buffers[(touch.touch_buffer_idx + 1) % MAX_TOUCH_BUFFER_SAVED][port];
            memset(data, 0, sizeof(SceTouchData));
            data->timeStamp = timestamp;

            const bool pressed = (port == SCE_TOUCH_PORT_BACK) ? right : left;
            if (pressed && allow_mouse_touch) {
                if (!touch.is_touched[port]) {
                    touch.curr_touch_id[port]++;
                    // the touch id must be between 0 and 127
                    touch.curr_touch_id[port] %= 128;
                    touch.is_touched[port] = true;
                }

                const SceFVector2 touch_pos_viewport = {
                    (mouse_x - emuenv.display.viewport_x) / emuenv.display.viewport_w,
                    (mouse_y - emuenv.display.viewport_y) / emuenv.display.viewport_h
                };

                if ((touch_pos_viewport.x >= 0) && (touch_pos_viewport.y >= 0) && (touch_pos_viewport.x < 1) && (touch_pos_viewport.y < 1)) {
                    data->report[data->reportNum].x = static_cast<uint16_t>(touch_pos_viewport.x * 1920);
                    if (port == SCE_TOUCH_PORT_FRONT) {
                        data->report[data->reportNum].y = static_cast<uint16_t>(touch_pos_viewport.y * 1088);
                    } else {
                        data->report[data->reportNum].y = static_cast<uint16_t>(108 + touch_pos_viewport.y * 781);
                    }
                    data->report[data->reportNum].id = touch.curr_touch_id[port];
                    ++data->reportNum;

                    if (touch.pinch_modifier_enabled) {
                        if (touch.pinch_velocity != 0) {
                            pinch_move(touch, touch.pinch_velocity);
                        }

                        const SceInt16 baseTouchposX = data->report[data->reportNum - 1].x;

                        data->report[data->reportNum - 1].x = baseTouchposX - touch.pinch_dist;
                        data->report[data->reportNum].x = baseTouchposX + touch.pinch_dist;
                        data->report[data->reportNum].y = data->report[data->reportNum - 1].y;
                        data->report[data->reportNum].id = data->report[data->reportNum - 1].id + 1;
                        ++data->reportNum;

                        // QOL loop pinch when going too far, this is convenient when you want multiple consecutive pinches without releasing the modifier key
                        if (data->report[data->reportNum - 1].x < baseTouchposX || data->report[data->reportNum - 2].x < 0) {
                            reset_pinch(touch);
                        }
                    }
                }

                if (!emuenv.touch.touch_mode[port]) {
                    data->reportNum = 0;
                }
            } else {
                touch.is_touched[port] = false;
            }
        }
    }

    // Touch mapping points are merged into the buffer of whichever source filled it, so they work
    // with the screen, a pad's touchpad or the mouse alike.
    const bool mapped_tilt = touch.touchscreen_tilt && !touch.is_touchpad;
    if (!mapped_tilt) {
        int active_points = 0;
        for (const auto &point : touch.mapped_touch)
            active_points += point.active ? 1 : 0;
        if (active_points > 0) {
            LOG_INFO_ONCE("[TOUCH] on-screen touch mapping is active");
            SceTouchData *buffers = touch.touch_buffers[(touch.touch_buffer_idx + 1) % MAX_TOUCH_BUFFER_SAVED];
            for (int port = 0; port < 2; port++) {
                if (!touch.touchscreen_both && port != touch.touchscreen_port)
                    continue;
                SceTouchData &data = buffers[port];
                for (const auto &point : touch.mapped_touch) {
                    if (!point.active || data.reportNum >= SCE_TOUCH_MAX_REPORT)
                        continue;
                    // the point is over the letterbox rather than the panel: nothing to report
                    const float x = (point.x * emuenv.display.viewport_drawable_w - emuenv.display.viewport_x) / emuenv.display.viewport_w;
                    const float y = (point.y * emuenv.display.viewport_drawable_h - emuenv.display.viewport_y) / emuenv.display.viewport_h;
                    if (!(x >= 0.f) || !(y >= 0.f) || x >= 1.f || y >= 1.f)
                        continue;
                    SceTouchReport &report = data.report[data.reportNum];
                    report.id = point.id;
                    report.force = touch.force_touch_enabled[port] ? 128 : 0;
                    report.x = static_cast<uint16_t>(x * 1920.f);
                    report.y = static_cast<uint16_t>((port == SCE_TOUCH_PORT_FRONT) ? (y * 1088.f) : (108.f + y * 781.f));
                    data.reportNum++;
                }
            }
        }
    }

    touch.touch_buffer_idx++;
    touch.touch_buffer_idx %= MAX_TOUCH_BUFFER_SAVED;
    touch.sample_seq.store(seq, std::memory_order_release);
}

int set_mapped_touch(TouchState &state, const int slot, const float x, const float y, const bool active) {
    if (slot < 0 || slot >= MAX_MAPPED_TOUCH)
        return -1;

    const std::lock_guard<std::mutex> lock(state.finger_mutex);
    MappedTouch &point = state.mapped_touch[slot];
    if (active) {
        if (!std::isfinite(x) || !std::isfinite(y))
            return -1;
        point.x = std::clamp(x, 0.f, 1.f);
        point.y = std::clamp(y, 0.f, 1.f);
        if (!point.active) {
            point.id = static_cast<uint8_t>(state.next_touch_id);
            state.next_touch_id = (state.next_touch_id + 1) % 128;
        }
    }

    point.active = active;
    return 0;
}

void clear_mapped_touch(TouchState &state) {
    const std::lock_guard<std::mutex> lock(state.finger_mutex);
    for (auto &point : state.mapped_touch)
        point.active = false;
}

void pinch_modifier(TouchState &state, bool isHold) {
    state.pinch_modifier_enabled = isHold;

    // reset pinch state when modifier key is released
    if (!isHold) {
        reset_pinch(state);
    }
}

void pinch_move(TouchState &state, float velocity) {
    state.pinch_dist += velocity * state.pinch_speed;
}

void pinch_automove(TouchState &state, float velocity) {
    state.pinch_velocity = velocity;
}

int handle_touch_event(TouchState &state, SDL_TouchFingerEvent &finger) {
    const std::lock_guard<std::mutex> lock(state.finger_mutex);
    switch (finger.type) {
    case SDL_EVENT_FINGER_DOWN: {
        if (state.finger_count >= 8)
            drop_lifted_fingers(state, 0, true);
        if (state.finger_count >= 8)
            // best we can do is clean everything
            state.finger_count = 0;

        const int index = state.finger_count;
        state.finger_buffer[index] = finger;
        state.finger_buffer[index].touchID = state.next_touch_id;
        state.finger_first_seq[index] = 0;
        state.finger_down_us[index] = touch_now_us();
        state.finger_lifted[index] = false;
        state.next_touch_id = (state.next_touch_id + 1) % 128;
        state.finger_count++;
        break;
    }
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED: {
        const int index = find_down_finger(state, finger.fingerID);
        if (index < 0)
            break;
        if (finger_read_by_game(state, index))
            remove_finger(state, index);
        else
            state.finger_lifted[index] = true;
        break;
    }

    case SDL_EVENT_FINGER_MOTION: {
        const int index = find_down_finger(state, finger.fingerID);
        if (index >= 0) {
            const SDL_TouchID touch_id = state.finger_buffer[index].touchID;
            state.finger_buffer[index] = finger;
            state.finger_buffer[index].touchID = touch_id;
        }
        break;
    }
    }

    return 0;
}

int handle_touchpad_event(TouchState &state, SDL_GamepadTouchpadEvent &touchpad) {
    const std::lock_guard<std::mutex> lock(state.finger_mutex);
    switch (touchpad.type) {
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
        if (state.touchpad_finger_count >= 8) // best we can do is clean everything
            state.touchpad_finger_count = 0;

        state.touchpad_buffer[state.touchpad_finger_count] = touchpad;
        state.touchpad_buffer[state.touchpad_finger_count].which = state.next_touch_id;
        state.next_touch_id = (state.next_touch_id + 1) % 128;
        state.touchpad_finger_count++;
        break;
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP: {
        int index = -1;
        for (int i = 0; i < state.touchpad_finger_count; i++) {
            if (touchpad.finger == state.touchpad_buffer[i].finger) {
                index = i;
                break;
            }
        }
        if (index < 0)
            break;
        for (int i = index; i + 1 < state.touchpad_finger_count; i++)
            state.touchpad_buffer[i] = state.touchpad_buffer[i + 1];
        state.touchpad_finger_count--;
        break;
    }
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
        for (int i = 0; i < state.touchpad_finger_count; i++) {
            if (touchpad.finger == state.touchpad_buffer[i].finger) {
                const auto touch_id = state.touchpad_buffer[i].which;
                state.touchpad_buffer[i] = touchpad;
                state.touchpad_buffer[i].which = touch_id;
            }
        }
        break;
    }

    state.is_touchpad = state.touchpad_finger_count > 0;

    return 0;
}

std::vector<SceFVector2> get_touchpad_fingers_pos(const TouchState &state, SceTouchPortType &port) {
    if (!state.is_touchpad)
        return {};

    std::vector<SceFVector2> touchpad_fingers_pos;
    touchpad_fingers_pos.reserve(state.touchpad_finger_count);
    for (int i = 0; i < state.touchpad_finger_count; i++) {
        touchpad_fingers_pos.push_back({ state.touchpad_buffer[i].x, state.touchpad_buffer[i].y });
    }

    port = state.touchscreen_port;

    return touchpad_fingers_pos;
}

int toggle_touchscreen(TouchState &state) {
    state.touchscreen_both = false;
    if (state.touchscreen_port == SCE_TOUCH_PORT_FRONT) {
        state.touchscreen_port = SCE_TOUCH_PORT_BACK;
    } else {
        state.touchscreen_port = SCE_TOUCH_PORT_FRONT;
    }
    return 0;
}

int touch_get(const SceUID thread_id, EmuEnvState &emuenv, const SceUInt32 &port, SceTouchData *pData, SceUInt32 count, bool is_peek) {
    emuenv.display.guest_input_reads.fetch_add(1, std::memory_order_relaxed);
    memset(pData, 0, sizeof(SceTouchData) * count);

    const bool input_blocked = emuenv.drop_inputs || emuenv.ctrl.input_intercepted();

    const int port_idx = static_cast<int>(port);

    int nb_returned_data = 1;
    if (is_peek) {
        if (emuenv.touch.touch_mode[port])
            nb_returned_data = count;
        else
            nb_returned_data = 0;
    } else {
        const uint64_t current_vcount = emuenv.display.vblank_count.load();
        if (current_vcount <= emuenv.touch.last_vcount[port_idx]) {
            // sceTouchRead is blocking, wait for the next vsync for the buffer to be updated
            auto thread = emuenv.kernel.get_thread(thread_id);

            wait_vblank(emuenv.display, emuenv.kernel, thread, emuenv.touch.last_vcount[port_idx] + 1, false);
        }
        uint64_t vblank_count = emuenv.display.vblank_count.load();
        nb_returned_data = std::min<int>(count, vblank_count - emuenv.touch.last_vcount[port_idx]);
        emuenv.touch.last_vcount[port_idx] = vblank_count;
        if ((count > 0) && (nb_returned_data < 1)) {
            LOG_WARN_ONCE("sceTouchRead port {} had no buffer to return; answering 1 empty one", port_idx);
            nb_returned_data = 1;
        }
    }

    if (input_blocked)
        return nb_returned_data;

    const uint64_t read_seq = emuenv.touch.sample_seq.load(std::memory_order_acquire);
    int corr_buffer_idx;
    if (is_peek) {
        corr_buffer_idx = emuenv.touch.touch_buffer_idx;
    } else {
        // give the oldest buffer first
        corr_buffer_idx = (emuenv.touch.touch_buffer_idx - nb_returned_data + 1 + MAX_TOUCH_BUFFER_SAVED) % MAX_TOUCH_BUFFER_SAVED;
    }
    for (int32_t i = 0; i < nb_returned_data; i++) {
        pData[i] = emuenv.touch.touch_buffers[corr_buffer_idx][port_idx];
        if (emuenv.touch.force_touch_enabled[port_idx]) {
            for (int32_t j = 0; j < pData[i].reportNum; j++) {
                pData[i].report[j].force = 128;
            }
        }
        // if peek, repeat the last buffer
        if (!is_peek) {
            corr_buffer_idx++;
            corr_buffer_idx %= MAX_TOUCH_BUFFER_SAVED;
        }
    }
    if (nb_returned_data > 0)
        emuenv.touch.last_read_seq[port_idx].store(read_seq, std::memory_order_release);

    return nb_returned_data;
}

void touch_set_force_mode(TouchState &state, int port, bool mode) {
    state.force_touch_enabled[port] = mode;
}
