#pragma once

#include <stdint.h>

struct dynamic_macros_behavior_api {
    int (*record)(uint8_t id);
    int (*stop_record)(void);
    int (*play)(uint8_t id);
    int (*play_loop)(uint8_t id);
    int (*stop_play)(void);
    void (*reset)(void);
};

const struct dynamic_macros_behavior_api *get_dynamic_macros_logic(void);