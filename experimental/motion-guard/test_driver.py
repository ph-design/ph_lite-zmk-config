"""Exercise actual patched conversion, unpacking and per-axis getters with register fixtures."""
from pathlib import Path
import subprocess, sys, tempfile
assets = Path(__file__).resolve().parent
driver = (Path(sys.argv[1]) / 'app/module/drivers/sensor/lis2dh/lis2dh.c').read_text()
def function(name):
    start = driver.index('static int ' + name + '(') if name != 'lis2dh_convert' else driver.index('static void lis2dh_convert(')
    brace = driver.index('{', start); depth = 1; end = brace + 1
    while depth:
        depth += (driver[end] == '{') - (driver[end] == '}'); end += 1
    return driver[start:end]
with tempfile.TemporaryDirectory(prefix='lis2dh-units-') as directory:
    root = Path(directory)
    source = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define CONFIG_ZMK_LIS2DH_OPER_MODE_NORMAL 1
#define LIS2DH_REG_STATUS 0x27
#define LIS2DH_STATUS_DRDY_MASK 0x08
#define LOG_WRN(...)
struct sensor_value { int32_t val1, val2; };
enum sensor_channel { SENSOR_CHAN_ACCEL_X, SENSOR_CHAN_ACCEL_Y, SENSOR_CHAN_ACCEL_Z, SENSOR_CHAN_ACCEL_XYZ, SENSOR_CHAN_LIS2DH_ORIENTATION };
struct device { void *data; };
struct transfer {int (*read_data)(const struct device*,uint8_t,uint8_t*,uint8_t);};
union sample {uint8_t raw[7];struct {uint8_t status;int16_t xyz[3];}__attribute__((packed));};
struct lis2dh_data {union sample sample; uint32_t scale; uint8_t orientation; const struct transfer *hw_tf;};
static uint8_t fixture[7];
static int read_data(const struct device*d,uint8_t reg,uint8_t*out,uint8_t len){assert(reg==0x27 && len==7);memcpy(out,fixture,7);return 0;}
static int lis2dh_orient_from_zone(uint8_t zone){return zone;}
'''
    source += '#include "' + (assets / 'lis2dh_decode.h').as_posix() + '"\n'
    source += '\n'.join(function(name) for name in ['lis2dh_convert','lis2dh_channel_get','lis2dh_fetch_xyz'])
    source += r'''
static int64_t micro(struct sensor_value v){return (int64_t)v.val1*1000000+v.val2;}
int main(void){
 const uint32_t sensitivities[]={1600,3200,6400,19200};
 for(unsigned fs=0;fs<4;fs++)for(unsigned shift=4;shift<=8;shift+=2){
   int16_t raw=(int16_t)(10U<<shift);int64_t expected=10LL*9806650*sensitivities[fs]/(100000*(1U<<(8-shift)));
   assert(lis2dh_decode_micro_ms2(raw,sensitivities[fs],shift)==expected);
   raw=(int16_t)(-(10*(1<<shift)));assert(lis2dh_decode_micro_ms2(raw,sensitivities[fs],shift)==-expected);
 }
 assert(lis2dh_decode_micro_ms2(16000,1600,6)==9806650); /* +1g normal */
 assert(lis2dh_decode_micro_ms2(-16000,1600,6)==-9806650);
 assert(lis2dh_decode_micro_ms2(16000,1600,4)==9806650); /* +1g HR */
 assert(lis2dh_decode_micro_ms2(0,1600,8)==0);
 const uint8_t bytes[]={0x08,0x80,0x3e,0x00,0x83,0x40,0x1f}; /* X +1g, Y -2g, Z +0.5g */
 memcpy(fixture,bytes,7);const struct transfer tf={read_data};struct lis2dh_data data={.scale=1600,.hw_tf=&tf};struct device dev={&data};
 assert(!lis2dh_fetch_xyz(&dev,SENSOR_CHAN_ACCEL_XYZ));
 struct sensor_value xyz[3];assert(!lis2dh_channel_get(&dev,SENSOR_CHAN_ACCEL_XYZ,xyz));
 assert(micro(xyz[0])==9806650 && micro(xyz[1])==-19613300 && micro(xyz[2])==4903325);
 struct sensor_value value;
 for(int i=0;i<3;i++){assert(!lis2dh_channel_get(&dev,(enum sensor_channel)i,&value));assert(micro(value)==micro(xyz[i]));}
 assert(data.sample.status==0x08);
 puts("PASS: actual driver unpack/X/Y/Z getters, signed and zero data, 8/10/12-bit modes, all ranges, exact nominal 1g; no unaligned word access");
 return 0;
}
'''
    (root/'test.c').write_text(source)
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Wno-unused-parameter','-fsanitize=address,undefined',str(root/'test.c'),'-o',str(root/'test')],check=True)
    subprocess.run([str(root/'test')],check=True)
