"""Exercise the actual sensorless input adapter, without MCU or sensor mocks."""
from pathlib import Path
import tempfile, subprocess
assets=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix="no-imu-") as directory:
    root=Path(directory)
    for name in ["zephyr/kernel.h","zmk/motion_guard.h","zmk/events/position_state_changed.h"]:
        p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text("/* mocked */\n")
    source=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define ARG_UNUSED(x) (void)(x)
struct sensor_value;
struct zmk_position_state_changed {uint8_t source;uint32_t position;bool state;int64_t timestamp;};
#define ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL 0
static struct zmk_position_state_changed output[256];static unsigned count;
static void raise_zmk_position_state_changed(struct zmk_position_state_changed e){assert(count<256);output[count++]=e;}
'''
    source+="\n#include \""+(assets/"no_imu.c").as_posix()+"\"\n"
    source+=r'''
int main(void){
 for(unsigned i=0;i<128;i++){zmk_motion_guard_key(i,true,i*2);zmk_motion_guard_key(i,false,i*2+1);}
 assert(count==256);for(unsigned i=0;i<256;i++){assert(output[i].position==i/2 && output[i].timestamp==i && output[i].state==(i%2==0));}
 assert(!zmk_motion_guard_blocks_output() && !zmk_motion_guard_keeps_awake());
 zmk_motion_guard_set_enabled(true);assert(!zmk_motion_guard_blocks_output());
 zmk_motion_guard_prepare_sleep();zmk_motion_guard_resume_sampling();
 assert(zmk_motion_guard_read_accel(0)==-ENOTSUP && zmk_motion_guard_set_tap_odr(true)==-ENOTSUP);
 return 0;
}
'''
    (root/"test.c").write_text(source)
    subprocess.run(["gcc","-std=c11","-Wall","-Wextra","-Werror","-fsanitize=address,undefined","-I",str(root),str(root/"test.c"),"-o",str(root/"test")],check=True)
    subprocess.run([str(root/"test")],check=True)
print("PASS: actual no-IMU adapter forwards all 128 positions immediately/in order; no lock, sampler or sensor sleep dependency")
