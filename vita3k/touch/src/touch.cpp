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

// A rear point handed to L2/R2 as a stroke instead of a tap: it travels the height of the back
// panel in this many microseconds, which is roughly the pace of a finger dragged across it. The
// ends are pulled in from 0 and 1 so the stroke never sits on the panel border, where games tend
// to clip the report away.
constexpr uint64_t REAR_SWIPE_US = 180000;
constexpr float REAR_SWIPE_FROM_Y = 0.10f;
constexpr float REAR_SWIPE_TO_Y = 0.90f;

// A mapping point told to run a gesture instead of holding still travels this much of the panel and
// takes this long. A swipe is a single pass ending at the far side; a rub swings between the two
// ends a whole number of times, so it finishes back where it started, which leaves the finger on
// the spot the game last saw it at instead of drifting a little further across the screen on every
// press. Both ends are measured around wherever the player put the point.
constexpr uint64_t SWIPE_US = 200000;
constexpr float SWIPE_SPAN = 0.30f;
constexpr uint64_t RUB_US = 700000;
constexpr float RUB_SPAN = 0.36f;
constexpr float RUB_PASSES = 2.5f;
constexpr float TWO_PI = 6.28318530718f;

// How long the finger of a click stays down. Short enough to read as a tap rather than a hold,
// long enough that a game polling once per frame always sees it; the finger lifts on its own
// afterwards instead of waiting for the button, which is what a menu entry wants.
constexpr uint64_t CLICK_US = 90000;

// A diagonal moves the same share of the panel across and down, which is what reads as a diagonal
// on a panel wider than it is tall; the share is trimmed so both ends together still cover about
// the same distance as the straight swipe above, instead of the longer run two full offsets would
// cover. Games that ask for one are reading the heading of the finger, so that it is clearly
// neither horizontal nor vertical matters more than the exact angle.
constexpr float DIAGONAL_SHARE = 0.70710678f;

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
        const uint64_t mapped_now_us = touch_now_us();
        int active_points = 0;
        for (const auto &point : touch.mapped_touch)
            active_points += point.held() ? 1 : 0;
        if (active_points > 0) {
            LOG_INFO_ONCE("[TOUCH] on-screen touch mapping is active");
            SceTouchData *buffers = touch.touch_buffers[(touch.touch_buffer_idx + 1) % MAX_TOUCH_BUFFER_SAVED];
            for (int port = 0; port < 2; port++) {
                SceTouchData &data = buffers[port];
                for (int slot = 0; slot < MAX_MAPPED_TOUCH; slot++) {
                    MappedTouch &point = touch.mapped_touch[slot];
                    if (!point.held() || point.click_done || data.reportNum >= SCE_TOUCH_MAX_REPORT)
                        continue;
                    // A rear point reports on the back panel by default: it is the pad's triggers that
                    // hold it, and driving it from the front would put a finger on the screen where
                    // the ring is not even drawn. Every other point follows the front/back switch.
                    // With the front-panel switch on, everything reports on the screen instead, which
                    // is the only panel a game like Uncharted ever reads.
                    const bool wants_port = touch.pad_touch_front
                        ? (port == SCE_TOUCH_PORT_FRONT)
                        : ((slot >= MAPPED_TOUCH_REAR_FIRST)
                                  ? (port == SCE_TOUCH_PORT_BACK)
                                  : (touch.touchscreen_both || port == touch.touchscreen_port));
                    if (!wants_port)
                        continue;
                    // the point is over the letterbox rather than the panel: nothing to report
                    // A stroke moves the finger between its two ends as time runs; a point held as a
                    // tap has no stroke and stays where it was put. A start time ahead of the clock
                    // would put the stroke somewhere absurd, so anything in the future counts as
                    // "not started yet" rather than wrapping round.
                    float point_x = point.x;
                    float point_y = point.y;
                    if (point.stroke_started) {
                        const uint64_t stroke_elapsed = (mapped_now_us > point.stroke_start_us)
                            ? mapped_now_us - point.stroke_start_us
                            : 0;
                        // A click is over as soon as its short stay runs out: the finger lifts by
                        // itself and the button being held down does not keep it on the panel, so
                        // holding produces one tap rather than one long press.
                        if (point.stroke_gesture == TOUCH_GESTURE_CLICK && stroke_elapsed >= point.stroke_duration_us) {
                            point.click_done = true;
                            continue;
                        }
                        const float stroke_t = (point.stroke_duration_us == 0)
                            ? 1.f
                            : std::min(1.f, static_cast<float>(stroke_elapsed) / static_cast<float>(point.stroke_duration_us));
                        // A rub swings out and comes back, so it ends where it began; a swipe is a
                        // plain ramp that stays at the far end until the button is let go.
                        const bool rubbing = point.stroke_gesture == TOUCH_GESTURE_RUB_H || point.stroke_gesture == TOUCH_GESTURE_RUB_V;
                        const float travel = rubbing
                            ? (0.5f - 0.5f * std::cos(stroke_t * TWO_PI * RUB_PASSES))
                            : stroke_t;
                        point_x = point.stroke_from_x + (point.stroke_to_x - point.stroke_from_x) * travel;
                        point_y = point.stroke_from_y + (point.stroke_to_y - point.stroke_from_y) * travel;
                    }
                    const float x = (point_x * emuenv.display.viewport_drawable_w - emuenv.display.viewport_x) / emuenv.display.viewport_w;
                    const float y = (point_y * emuenv.display.viewport_drawable_h - emuenv.display.viewport_y) / emuenv.display.viewport_h;
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

// Hands a fresh touch id to a point that goes from fully released to held, whichever source pressed
// it, so a finger and a pad button holding the same point do not fight over the id.
static void mapped_touch_down(TouchState &state, MappedTouch &point) {
    if (point.held())
        return;

    point.id = static_cast<uint8_t>(state.next_touch_id);
    state.next_touch_id = (state.next_touch_id + 1) % 128;
}

// Keeps both ends of a stroke on the panel. A stroke that ran off the edge would take the finger
// off the panel halfway through, and a finger leaving the panel reads as a lift, which ends the
// gesture before the game ever sees where it was heading. Both ends shift together, so the stroke
// keeps the length the gesture asked for.
static void fit_stroke_on_panel(float &from, float &to) {
    constexpr float margin = 0.05f;
    const float low = std::min(from, to);
    const float high = std::max(from, to);
    float shift = 0.f;
    if (low < margin && high <= 1.f - margin)
        shift = margin - low;
    else if (high > 1.f - margin && low >= margin)
        shift = (1.f - margin) - high;
    from += shift;
    to += shift;
}

// Works out the two ends of the stroke this press should run. The player's choice for the point
// wins; with none set, the rear panel switch can still ask for the stroke down the back of the
// panel that earlier builds had, so that switch keeps behaving exactly as it did.
static void begin_stroke(MappedTouch &point, const bool rear_swipe) {
    uint8_t gesture = point.gesture;
    if (gesture == TOUCH_GESTURE_NONE && rear_swipe)
        gesture = TOUCH_GESTURE_DOWN;

    point.stroke_gesture = gesture;
    point.stroke_started = false;
    point.stroke_release = false;
    if (gesture == TOUCH_GESTURE_NONE)
        return;

    const float half = SWIPE_SPAN * 0.5f;
    const float diag = half * DIAGONAL_SHARE;
    const float rub_half = RUB_SPAN * 0.5f;
    float from_x = point.x;
    float from_y = point.y;
    float to_x = point.x;
    float to_y = point.y;
    uint64_t duration = SWIPE_US;

    switch (gesture) {
    case TOUCH_GESTURE_UP:
        from_y = point.y + half;
        to_y = point.y - half;
        break;
    case TOUCH_GESTURE_DOWN:
        // the rear panel switch has always run the full height of the panel, and games were tuned
        // against that, so it keeps its own span; a gesture runs the shorter one around the point
        if (rear_swipe && point.gesture == TOUCH_GESTURE_NONE) {
            from_y = REAR_SWIPE_FROM_Y;
            to_y = REAR_SWIPE_TO_Y;
            duration = REAR_SWIPE_US;
        } else {
            from_y = point.y - half;
            to_y = point.y + half;
        }
        break;
    case TOUCH_GESTURE_LEFT:
        from_x = point.x + half;
        to_x = point.x - half;
        break;
    case TOUCH_GESTURE_RIGHT:
        from_x = point.x - half;
        to_x = point.x + half;
        break;
    case TOUCH_GESTURE_RUB_H:
        from_x = point.x - rub_half;
        to_x = point.x + rub_half;
        duration = RUB_US;
        break;
    case TOUCH_GESTURE_RUB_V:
        from_y = point.y - rub_half;
        to_y = point.y + rub_half;
        duration = RUB_US;
        break;
    case TOUCH_GESTURE_CLICK:
        // A click does not travel: the finger lands on the point and lifts again after a moment,
        // which is the whole of what an on-screen button wants.
        duration = CLICK_US;
        break;
    // The four corners, read as where the finger ends up. Each one starts on the opposite side of
    // the point so the point itself is crossed halfway through, and both axes move together so the
    // heading stays put for the whole travel instead of curving towards the corner at the end.
    case TOUCH_GESTURE_UP_LEFT:
        from_x = point.x + diag;
        from_y = point.y + diag;
        to_x = point.x - diag;
        to_y = point.y - diag;
        break;
    case TOUCH_GESTURE_UP_RIGHT:
        from_x = point.x - diag;
        from_y = point.y + diag;
        to_x = point.x + diag;
        to_y = point.y - diag;
        break;
    case TOUCH_GESTURE_DOWN_LEFT:
        from_x = point.x + diag;
        from_y = point.y - diag;
        to_x = point.x - diag;
        to_y = point.y + diag;
        break;
    case TOUCH_GESTURE_DOWN_RIGHT:
        from_x = point.x - diag;
        from_y = point.y - diag;
        to_x = point.x + diag;
        to_y = point.y + diag;
        break;
    default:
        return;
    }

    fit_stroke_on_panel(from_x, to_x);
    fit_stroke_on_panel(from_y, to_y);
    point.stroke_from_x = from_x;
    point.stroke_from_y = from_y;
    point.stroke_to_x = to_x;
    point.stroke_to_y = to_y;
    point.stroke_duration_us = duration;
    point.stroke_started = true;
    point.stroke_start_us = touch_now_us();
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
        point.has_position = true;
        mapped_touch_down(state, point);
    }

    point.active = active;
    return 0;
}

int set_mapped_touch_position(TouchState &state, const int slot, const float x, const float y) {
    if (slot < 0 || slot >= MAX_MAPPED_TOUCH)
        return -1;
    if (!std::isfinite(x) || !std::isfinite(y))
        return -1;

    const std::lock_guard<std::mutex> lock(state.finger_mutex);
    MappedTouch &point = state.mapped_touch[slot];
    point.x = std::clamp(x, 0.f, 1.f);
    point.y = std::clamp(y, 0.f, 1.f);
    point.has_position = true;
    return 0;
}

int set_mapped_touch_pad(TouchState &state, const int slot, const bool active) {
    if (slot < 0 || slot >= MAX_MAPPED_TOUCH)
        return -1;

    const std::lock_guard<std::mutex> lock(state.finger_mutex);
    MappedTouch &point = state.mapped_touch[slot];
    const bool rear_swipe = slot >= MAPPED_TOUCH_REAR_FIRST && state.pad_rear_touch_swipe;
    if (!active) {
        // A stroke that has not finished keeps the finger down for the rest of it. Lifting on
        // release would leave a stroke that travelled a couple of pixels and stopped, which is
        // exactly the thing a gesture-reading game ignores; this is also what makes a quick tap on
        // the button produce a complete stroke. The ctrl side polls every frame, so the release
        // lands on the first poll after the stroke ends.
        if (point.stroke_started && point.stroke_gesture != TOUCH_GESTURE_CLICK && touch_now_us() - point.stroke_start_us < point.stroke_duration_us) {
            point.stroke_release = true;
            return 0;
        }
        point.pad_active = false;
        point.stroke_started = false;
        point.stroke_release = false;
        // The button has to come up before the next click can land, which is what makes one press
        // one click instead of a stream of them.
        point.click_done = false;
        return 0;
    }

    // Pressed again while a release was still finishing its stroke: carry on from there instead of
    // restarting, so a player tapping in rhythm does not cut the stroke short every time. Clearing
    // the pending release is what makes the stroke keep its finger down while the button stays
    // pressed, instead of lifting the moment the stroke runs out and starting a second one.
    if (point.stroke_started) {
        point.stroke_release = false;
        return 0;
    }

    // Without a reported position the point has nowhere to go, and the overlay is the only thing
    // that knows where the player put it. It reports on every layout, but this guard has been the
    // reason a pad press could do nothing at all, so it gets a place to land instead of a refusal:
    // the middle of the screen, which is where the prompts of a front-touch game usually sit.
    if (!point.has_position) {
        point.x = 0.5f;
        point.y = 0.5f;
        LOG_WARN_ONCE("[TOUCH] pad touch mapping had no position; using the middle of the screen until the controls editor sends one");
    }
    const bool was_held = point.pad_active;
    mapped_touch_down(state, point);
    point.pad_active = true;
    point.click_done = false;
    begin_stroke(point, rear_swipe);
    if (!was_held) {
        if (point.stroke_started)
            LOG_INFO_ONCE("[TOUCH] pad button runs a touch gesture from a mapping point");
        else if (slot >= MAPPED_TOUCH_REAR_FIRST)
            LOG_INFO_ONCE("[TOUCH] pad L2/R2 press a rear touch mapping point");
        else
            LOG_INFO_ONCE("[TOUCH] pad L3/R3 press a mapping point of the touch panel");
    }
    return 0;
}

void set_mapped_touch_gesture(TouchState &state, const int slot, const int gesture) {
    if (slot < 0 || slot >= MAX_MAPPED_TOUCH)
        return;

    const std::lock_guard<std::mutex> lock(state.finger_mutex);
    MappedTouch &point = state.mapped_touch[slot];
    point.gesture = (gesture > TOUCH_GESTURE_NONE && gesture < TOUCH_GESTURE_COUNT)
        ? static_cast<uint8_t>(gesture)
        : TOUCH_GESTURE_NONE;
    // Changing the choice while the button is down must not leave a half-travelled finger behind:
    // the stroke in flight finishes, and the new choice is what the next press runs.
    if (!point.stroke_started)
        point.stroke_gesture = point.gesture;
}

void set_pad_touch_mapping(TouchState &state, const bool enabled) {
    const std::lock_guard<std::mutex> lock(state.finger_mutex);
    state.pad_touch_mapping = enabled;
    if (!enabled) {
        for (int slot = 0; slot < MAPPED_TOUCH_REAR_FIRST; slot++) {
            state.mapped_touch[slot].pad_active = false;
            state.mapped_touch[slot].stroke_started = false;
            state.mapped_touch[slot].stroke_release = false;
        }
    }
}

void set_pad_rear_touch_mapping(TouchState &state, const bool enabled) {
    const std::lock_guard<std::mutex> lock(state.finger_mutex);
    state.pad_rear_touch_mapping = enabled;
    if (!enabled) {
        for (int slot = MAPPED_TOUCH_REAR_FIRST; slot < MAX_MAPPED_TOUCH; slot++) {
            state.mapped_touch[slot].pad_active = false;
            state.mapped_touch[slot].stroke_started = false;
            state.mapped_touch[slot].stroke_release = false;
        }
    }
}

void set_pad_rear_touch_swipe(TouchState &state, const bool enabled) {
    const std::lock_guard<std::mutex> lock(state.finger_mutex);
    state.pad_rear_touch_swipe = enabled;
    if (!enabled) {
        // Switching back to taps mid-stroke: drop the stroke state, the next press starts a tap.
        for (int slot = MAPPED_TOUCH_REAR_FIRST; slot < MAX_MAPPED_TOUCH; slot++) {
            state.mapped_touch[slot].stroke_started = false;
            state.mapped_touch[slot].stroke_release = false;
        }
    }
}

void clear_mapped_touch(TouchState &state) {
    const std::lock_guard<std::mutex> lock(state.finger_mutex);
    for (auto &point : state.mapped_touch) {
        point.active = false;
        point.pad_active = false;
        point.stroke_started = false;
        point.stroke_release = false;
        point.click_done = false;
    }
}

void set_pad_touch_front(TouchState &state, const bool enabled) {
    const std::lock_guard<std::mutex> lock(state.finger_mutex);
    state.pad_touch_front = enabled;
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
