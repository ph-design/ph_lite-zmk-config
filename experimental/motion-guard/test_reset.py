"""Host regression of the actual patched reset behavior, with hardware mocks."""
from pathlib import Path
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]) / 'app/src/behaviors/behavior_reset.c'
with tempfile.TemporaryDirectory(prefix='evo-reset-test-') as directory:
    root = Path(directory)
    headers = ['zephyr/device.h', 'zephyr/sys/reboot.h', 'zephyr/logging/log.h',
               'drivers/behavior.h', 'zmk/behavior.h', 'dt-bindings/zmk/reset.h',
               'hal/nrf_power.h', 'cmsis_core.h']
    for name in headers:
        file = root / name
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text('/* hardware mocked by harness */\n')
    harness = root / 'test.c'
    harness.write_text(r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define CONFIG_SOC_SERIES_NRF52X 1
#define DT_HAS_COMPAT_STATUS_OKAY(x) 1
#define DT_INST_FOREACH_STATUS_OKAY(x)
#define IS_ENABLED(x) 0
#define LOG_MODULE_DECLARE(...)
#define RST_UF2 0x57
#define ZMK_BEHAVIOR_OPAQUE 0
#define BEHAVIOR_LOCALITY_EVENT_SOURCE 0
struct device { const void *config; };
struct zmk_behavior_binding { int unused; };
struct zmk_behavior_binding_event { int unused; };
struct behavior_driver_api {
    int (*binding_pressed)(struct zmk_behavior_binding *,struct zmk_behavior_binding_event);
    int locality;
};
static struct device device;
static const struct device *zmk_behavior_get_binding(void *ignored) {
    (void)ignored; return &device;
}
/* Actual source references behavior_dev. */
#define behavior_dev unused
struct mock_power { volatile uint32_t GPREGRET; };
static struct mock_power power;
#define NRF_POWER (&power)
static unsigned resets, sys_resets;
static int last_type;
static void __DSB(void) {}
static void NVIC_SystemReset(void) { assert(power.GPREGRET==0x57); resets++; }
static void sys_reboot(int type) { last_type=type; sys_resets++; }
''' + '#include "' + source.as_posix() + '"\n' + r'''
int main(void) {
    struct behavior_reset_config cfg={.type=0x57};
    device.config=&cfg;
    struct zmk_behavior_binding binding={0};
    struct zmk_behavior_binding_event event={0};
    on_keymap_binding_pressed(&binding,event);
    assert(resets==1 && sys_resets==0 && power.GPREGRET==0x57);
    cfg.type=0;on_keymap_binding_pressed(&binding,event);
    assert(resets==1 && sys_resets==1 && last_type==0);
    cfg.type=1;on_keymap_binding_pressed(&binding,event);
    assert(resets==1 && sys_resets==2 && last_type==1);
    puts("PASS: actual reset behavior UF2 magic/direct reset, normal warm/cold reset");
}
''')
    executable = root / 'test'
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Wno-unused-parameter',
                    '-Wno-unused-const-variable', '-Wno-int-conversion',
                    '-fsanitize=address,undefined', '-I', str(root), str(harness),
                    '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
