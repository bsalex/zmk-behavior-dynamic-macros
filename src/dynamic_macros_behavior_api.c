#include "dynamic_macros_behavior_api.h"
#include "dynamic_macros_sequence_store_api.h"
#include <zmk/behavior.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <stdbool.h>

LOG_MODULE_REGISTER(zmk_behavior_dynamic_macros_behavior_api,
                    CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACROS_LOG_LEVEL);

enum dynamic_macros_behavior_status {
    DYNAMIC_MACROS_BEHAVIOR_STATUS_IDLE,
    DYNAMIC_MACROS_BEHAVIOR_STATUS_RECORDING,
    DYNAMIC_MACROS_BEHAVIOR_STATUS_PLAYING,
    DYNAMIC_MACROS_BEHAVIOR_STATUS_PLAYING_LOOP
};

struct dynamic_macros_behavior_state {
    int64_t last_event_time;
    enum dynamic_macros_behavior_status status;
    uint8_t active_macros_id;
};

static struct dynamic_macros_behavior_state state = {.status = DYNAMIC_MACROS_BEHAVIOR_STATUS_IDLE,
                                                     .active_macros_id = 0};
static struct k_work_delayable play_work;
static uint32_t play_index = 0;

static int start_record(uint8_t id) {
    if (state.status == DYNAMIC_MACROS_BEHAVIOR_STATUS_RECORDING) {
        // Stop current recording first (even if same ID, restart)
        state.status = DYNAMIC_MACROS_BEHAVIOR_STATUS_IDLE;
    }

    // Clear the macro we are about to record
    const struct dynamic_macros_sequence_store_api *store = get_dynamic_macros_store();
    store->clear_macro(id);

    state.status = DYNAMIC_MACROS_BEHAVIOR_STATUS_RECORDING;
    state.active_macros_id = id;
    state.last_event_time = k_uptime_get();
    LOG_DBG("Started recording macro %d", id);
    return 0;
}

static int stop_record(void) {
    if (state.status == DYNAMIC_MACROS_BEHAVIOR_STATUS_RECORDING) {
        state.status = DYNAMIC_MACROS_BEHAVIOR_STATUS_IDLE;

        uint8_t id = state.active_macros_id;
        const struct dynamic_macros_sequence_store_api *store = get_dynamic_macros_store();
        const struct dynamic_macros_event *events = store->get_sequence(id);

        if (events) {
            size_t len = 0;
            // Find actual length by scanning for TERMINAL
            // We use get_max_length to bound the scan
            size_t max_len = store->get_max_length();
            for (size_t i = 0; i < max_len; i++) {
                if (events[i].type == DYNAMIC_MACROS_EVENT_TYPE_TERMINAL) {
                    len = i;
                    break;
                }
            }
            // If full/no terminal found, len might be 0 or max.
            // If events[0] is terminal, len is 0.
            // If max_len reached, we treat it as max_len?
            if (len == 0 && events[0].type != DYNAMIC_MACROS_EVENT_TYPE_TERMINAL) {
                if (events[max_len - 1].type != DYNAMIC_MACROS_EVENT_TYPE_TERMINAL) {
                    len = max_len;
                }
            }

            if (len > 0) {
                size_t head_trim = 0;
                size_t tail_trim = 0;

                // 1. Count leading released events (unpressed keys from beginning)
                while (head_trim < len && events[head_trim].state == false) {
                    head_trim++;
                }

                // 2. Count trailing pressed events (unreleased keys from end)
                // Note: Use len - 1 - tail_trim for index
                while (tail_trim < (len - head_trim) && events[len - 1 - tail_trim].state == true) {
                    tail_trim++;
                }

                if (head_trim > 0 || tail_trim > 0) {
                    store->trim_macro(id, head_trim, tail_trim);
                    size_t new_len = len - head_trim - tail_trim; // purely informational for log
                    LOG_DBG("Trimmed macro %d: -%zu head, -%zu tail. New len: %zu", id, head_trim,
                            tail_trim, new_len);
                }
            }
        }

        LOG_DBG("Stopped recording macro %d", state.active_macros_id);
    }
    return 0;
}

static void play_next_event(struct k_work *work) {
    const struct dynamic_macros_sequence_store_api *store = get_dynamic_macros_store();
    const struct dynamic_macros_event *events = store->get_sequence(state.active_macros_id);

    if ((state.status != DYNAMIC_MACROS_BEHAVIOR_STATUS_PLAYING &&
         state.status != DYNAMIC_MACROS_BEHAVIOR_STATUS_PLAYING_LOOP) ||
        !events) {
        state.status = DYNAMIC_MACROS_BEHAVIOR_STATUS_IDLE;
        LOG_DBG("Stopped playing macro %d (invalid state/events)", state.active_macros_id);
        return;
    }

    // Loop until we find a valid event or hit terminal
    while (true) {
        struct dynamic_macros_event event = events[play_index];

        if (event.type == DYNAMIC_MACROS_EVENT_TYPE_TERMINAL) {
            if (state.status == DYNAMIC_MACROS_BEHAVIOR_STATUS_PLAYING_LOOP) {
                play_index = 0;
                continue;
            }
            state.status = DYNAMIC_MACROS_BEHAVIOR_STATUS_IDLE;
            LOG_DBG("Finished playing macro %d", state.active_macros_id);
            return;
        }

        if (event.type == DYNAMIC_MACROS_EVENT_TYPE_SKIP) {
            play_index++;
            continue;
        }

        // Found valid event
        LOG_DBG("Replaying event: pos %d, state %d", event.position, event.state);
        raise_zmk_position_state_changed((struct zmk_position_state_changed){
            .position = event.position,
            .state = event.state,
            .timestamp = k_uptime_get(),
            .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
        });

        play_index++;

        // Schedule next event
        size_t next_idx = play_index;
        size_t max_len = store->get_max_length();
        while (next_idx < max_len && events[next_idx].type == DYNAMIC_MACROS_EVENT_TYPE_SKIP) {
            next_idx++;
        }

        if (next_idx >= max_len || events[next_idx].type == DYNAMIC_MACROS_EVENT_TYPE_TERMINAL) {
            if (state.status == DYNAMIC_MACROS_BEHAVIOR_STATUS_PLAYING_LOOP) {
                // Wrap around
                size_t wrap_idx = 0;
                while (wrap_idx < max_len &&
                       events[wrap_idx].type == DYNAMIC_MACROS_EVENT_TYPE_SKIP) {
                    wrap_idx++;
                }

                if (wrap_idx >= max_len ||
                    events[wrap_idx].type == DYNAMIC_MACROS_EVENT_TYPE_TERMINAL) {
                    // Empty macro even after wrap (unlikely if we are playing)
                    state.status = DYNAMIC_MACROS_BEHAVIOR_STATUS_IDLE;
                    LOG_DBG("Finished playing macro %d (loop empty)", state.active_macros_id);
                    return;
                }

                // Calculate delay: Terminal delay + First event delay
                // Note: events[next_idx] is the TERMINAL event (or we overran, which shouldn't
                // happen with TERMINAL sentinel)
                uint32_t terminal_delay = 0;
                if (next_idx < max_len) {
                    terminal_delay = events[next_idx].delay;
                }

                uint32_t wait_ms = terminal_delay + events[wrap_idx].delay;

                LOG_DBG("Looping macro %d. Terminal delay %d, First delay %d. Waiting %d",
                        state.active_macros_id, terminal_delay, events[wrap_idx].delay, wait_ms);

                play_index = wrap_idx;
                k_work_schedule(&play_work, K_MSEC(wait_ms));
                break;
            }

            state.status = DYNAMIC_MACROS_BEHAVIOR_STATUS_IDLE;
            LOG_DBG("Finished playing macro %d", state.active_macros_id);
            return;
        }

        // Schedule next one
        uint32_t wait_ms = events[next_idx].delay;
        play_index = next_idx; // Next invocation will handle this event
        k_work_schedule(&play_work, K_MSEC(wait_ms));
        break;
    }
}

static int play_macro_common(uint8_t id, bool loop) {
    if (state.status == DYNAMIC_MACROS_BEHAVIOR_STATUS_RECORDING ||
        state.status == DYNAMIC_MACROS_BEHAVIOR_STATUS_PLAYING ||
        state.status == DYNAMIC_MACROS_BEHAVIOR_STATUS_PLAYING_LOOP) {
        state.status = DYNAMIC_MACROS_BEHAVIOR_STATUS_IDLE;
        k_work_cancel_delayable(&play_work);
    }

    const struct dynamic_macros_sequence_store_api *store = get_dynamic_macros_store();
    const struct dynamic_macros_event *events = store->get_sequence(id);

    if (!events || events[0].type == DYNAMIC_MACROS_EVENT_TYPE_TERMINAL) {
        LOG_DBG("Macro %d is empty", id);
        return -1;
    }

    state.status =
        loop ? DYNAMIC_MACROS_BEHAVIOR_STATUS_PLAYING_LOOP : DYNAMIC_MACROS_BEHAVIOR_STATUS_PLAYING;
    state.active_macros_id = id;
    play_index = 0;

    k_work_init_delayable(&play_work, play_next_event);

    // Schedule first event immediately
    k_work_schedule(&play_work, K_NO_WAIT);

    return 0;
}

static int play_macro(uint8_t id) { return play_macro_common(id, false); }

static int play_macro_loop(uint8_t id) { return play_macro_common(id, true); }

static int recording_event_handler(const zmk_event_t *event) {
    if (state.status != DYNAMIC_MACROS_BEHAVIOR_STATUS_RECORDING) {
        return ZMK_BEHAVIOR_OPAQUE;
    }

    struct zmk_position_state_changed *pos_event = as_zmk_position_state_changed(event);
    if (!pos_event) {
        return ZMK_BEHAVIOR_OPAQUE;
    }

    int64_t current_time = k_uptime_get();
    uint32_t delay = (uint32_t)(current_time - state.last_event_time);

    state.last_event_time = current_time;

    struct dynamic_macros_event dm_event = {
        .position = pos_event->position,
        .state = pos_event->state,
        .type = DYNAMIC_MACROS_EVENT_TYPE_NORMAL,
        .delay = delay,
    };

    LOG_DBG("Recorded event: pos %d, state %d, delay %d", dm_event.position, dm_event.state,
            dm_event.delay);

    const struct dynamic_macros_sequence_store_api *store = get_dynamic_macros_store();
    int ret = store->save_event(state.active_macros_id, dm_event);

    if (ret == -ENOMEM) {
        LOG_WRN("Macro %d full, stopping recording", state.active_macros_id);
        // Stop recording immediately.
        // We should call stop_record() to properly trim and finish.
        // But stop_record needs to be called from a safe context?
        // recording_event_handler runs in event listener context.
        // Calling stop_record modifes state.
        // Let's call standard stop mechanism.
        stop_record();
        return ZMK_BEHAVIOR_OPAQUE;
    }

    // Optional: Check max length explicitly if store didn't return ENOMEM but we want to be safe?
    // store->save_event handles ENOMEM if full. So checking return value is sufficient.

    return ZMK_BEHAVIOR_OPAQUE;
}

ZMK_LISTENER(zmk_dynamic_macros_recorder, recording_event_handler);
ZMK_SUBSCRIPTION(zmk_dynamic_macros_recorder, zmk_position_state_changed);

static int stop_play(void) {
    if (state.status == DYNAMIC_MACROS_BEHAVIOR_STATUS_PLAYING ||
        state.status == DYNAMIC_MACROS_BEHAVIOR_STATUS_PLAYING_LOOP) {
        state.status = DYNAMIC_MACROS_BEHAVIOR_STATUS_IDLE;
        k_work_cancel_delayable(&play_work);
        LOG_DBG("Stopped playing macro %d", state.active_macros_id);
    }
    return 0;
}

static void reset(void) {
    stop_record();
    stop_play();

    const struct dynamic_macros_sequence_store_api *store = get_dynamic_macros_store();
    store->reset();

    LOG_DBG("Reset all macros");
}

static const struct dynamic_macros_behavior_api logic_api = {
    .record = start_record,
    .stop_record = stop_record,
    .play = play_macro,
    .play_loop = play_macro_loop,
    .stop_play = stop_play,
    .reset = reset,
};

const struct dynamic_macros_behavior_api *get_dynamic_macros_logic(void) { return &logic_api; }