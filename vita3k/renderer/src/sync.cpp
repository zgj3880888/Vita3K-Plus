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

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <renderer/commands.h>
#include <renderer/driver_functions.h>
#include <renderer/state.h>
#include <renderer/types.h>

#include <display/state.h>
#include <renderer/gl/functions.h>
#include <renderer/gl/state.h>
#include <renderer/vulkan/functions.h>
#include <renderer/vulkan/state.h>
#include <renderer/vulkan/types.h>

#include <renderer/functions.h>
#include <util/tracy.h>

namespace renderer {
COMMAND(handle_nop) {
    TRACY_FUNC_COMMANDS(handle_nop);
    // Signal back to client
    int code_to_finish = helper.pop<int>();
    complete_command(renderer, helper, code_to_finish);
}

COMMAND(handle_signal_sync_object) {
    TRACY_FUNC_COMMANDS(handle_signal_sync_object);
    SceGxmSyncObject *sync = helper.pop<Ptr<SceGxmSyncObject>>().get(mem);
    const uint32_t timestamp = helper.pop<uint32_t>();

    if (features.enable_memory_mapping && config.current_config.high_accuracy) {
        assert(renderer.current_backend == renderer::Backend::Vulkan);
        vulkan::signal_sync_object(dynamic_cast<vulkan::VKState &>(renderer), sync, timestamp);
    } else {
        renderer::subject_done(sync, timestamp);
    }
}

COMMAND(handle_wait_sync_object) {
    TRACY_FUNC_COMMANDS(handle_wait_sync_object);
    SceGxmSyncObject *sync = helper.pop<Ptr<SceGxmSyncObject>>().get(mem);
    const uint32_t timestamp = helper.pop<uint32_t>();

    renderer::wishlist(sync, timestamp);
}

COMMAND(handle_notification) {
    TRACY_FUNC_COMMANDS(handle_notification);
    SceGxmNotification notif = helper.pop<SceGxmNotification>();

    {
        std::unique_lock<std::mutex> lock(renderer.notification_mutex);
        uint32_t *val = notif.address.get(mem);
        if (val) // Ratchet and clank Trilogy request this
            *val = notif.value;
    }
    renderer.notification_ready.notify_all();
    note_gpu_event();
}

COMMAND(handle_set_screen_filter) {
    TRACY_FUNC_COMMANDS(handle_set_screen_filter);
    std::unique_ptr<std::string> filter(helper.pop<std::string *>());

    switch (renderer.current_backend) {
    case Backend::OpenGL:
        dynamic_cast<gl::GLState &>(renderer).set_screen_filter(*filter);
        break;

    case Backend::Vulkan:
        dynamic_cast<vulkan::VKState &>(renderer).screen_renderer.set_filter(*filter);
        break;
    }
}

COMMAND(new_frame) {
    TRACY_FUNC_COMMANDS(new_frame);
    DisplayFrameInfo *next_frame = helper.pop<DisplayFrameInfo *>();
    DisplayState *display = helper.pop<DisplayState *>();

    if (next_frame) {
        // set the predicted frame as the next one to render
        std::lock_guard<std::mutex> guard(display->display_info_mutex);
        display->next_rendered_frame = *next_frame;
        delete next_frame;

        renderer.should_display = true;
    }

    if (renderer.current_backend == Backend::Vulkan) {
        renderer::Context *active_context = helper.pop<renderer::Context *>();
        if (active_context) {
            vulkan::new_frame(*reinterpret_cast<vulkan::VKContext *>(active_context));
        }
    }
}

// Client side function
void finish(State &state, Context *context) {
    // Add NOP then wait for it
    renderer::send_single_command(state, context, renderer::CommandOpcode::Nop, true, 1);

    // unblock game threads if shutting down
    if (state.render_abort.load(std::memory_order_relaxed))
        return;

    // Wait for the VK wait thread to finish processing all pending requests.
    // Push a callback request on the queue and wait for it to be treated
    if (state.current_backend == Backend::Vulkan && state.features.enable_memory_mapping) {
        auto &vk_state = static_cast<vulkan::VKState &>(state);
        auto promise = std::make_shared<std::promise<void>>();
        std::future<void> future = promise->get_future();
        vk_state.request_queue.push(vulkan::CallbackRequest{
            new vulkan::CallbackRequestFunction([promise]() { promise->set_value(); }), /* wait_for_gpu = */ true });

        while (future.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready) {
            if (state.render_abort.load(std::memory_order_relaxed) || vk_state.request_queue.is_aborted())
                return;
        }
    }
}

// A list abandoned around or after a wait began may carry the signal that the wait is blocked on
bool signal_may_be_lost(State &state, const int64_t wait_start_epoch_ms) {
    if (!recover_from_abandoned_lists)
        return false;
    // a signal this late is a wedge rather than a slow frame, and an abandon this old can still be ours
    constexpr int64_t MIN_WAIT_MS = 2000;
    constexpr int64_t ABANDON_WINDOW_MS = 60000;
    const int64_t abandoned = state.last_abandon_epoch_ms.load(std::memory_order_relaxed);
    if (abandoned == 0)
        return false;
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return now - wait_start_epoch_ms >= MIN_WAIT_MS && abandoned + ABANDON_WINDOW_MS >= wait_start_epoch_ms;
}

int wait_for_status(State &state, int *status, int signal, bool wake_on_equal) {
    std::unique_lock<std::mutex> lock(state.command_finish_one_mutex);
    const bool wake_on_unequal = !wake_on_equal;
    if ((*status == signal) ^ wake_on_unequal) {
        // Signaled, return
        return *status;
    }

    // unblock threads if shutting down
    const auto ready = [&]() {
        return state.render_abort.load(std::memory_order_relaxed)
            || ((*status == signal) ^ wake_on_unequal);
    };
    const int64_t wait_start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    while (!state.command_finish_one.wait_for(lock, std::chrono::milliseconds(250), ready)) {
        if (!signal_may_be_lost(state, wait_start_ms))
            continue;
        // the command that would complete this status was abandoned, so a stale status beats never returning
        static std::atomic<uint32_t> released{ 0 };
        const uint32_t n = released.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8 || (n & 1023) == 0)
            LOG_ERROR("[CMDLOST] a command list was abandoned while waiting for its completion status; releasing the waiter (#{})", n);
        break;
    }
    return *status;
}

SyncWaitResult wishlist(SceGxmSyncObject *sync_object, const uint32_t timestamp, const int32_t timeout_micros) {
    std::unique_lock<std::mutex> lock(sync_object->lock);
    if (sync_object->timestamp_current < timestamp) {
        const auto &pred = [&]() {
            return sync_object->being_deleted || sync_object->timestamp_current >= timestamp;
        };

        if (timeout_micros == -1) {
            sync_object->cond.wait(lock, pred);
        } else if (!sync_object->cond.wait_for(lock, std::chrono::microseconds(timeout_micros), pred)) {
            return SyncWaitResult::TimedOut;
        }
    }
    if (sync_object->being_deleted)
        return SyncWaitResult::Shutdown;
    return SyncWaitResult::Ready;
}

namespace {
std::mutex gpu_event_mutex;
std::condition_variable gpu_event_cond;
uint64_t gpu_events = 0;
} // namespace

void note_gpu_event() {
    {
        const std::lock_guard<std::mutex> lock(gpu_event_mutex);
        gpu_events++;
    }
    gpu_event_cond.notify_all();
}

uint64_t wait_gpu_event(const uint64_t seen, const uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(gpu_event_mutex);
    gpu_event_cond.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return gpu_events != seen; });
    return gpu_events;
}

void subject_done(SceGxmSyncObject *sync_object, const uint32_t timestamp) {
    assert(sync_object->timestamp_ahead >= timestamp);
    {
        std::unique_lock<std::mutex> lock(sync_object->lock);
        sync_object->timestamp_current = std::max(sync_object->timestamp_current.load(), timestamp);
    }
    // maybe notify_one is enough
    sync_object->cond.notify_all();
    note_gpu_event();
}

void submit_command_list(State &state, renderer::Context *context, CommandList &command_list) {
    command_list.context = context;
    // A full queue means the render thread stopped draining, and the guest thread that submits
    // is the one that appears hung afterwards, so time the hand-off.
    const auto submit_start = std::chrono::steady_clock::now();
    state.command_buffer_queue.push(std::move(command_list));
    const auto submit_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - submit_start).count();
    if (submit_ms >= 500) {
        static std::atomic<int64_t> last_log_ms{ 0 };
        const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        int64_t prev = last_log_ms.load(std::memory_order_relaxed);
        if (now_ms - prev >= 1'000 && last_log_ms.compare_exchange_strong(prev, now_ms))
            LOG_ERROR("[CMDQ] submit_command_list blocked for {} ms (queue depth {} of {}): the render thread is not draining command lists", submit_ms, state.command_buffer_queue.size(), state.command_buffer_queue.maxPendingCount_);
    }
}
} // namespace renderer
