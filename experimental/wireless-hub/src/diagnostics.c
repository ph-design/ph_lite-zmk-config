#include <zephyr/kernel.h>
#include <zephyr/fatal.h>
#include <zephyr/init.h>
#include <zephyr/arch/arm/exception.h>
#include <hal/nrf_power.h>
#include "diagnostics.h"

/* RAM only: survive a software fault reset without touching pairing/NVS. */
static __noinit volatile struct {
    uint32_t magic, reason, pc, stage, line;
    char file[96];
} retained;
static uint32_t last_reason, last_pc, last_stage, last_line;
static char last_file[96];
static volatile uint32_t stage;
static int diagnostics_init(void) {
    if (retained.magic == 0x48554245) {
        last_reason=retained.reason;last_pc=retained.pc;last_stage=retained.stage;
        last_line=retained.line;
        for(unsigned i=0;i<sizeof(last_file);i++)last_file[i]=retained.file[i];
        last_file[sizeof(last_file)-1]=0;
    }
    retained.magic=0;return 0;
}
SYS_INIT(diagnostics_init,PRE_KERNEL_1,0);
void hub_diagnostic_stage(unsigned int value){stage=value;}
bool hub_diagnostics_safe_mode(void){return last_reason!=0;}
void hub_diagnostics_status(char *out,size_t size) {
    snprintk(out,size,"HUB_FAULT=%u\r\nHUB_FAULT_PC=%08X\r\nHUB_FAULT_STAGE=%u\r\nHUB_TRACE_STAGE=%u\r\nHUB_ASSERT_FILE=%s\r\nHUB_ASSERT_LINE=%u\r\nHUB_SAFE_MODE=%u\r\n",
             last_reason,last_pc,last_stage,stage,last_file,last_line,hub_diagnostics_safe_mode());
}
void k_sys_fatal_error_handler(unsigned int reason,const struct arch_esf *esf) {
    retained.reason=reason+1; /* zero means no retained fault */
    retained.pc=esf?esf->basic.pc:0;retained.stage=stage;
    retained.line=0;retained.file[0]=0;
    retained.magic=0x48554245;
    __DSB();NVIC_SystemReset();
    for(;;){}
}
void assert_post_action(const char *file,unsigned int line) {
    retained.reason=5;retained.pc=(uint32_t)__builtin_return_address(0);
    retained.stage=stage;retained.line=line;
    unsigned i=0;
    while(file && file[i] && i<sizeof(retained.file)-1){retained.file[i]=file[i];i++;}
    retained.file[i]=0;retained.magic=0x48554245;
    __DSB();NVIC_SystemReset();
    for(;;){}
}
