#include <errno.h>
#include "dynamic_macros_sequence_store_api.h"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(zmk_behavior_dynamic_macros_sequence_store_api,
                    CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACROS_LOG_LEVEL);

#define MAX_EVENTS CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACROS_MAX_LENGTH
#define MACRO_COUNT CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACROS_NUMBER

static struct dynamic_macros_event buffers[MACRO_COUNT][MAX_EVENTS];

static int save_event(uint8_t id, struct dynamic_macros_event event) {
    if (id >= MACRO_COUNT) {
        LOG_ERR("Invalid macro ID %d", id);
        return -EINVAL;
    }

    struct dynamic_macros_event *events = buffers[id];

    // Check for buffer availability

    for (int i = 0; i < MAX_EVENTS - 1; i++) {
        if (events[i].type == DYNAMIC_MACROS_EVENT_TYPE_TERMINAL) {
            events[i] = event;
            events[i + 1].type = DYNAMIC_MACROS_EVENT_TYPE_TERMINAL; // New terminal
            return 0;
        }
    }

    LOG_WRN("Macro %d buffer full", id);
    return -ENOMEM;
}

static int clear_macro(uint8_t id) {
    if (id >= MACRO_COUNT)
        return -EINVAL;
    buffers[id][0].type = DYNAMIC_MACROS_EVENT_TYPE_TERMINAL;
    return 0;
}

static void reset() {
    for (int i = 0; i < MACRO_COUNT; i++) {
        clear_macro(i);
    }
}

static const struct dynamic_macros_event *get_sequence(uint8_t id) {
    return (id < MACRO_COUNT) ? buffers[id] : NULL;
}

static void trim_macro(uint8_t id, size_t head_count, size_t tail_count) {
    if (id >= MACRO_COUNT) {
        return;
    }

    struct dynamic_macros_event *events = buffers[id];
    size_t len = 0;

    // Find length, stop at TERMINAL or MAX
    for (size_t i = 0; i < MAX_EVENTS; i++) {
        if (events[i].type == DYNAMIC_MACROS_EVENT_TYPE_TERMINAL) {
            len = i;
            break;
        }
    }

    // Safety check
    if (len == 0 && events[0].type != DYNAMIC_MACROS_EVENT_TYPE_TERMINAL) {
        if (events[MAX_EVENTS - 1].type != DYNAMIC_MACROS_EVENT_TYPE_TERMINAL) {
            // Maybe full buffer.
            len = MAX_EVENTS;
        }
    }

    // Mark head
    for (size_t i = 0; i < len && i < head_count; i++) {
        events[i].type = DYNAMIC_MACROS_EVENT_TYPE_SKIP;
    }

    // Mark tail
    // Iterate backwards from end
    size_t trimmed_tail = 0;
    if (len > head_count) { // Only trim tail if we haven't trimmed everything already
        for (size_t i = 0; i < len; i++) {
            size_t idx = len - 1 - i;
            if (trimmed_tail < tail_count) {
                if (events[idx].type != DYNAMIC_MACROS_EVENT_TYPE_SKIP) {
                    events[idx].type = DYNAMIC_MACROS_EVENT_TYPE_SKIP;
                    trimmed_tail++;
                }
            } else {
                break;
            }
        }
    }
}

static size_t get_max_length(void) { return MAX_EVENTS; }

static const struct dynamic_macros_sequence_store_api ram_api = {.save_event = save_event,
                                                                 .clear_macro = clear_macro,
                                                                 .get_sequence = get_sequence,
                                                                 .trim_macro = trim_macro,
                                                                 .get_max_length = get_max_length,
                                                                 .reset = reset};

const struct dynamic_macros_sequence_store_api *get_dynamic_macros_store(void) { return &ram_api; }