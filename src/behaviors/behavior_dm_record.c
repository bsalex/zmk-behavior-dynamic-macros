#define DT_DRV_COMPAT zmk_behavior_dm_record

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <zmk/behavior.h>
#include "dynamic_macros_behavior_api.h"

LOG_MODULE_REGISTER(zmk_behavior_dm_record, CONFIG_ZMK_BEHAVIOR_DYNAMIC_MACROS_LOG_LEVEL);

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    const struct dynamic_macros_behavior_api *api = get_dynamic_macros_logic();
    uint8_t id = binding->param1;

    LOG_DBG("Record behavior pressed, macro %d", id);
    api->record(id);

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_dm_record_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_dm_record_driver_api);
