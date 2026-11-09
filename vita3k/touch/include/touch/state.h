#pragma once

#include <SDL3/SDL_events.h>

#include <touch/touch.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>

// A touch mapping point: an on-screen (or bindable) button whose press is reported to the guest as a
// finger held at a fixed place on the touch panel, so games that ask for a tap can be played with
// buttons instead of reaching for the screen.
static constexpr int MAX_MAPPED_TOUCH = 8;

// Points from this slot on report on the rear panel instead of the screen, whatever the front/back
// switch says, and the pad's L2/R2 are what hold them down. They exist for the games that put extra
// buttons on the back of the console: reaching for the screen is exactly what a pad player cannot
// do, so the triggers stand in for the fingers nobody has there.
static constexpr int MAPPED_TOUCH_REAR_FIRST = 4;

// What the finger does while a pad button holds a mapping point down. A tap reports one finger
// sitting still, which is all a game that uses the panel as a button ever asks for, and useless to
// the far larger set of games that read the panel as a gesture: those compare where the finger was
// with where it went, and a finger that never moves compares equal to itself forever.
//
// Everything here walks the finger along a stroke, and a stroke is just two ends plus how long it
// takes, so the plain tap stays the default and needs no special case: with NONE no stroke starts.
enum MappedTouchGesture : uint8_t {
    TOUCH_GESTURE_NONE = 0, // stay put, the behaviour every earlier build had
    TOUCH_GESTURE_UP,
    TOUCH_GESTURE_DOWN,
    TOUCH_GESTURE_LEFT,
    TOUCH_GESTURE_RIGHT,
    TOUCH_GESTURE_RUB_H, // back and forth across, for the wiping a game asks to be rubbed off
    TOUCH_GESTURE_RUB_V,
    // A real click: the finger goes down, stays for a moment and lifts again, all from one press of
    // the button. Holding the button down afterwards does not keep the finger there. This is what
    // a menu entry or an on-screen prompt wants, and it is the one thing a held-down tap cannot do.
    TOUCH_GESTURE_CLICK,
    // Corner to corner. Plenty of games ask for a finger dragged at an angle rather than straight
    // across, and a straight stroke is not a diagonal one however the player sets the point: the
    // game compares the direction of the travel, so the whole length of the stroke has to be at that
    // angle. Named after where the finger is heading, like the four straight ones above.
    TOUCH_GESTURE_UP_LEFT,
    TOUCH_GESTURE_UP_RIGHT,
    TOUCH_GESTURE_DOWN_LEFT,
    TOUCH_GESTURE_DOWN_RIGHT,
    TOUCH_GESTURE_COUNT
};

struct MappedTouch {
    bool active = false;
    bool pad_active = false; // held by a pad button rather than a finger on the overlay ring
    bool has_position = false; // the overlay told us where this point sits
    // The stroke a pad button runs while it holds this point down. Set by the player per point; the
    // rear panel switch below can still ask for a stroke of its own without touching the setting.
    uint8_t gesture = TOUCH_GESTURE_NONE;
    uint8_t stroke_gesture = TOUCH_GESTURE_NONE; // what this press is actually running
    bool stroke_started = false; // the trigger is running a stroke instead of holding a spot
    bool stroke_release = false; // released early; finish the stroke before lifting the finger
    uint64_t stroke_start_us = 0;
    uint64_t stroke_duration_us = 0;
    // The two ends of the stroke, worked out once when the button goes down: interpolating between
    // them is all the per-frame work there is, whichever curve the gesture uses.
    float stroke_from_x = 0.f;
    float stroke_from_y = 0.f;
    float stroke_to_x = 0.f;
    float stroke_to_y = 0.f;
    // The click already delivered its finger-up and must stay up until the button is released.
    bool click_done = false;
    float x = 0.f; // normalized over the drawable area, same space as SDL finger events
    float y = 0.f;
    uint8_t id = 0; // touch id handed out when the point goes down

    bool held() const { return active || pad_active; }
};

struct TouchState {
    SceTouchSamplingState touch_mode[2] = { SCE_TOUCH_SAMPLING_STATE_STOP, SCE_TOUCH_SAMPLING_STATE_STOP };

    float mouse_x = 0;
    float mouse_y = 0;
    bool mouse_button_left = false;
    bool mouse_button_right = false;
    bool mouse_button_middle = false;
    bool tilt_key_held = false;
    bool renderer_focused = false;
    SceTouchPortType touchscreen_port = SCE_TOUCH_PORT_FRONT;
    bool touchscreen_both = false; // screen touches feed both ports
    bool touchscreen_tilt = false;
    bool tilt_active = false;
    float tilt_x = 0.f;
    float tilt_y = 0.f;
    SceTouchData touch_buffers[MAX_TOUCH_BUFFER_SAVED][2] = {};
    int touch_buffer_idx = 0;
    bool is_touchpad = false;
    SDL_TouchFingerEvent finger_buffer[8] = {};
    uint64_t finger_first_seq[8] = {};
    uint64_t finger_down_us[8] = {};
    bool finger_lifted[8] = {};
    std::atomic<uint64_t> sample_seq{ 0 };
    std::atomic<uint64_t> last_read_seq[2]{};
    std::mutex finger_mutex;
    SDL_GamepadTouchpadEvent touchpad_buffer[8] = {};
    uint8_t finger_count = 0;
    uint8_t touchpad_finger_count = 0;
    bool is_touched[2] = { false, false };
    int curr_touch_id[2] = { 0, 0 };
    int next_touch_id = 1;
    uint64_t last_vcount[2] = { 0, 0 };
    bool force_touch_enabled[2] = { false, false };
    bool pinch_modifier_enabled = false;
    float pinch_velocity = 0.f;
    bool old_pinch_behavior = false;
    MappedTouch mapped_touch[MAX_MAPPED_TOUCH] = {};
    // When set, the pad's L3 and R3 fire mapping points 1 and 2. This is what makes a game that
    // asks for a tap playable on a DS4 alone, without reaching for the screen. Atomic because the
    // ctrl thread reads it on every input poll while the UI thread writes it from a setting.
    std::atomic<bool> pad_touch_mapping{ true };
    // When set, the pad's L2 and R2 hold the two rear points down, which is what makes a game that
    // puts extra buttons on the back of the console playable from the pad alone. Kept apart from the
    // switch above on purpose: a game can want one and not the other, and each is set per game.
    std::atomic<bool> pad_rear_touch_mapping{ true };
    // When set, a rear point held by L2/R2 is delivered as a stroke down the panel instead of a
    // finger parked on one spot. Games that use the back panel as a gesture ignore a stationary
    // touch, and a stationary touch is all a tap can produce; this is the switch that lets the
    // player pick whichever of the two the game in front of them actually understands.
    std::atomic<bool> pad_rear_touch_swipe{ false };
    // When set, every mapping point reports on the front panel, whatever its slot and whatever the
    // front/back switch says. A game that only reads the screen (Uncharted and most of the rest)
    // never looks at the back panel, so a rear point driving a front-touch action would be dead
    // without this. Off by default: the games that put buttons on the back panel need the old rule.
    std::atomic<bool> pad_touch_front{ false };
    static constexpr SceInt16 initial_pinch_dist = 300;
    SceInt16 pinch_dist = initial_pinch_dist;
    SceInt16 pinch_speed = 40;

    void reset_runtime() {
        const std::lock_guard<std::mutex> lock(finger_mutex);
        touch_mode[0] = SCE_TOUCH_SAMPLING_STATE_STOP;
        touch_mode[1] = SCE_TOUCH_SAMPLING_STATE_STOP;
        mouse_x = 0.f;
        mouse_y = 0.f;
        mouse_button_left = false;
        mouse_button_right = false;
        mouse_button_middle = false;
        tilt_key_held = false;
        renderer_focused = false;
        touchscreen_port = SCE_TOUCH_PORT_FRONT;
        touchscreen_both = false;
        touchscreen_tilt = false;
        tilt_active = false;
        tilt_x = 0.f;
        tilt_y = 0.f;
        std::fill_n(touch_buffers[0], MAX_TOUCH_BUFFER_SAVED * 2, SceTouchData{});
        touch_buffer_idx = 0;
        is_touchpad = false;
        std::fill_n(finger_buffer, 8, SDL_TouchFingerEvent{});
        std::fill_n(finger_first_seq, 8, uint64_t{ 0 });
        std::fill_n(finger_down_us, 8, uint64_t{ 0 });
        std::fill_n(finger_lifted, 8, false);
        sample_seq.store(0);
        last_read_seq[0].store(0);
        last_read_seq[1].store(0);
        std::fill_n(touchpad_buffer, 8, SDL_GamepadTouchpadEvent{});
        finger_count = 0;
        touchpad_finger_count = 0;
        is_touched[0] = false;
        is_touched[1] = false;
        curr_touch_id[0] = 0;
        curr_touch_id[1] = 0;
        next_touch_id = 1;
        last_vcount[0] = 0;
        last_vcount[1] = 0;
        force_touch_enabled[0] = false;
        force_touch_enabled[1] = false;
        pinch_modifier_enabled = false;
        pinch_velocity = 0.f;
        old_pinch_behavior = false;
        // What the player picked for each point outlives the restart; the rest of the point does not.
        for (auto &point : mapped_touch) {
            const uint8_t gesture = point.gesture;
            point = MappedTouch{};
            point.gesture = gesture;
        }
        pinch_dist = initial_pinch_dist;
        pinch_speed = 40;
    }
};
