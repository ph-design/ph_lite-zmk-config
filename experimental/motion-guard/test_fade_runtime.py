"""Exercise actual RGB tick/apply-state code, including mid-fade reversals."""
from pathlib import Path
import tempfile, subprocess, sys, re
source=(Path(sys.argv[1])/"app/src/rgb_underglow.c").read_text()
def function(name):
    a=re.search(r'^(?:static )?(?:int|void) '+name+r'\(',source,re.M).start();b=source.index('{',a)+1;depth=1
    while depth:
        depth+=(source[b]=='{')-(source[b]=='}');b+=1
    return source[a:b]
with tempfile.TemporaryDirectory(prefix='rgb-fade-') as d:
    p=Path(d)
    code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER 1
#define UNDERGLOW_LAYER_ENABLED 0
#define IS_ENABLED(x) (x)
#define K_NO_WAIT 0
#define K_MSEC(x) (x)
#define ANIMATION_FPS 60
#define STRIP_NUM_PIXELS 2
#define LOG_ERR(...)
struct k_work{int unused;};struct k_timer{int unused;};
static struct k_timer underglow_tick;static unsigned starts,writes;static bool power,target;
static int64_t now;static uint8_t fade_level;static int64_t fade_time;static bool fade_target;
static int64_t k_uptime_get(void){return now;}
static bool underglow_should_render(void){return target;}
static void underglow_ext_power_set(bool on){power=on;}
static void k_timer_start(struct k_timer*t,int first,int ms){starts++;}
static struct {unsigned current_effect;} state;
struct effect_event{unsigned position;};static int effect_event_msgq;
static int k_msgq_get(void*q,void*event,int wait){return -ENOMSG;}
static void render(void){}
struct rgb_effect_desc{void(*on_keypress)(unsigned);void(*render)(void);};
static const struct rgb_effect_desc effect_table[]={{0,render}};
struct led_rgb{uint8_t r,g,b;};static struct led_rgb pixels[STRIP_NUM_PIXELS];
static float fx_pixels[STRIP_NUM_PIXELS];static void*led_strip=(void*)1;
static void rgb_float_to_led(int i,float*f,struct led_rgb*p){*p=(struct led_rgb){200,100,50};}
static void zmk_rgb_underglow_apply_status_overlay(int layer){}
static int rgb_underglow_top_layer(void){return 0;}
static int led_strip_update_rgb(void*dev,struct led_rgb*p,int n){writes++;return 0;}
'''
    code+='\n'+function('zmk_rgb_underglow_tick')+'\n'+function('zmk_rgb_underglow_apply_state')
    code+=r'''
int main(void){
 target=true;assert(!zmk_rgb_underglow_apply_state()&&power&&writes==0);
 zmk_rgb_underglow_tick(0);assert(fade_level==0&&pixels[0].r==0);
 now=90;zmk_rgb_underglow_tick(0);assert(fade_level>=126&&fade_level<=128&&pixels[0].r>90&&pixels[0].r<110);
 now=181;zmk_rgb_underglow_tick(0);assert(fade_level==255&&pixels[0].r==200);
 target=false;assert(!zmk_rgb_underglow_apply_state()&&power&&writes==3);
 now+=90;zmk_rgb_underglow_tick(0);assert(power&&fade_level>=127&&fade_level<=129);
 target=true;zmk_rgb_underglow_apply_state();now+=90;zmk_rgb_underglow_tick(0);assert(power&&fade_level>=254);
 target=false;zmk_rgb_underglow_apply_state();now+=181;zmk_rgb_underglow_tick(0);assert(!power&&!fade_level&&!pixels[0].r);
 led_strip=0;assert(zmk_rgb_underglow_apply_state()==-ENODEV);
 puts("PASS: actual RGB fade-in/out, no synchronous LED write from apply-state, reversal and delayed external-power cutoff");
}
'''
    (p/'test.c').write_text(code)
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Wno-unused-parameter','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
