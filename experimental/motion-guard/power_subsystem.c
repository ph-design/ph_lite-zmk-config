/* SPDX-License-Identifier: MIT */
#include <zephyr/kernel.h>
#include <zmk/power.h>
#include <zmk/studio/rpc.h>
/* Same wire subsystem, but no IMU handlers or capabilities on sensorless boards. */
ZMK_RPC_SUBSYSTEM(motion)
#define RESPONSE(type, ...) ZMK_RPC_RESPONSE(motion, type, __VA_ARGS__)
zmk_studio_Response get_capabilities(const zmk_studio_Request *req) {
    ARG_UNUSED(req);
    zmk_motion_Capabilities c = zmk_motion_Capabilities_init_zero;
    c.power_version = 1;
    c.power_lights = (IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW) ? 1 : 0) | (IS_ENABLED(CONFIG_ZMK_BACKLIGHT) ? 2 : 0);
    return RESPONSE(capabilities, c);
}
zmk_studio_Response get_power_config(const zmk_studio_Request *req) {
    ARG_UNUSED(req);
    struct zmk_power_config c; zmk_power_get_config(&c);
    zmk_motion_PowerConfig out = {.idle_timeout_ms=c.idle_timeout_ms, .rgb_off_ms=c.rgb_off_ms,
        .backlight_off_ms=c.backlight_off_ms, .deep_sleep_ms=c.deep_sleep_ms,
        .on_usb=c.on_usb, .off_when_locked=c.off_when_locked};
    return RESPONSE(power_config, out);
}
zmk_studio_Response set_power_config(const zmk_studio_Request *req) {
    const zmk_motion_PowerConfig *in=&req->subsystem.motion.request_type.set_power_config;
    struct zmk_power_config c={in->idle_timeout_ms,in->rgb_off_ms,in->backlight_off_ms,
        in->deep_sleep_ms,in->on_usb,in->off_when_locked};
    return RESPONSE(set_power_config, zmk_power_set_config(&c)==0);
}
zmk_studio_Response save_state(const zmk_studio_Request *req) {
    ARG_UNUSED(req); return RESPONSE(save_state,zmk_power_save_config()==0);
}
ZMK_RPC_SUBSYSTEM_HANDLER(motion,get_capabilities,ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(motion,get_power_config,ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(motion,set_power_config,ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(motion,save_state,ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_SETTINGS_RESET(motion,zmk_power_reset_config);
