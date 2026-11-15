"""Bounded Studio telemetry, transport ownership, USB UF2 entry and RGB fades."""
from pathlib import Path
import sys

r, messages = map(Path, sys.argv[1:3])
def edit(path, old, new):
    p=r/path
    s=p.read_text()
    if old not in s: raise SystemExit(f"Missing runtime patch anchor: {path}: {old[:70]}")
    p.write_bytes(s.replace(old,new,1).encode())

# The base radio overlay provides whole-frame bounded writes. Only telemetry
# remains best effort here; other notifications use the independent thread below.
edit("app/src/studio/rpc.c", "bool notification = resp->which_type == zmk_studio_Response_notification_tag;", "bool notification = resp->which_type == zmk_studio_Response_notification_tag &&\n        resp->type.notification.which_subsystem == zmk_studio_Notification_motion_tag;")

# Small reliable notifications are serialized on their own thread; their event
# producers (unlock/key paths) never wait on the transport mutex or buffer.
edit("app/src/studio/rpc.c", "static int studio_rpc_listener_cb", """K_MSGQ_DEFINE(rpc_notifications, sizeof(zmk_studio_Notification), 8, 4);
static void notification_main(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    for (;;) {
        zmk_studio_Response resp = zmk_studio_Response_init_zero;
        resp.which_type = zmk_studio_Response_notification_tag;
        k_msgq_get(&rpc_notifications, &resp.type.notification, K_FOREVER);
        send_response(&resp);
    }
}
K_THREAD_DEFINE(studio_notify_thread, 2048, notification_main, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
static int studio_rpc_listener_cb""")
edit("app/src/studio/rpc.c", "        send_response(&resp);\n        return ZMK_EV_EVENT_BUBBLE;", """        if (resp.type.notification.which_subsystem == zmk_studio_Notification_motion_tag)
            send_response(&resp);
        else
            k_msgq_put(&rpc_notifications, &resp.type.notification, K_NO_WAIT);
        return ZMK_EV_EVENT_BUBBLE;""")

# UART owns the USB stream whenever USB HID is ready, independently of the
# preferred wireless HID endpoint. The radio worker must not drain USB replies.
edit("app/src/studio/rpc.c", '#include <zmk/events/endpoint_changed.h>', '#include <zmk/events/endpoint_changed.h>\n#include <zmk/events/usb_conn_state_changed.h>\n#include <zmk/usb.h>')
edit("app/src/studio/rpc.c", 'enum zmk_transport transport = zmk_endpoints_selected().transport;', 'enum zmk_transport transport = zmk_usb_is_hid_ready() ? ZMK_TRANSPORT_USB : zmk_endpoints_selected().transport;')
edit("app/src/studio/rpc.c", 'if (ep_changed) {', 'if (ep_changed || as_zmk_usb_conn_state_changed(eh)) {')
edit("app/src/studio/rpc.c", 'ZMK_SUBSCRIPTION(studio_rpc, zmk_endpoint_changed);', 'ZMK_SUBSCRIPTION(studio_rpc, zmk_endpoint_changed);\nZMK_SUBSCRIPTION(studio_rpc, zmk_usb_conn_state_changed);')

# Additive secured core RPC, available on keyboards with or without IMU.
p=messages/"proto/zmk/core.proto";s=p.read_text().replace('bool reset_settings = 4;', 'bool reset_settings = 4;\n        bool enter_bootloader = 5;').replace('bytes serial_number = 2;', 'bytes serial_number = 2;\n    uint32 bootloader_version = 3;');p.write_bytes(s.encode())
edit("app/src/studio/core_subsystem.c", '#include <pb_encode.h>', '#include <pb_encode.h>\n#include <zephyr/sys/reboot.h>\n#include <zmk/usb.h>\n#include <dt-bindings/zmk/reset.h>\n#if defined(CONFIG_SOC_SERIES_NRF52X)\n#include <hal/nrf_power.h>\n#include <cmsis_core.h>\n#endif')
edit("app/src/studio/core_subsystem.c", 'resp.name.funcs.encode = encode_device_info_name;', 'resp.bootloader_version = 1;\n    resp.name.funcs.encode = encode_device_info_name;')
edit("app/src/studio/core_subsystem.c", 'ZMK_RPC_SUBSYSTEM_HANDLER(core, get_device_info,', '''static void bootloader_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
#if defined(CONFIG_SOC_SERIES_NRF52X)
    NRF_POWER->GPREGRET = RST_UF2;
    __DSB();
    NVIC_SystemReset();
#else
    sys_reboot(RST_UF2);
#endif
}
static K_WORK_DELAYABLE_DEFINE(bootloader_work, bootloader_work_cb);
zmk_studio_Response enter_bootloader(const zmk_studio_Request *req) {
    bool ok = req->subsystem.core.request_type.enter_bootloader && zmk_usb_is_hid_ready();
    if (ok) k_work_reschedule(&bootloader_work, K_MSEC(300));
    return CORE_RESPONSE(enter_bootloader, ok);
}
ZMK_RPC_SUBSYSTEM_HANDLER(core, enter_bootloader, ZMK_STUDIO_RPC_HANDLER_SECURED);

ZMK_RPC_SUBSYSTEM_HANDLER(core, get_device_info,''')

# Animate only on the existing low-priority light queue. User settings remain
# unchanged; power is removed only after the final black frame.
edit("app/src/rgb_underglow.c", 'static void zmk_rgb_underglow_tick(struct k_work *work) {', '''static uint8_t fade_level;
static int64_t fade_time;
static bool fade_target;
static void zmk_rgb_underglow_tick(struct k_work *work) {
    int64_t now = k_uptime_get();
    bool target = underglow_should_render();
    unsigned elapsed = MIN(MAX(now-fade_time, 0), 200);
    fade_time = now;
    unsigned delta = elapsed * 255 / 180;
    if (target) fade_level = MIN(255, fade_level + delta);
    else fade_level = delta >= fade_level ? 0 : fade_level - delta;
    fade_target = target;''')
edit("app/src/rgb_underglow.c", '    int err = led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);', '''    for (int i=0; i<STRIP_NUM_PIXELS; i++) {
        pixels[i].r = pixels[i].r * fade_level / 255;
        pixels[i].g = pixels[i].g * fade_level / 255;
        pixels[i].b = pixels[i].b * fade_level / 255;
    }
    int err = led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    if (!target && !fade_level) underglow_ext_power_set(false);
#endif''')
edit("app/src/rgb_underglow.c", 'if (!underglow_should_render()) {\n        return;\n    }\n    k_work_submit_to_queue', 'if (!underglow_should_render() && !fade_level) {\n        k_timer_stop(timer);\n        return;\n    }\n    k_work_submit_to_queue')
p=r/"app/src/rgb_underglow.c";s=p.read_text();a=s.index('    if (underglow_should_render()) {',s.index('int zmk_rgb_underglow_apply_state(void)'));b=s.index('\n    return 0;',a)
s=s[:a]+'''    if (underglow_should_render()) {
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
        underglow_ext_power_set(true);
#endif
    }
    /* Restart from the current brightness when toggled mid-transition. */
    if (fade_target != underglow_should_render()) fade_time = k_uptime_get();
    k_timer_start(&underglow_tick, K_NO_WAIT, K_MSEC(1000 / ANIMATION_FPS));
'''+s[b:];p.write_bytes(s.encode())
