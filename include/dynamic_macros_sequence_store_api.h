#pragma once

#include <stdint.h>
#include <stddef.h>

enum dynamic_macros_event_type {
    DYNAMIC_MACROS_EVENT_TYPE_TERMINAL = 0,
    DYNAMIC_MACROS_EVENT_TYPE_NORMAL = 1,
    DYNAMIC_MACROS_EVENT_TYPE_SKIP = 2,
};

struct dynamic_macros_event {
    uint32_t position : 8;
    uint32_t state : 1;
    enum dynamic_macros_event_type type : 2;
    uint32_t delay : 21;
};

struct dynamic_macros_sequence_store_api {
    int (*save_event)(uint8_t id, struct dynamic_macros_event event);
    int (*clear_macro)(uint8_t id);
    const struct dynamic_macros_event *(*get_sequence)(uint8_t id);
    void (*trim_macro)(uint8_t id, size_t head_count, size_t tail_count);
    size_t (*get_max_length)(void);
    void (*reset)(void);
};

const struct dynamic_macros_sequence_store_api *get_dynamic_macros_store(void);