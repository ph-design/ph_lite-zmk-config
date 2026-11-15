from pathlib import Path
import re,tempfile,subprocess
root=Path(__file__).resolve().parents[1]
s=(root/"src/usb.c").read_text();descriptor=s[s.index("static const uint8_t descriptor[]="):s.index("struct usb_report")];descriptor=descriptor[descriptor.index("{"):descriptor.index("};")+1]
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/"test.c").write_text('''#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include "axis_descriptor.h"
#include "axis-state.h"
static const uint8_t descriptor[]='''+descriptor+''';
int main(void){
 unsigned bits[6]={0},id=0,size=0,count=0;
 for(unsigned i=0;i<sizeof(descriptor);){uint8_t tag=descriptor[i++];unsigned n=tag&3;if(n==3)n=4;unsigned value=0;for(unsigned j=0;j<n;j++)value|=(unsigned)descriptor[i++]<<(j*8);
 if(tag==0x85){id=value;}if(tag==0x75){size=value;}if(tag==0x95){count=value;}if((tag&0xfc)==0x80){assert(id<6);bits[id]+=size*count;}}
 assert(bits[1]==120 && bits[2]==192 && bits[3]==72 && bits[4]==16 && bits[5]==16);
 assert(hub_axis_fresh(1250,1000,-32767)==-32767 && hub_axis_fresh(1251,1000,32767)==0);
 assert(hub_axis_fresh(10,0xfffffff0u,123)==123);
 puts("PASS: actual Hub HID input sizes/IDs, independent controller reports and 250ms axis timeout including clock wrap");
}
''')
 subprocess.run(["gcc","-std=c11","-Wall","-Wextra","-I",str(root/"src"),str(p/"test.c"),"-o",str(p/"test")],check=True)
 subprocess.run([str(p/"test")],check=True)

# Execute the real USB output loop: timeout must still center axes under key traffic.
usb=(root/"src/usb.c").read_text()
loop=usb[usb.index("static void usb_thread(void)"):usb.index("K_THREAD_DEFINE(usb_output")]
event=usb[usb.index("void hub_axis_event("):usb.index("static void submit(")]
with tempfile.TemporaryDirectory() as d:
 p=Path(d);source=r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <setjmp.h>
#include <assert.h>
#include <stdio.h>
#include "axis-state.h"
typedef int32_t atomic_t;static int32_t atomic_get(atomic_t*p){return *p;}static int32_t atomic_set(atomic_t*p,int32_t v){int32_t old=*p;*p=v;return old;}
static uint32_t now;static uint32_t k_uptime_get_32(void){return now;}
static atomic_t ready=1,axis_force=1,axes[2],axis_time[2];static uint8_t last[5][24];
struct usb_report{uint8_t len,data[25];};static int usb_reports;
#define K_MSEC(x) (x)
static jmp_buf done;static unsigned remaining;static bool keys;
static int k_msgq_get(int*q,struct usb_report*r,int timeout){now+=10;if(!remaining--)longjmp(done,1);if(!keys)return -1;memset(r,0,sizeof(*r));r->len=16;r->data[0]=1;return 0;}
static void sys_put_le16(uint16_t v,uint8_t*p){p[0]=v;p[1]=v>>8;}
static uint16_t sys_get_le16(const uint8_t*p){return p[0]|(uint16_t)p[1]<<8;}
static int16_t outputs[6];static unsigned count[6];
static void submit(uint8_t id,const uint8_t*data,unsigned len){count[id]++;if(id>=4){assert(len==2);outputs[id]=(int16_t)sys_get_le16(data);}}
'''+event+loop+r'''
static void run(unsigned n){remaining=n;if(!setjmp(done))usb_thread();}
int main(void){now=100;hub_axis_event(0,1234);hub_axis_event(1,-2345);run(5);assert(outputs[4]==1234&&outputs[5]==-2345);
 keys=true;now=400;run(5);assert(count[1]>0&&outputs[4]==0&&outputs[5]==0); /* key traffic cannot postpone neutral */
 now=0xfffffff0;hub_axis_event(0,-1000);hub_axis_event(1,2000);now=10;run(5);assert(outputs[4]==-1000&&outputs[5]==2000);
 hub_axis_event(0,0);run(5);assert(outputs[4]==0&&outputs[5]==2000); /* independent peer state */
 puts("PASS: actual Hub USB loop per-peer axis isolation, axis handoff, neutral timeout under key traffic and clock wrap");return 0;}
'''
 (p/"loop.c").write_text(source)
 subprocess.run(["gcc","-std=c11","-Wall","-Wextra","-Wno-unused-parameter","-fsanitize=address,undefined","-I",str(root/"src"),str(p/"loop.c"),"-o",str(p/"loop")],check=True)
 subprocess.run([str(p/"loop")],check=True)
