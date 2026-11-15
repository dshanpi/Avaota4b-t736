// SPDX-License-Identifier: GPL-2.0-only
/* Read fixed controller control words. UART TX is the only peripheral write. */
#include <stdint.h>
#define NCONTROL 20u
#define NWORDS (12u + 2u * NCONTROL + 6u)
struct report {
 uint32_t magic, version, status, sctlr, attempted, completed, uart_error, count;
 uint32_t address[NWORDS], value[NWORDS];
};
_Static_assert(sizeof(struct report) == 496, "report ABI");
volatile struct report probe_report __attribute__((section(".report")));
static const uint32_t context_address[6] = {0x02002020,0x02002c00,0x02002c0c,0x0a020010,0x0a020014,0x07010250};
static const uint32_t context_value[6] = {0xf9004a00,0x80000003,0x10001,0x2728,2,1};
static const uint32_t control_offset[NCONTROL] = {
 0x010,0x180,0x18c,0x200,0x208,0x280,0x288,0x300,0x308,0x380,
 0x500,0x508,0x510,0x518,0x514,0xc80,0xc84,0xc9c,0x080,0x084
};
#ifdef T736_CONTROL_STATE_NATIVE
uint32_t control_test_read(uint32_t address);
int control_test_putc(unsigned int value);
int control_test_drain(void);
#define read32 control_test_read
#define putc_uart control_test_putc
#define drain_uart control_test_drain
#else
static uint32_t read32(uint32_t address) {
 uint32_t value=*(volatile uint32_t *)(uintptr_t)address;
 __asm__ volatile("dsb sy" ::: "memory"); return value;
}
static int putc_uart(unsigned int value) {
 unsigned int left=1000000;
 if(read32(0x0250000c)&0x80)return -1; /* DLAB must be clear: THR, not divisor. */
 while (!(read32(0x02500014)&0x20)) if (!--left) return -1;
 *(volatile uint32_t *)(uintptr_t)0x02500000=value;
 __asm__ volatile("dsb sy" ::: "memory"); return 0;
}
static int drain_uart(void) {
 unsigned int left=1000000;
 while (!(read32(0x02500014)&0x40)) if (!--left) return -1;
 return 0;
}
#endif
static int progress(uint32_t address) {
 static const char hex[]="0123456789abcdef";
 int shift;
 if (putc_uart('R') || putc_uart('=')) return -1;
 for(shift=28;shift>=0;shift-=4)
  if(putc_uart((unsigned int)hex[(address>>(unsigned int)shift)&15]))return -1;
 return putc_uart('\r') || putc_uart('\n') || drain_uart() ? -1 : 0;
}
static int sample(uint32_t address, uint32_t *value) {
 uint32_t i=probe_report.completed;
 if(i>=NWORDS)return -1;
 probe_report.address[i]=address; probe_report.attempted=i+1;
 /* Emit the exact next address before touching it; helps diagnose a bus hang. */
 if(progress(address)){probe_report.uart_error=1;return -1;}
 *value=read32(address);probe_report.value[i]=*value;probe_report.completed=i+1;
 return 0;
}
int probe_main(void) {
 uint32_t i,ch,v,sctlr; volatile uint32_t *p=(volatile uint32_t *)&probe_report;
 for(i=0;i<sizeof(probe_report)/4u;i++)p[i]=0;
 probe_report.magic=0x54374353;probe_report.version=1;probe_report.status=1;probe_report.count=NWORDS;
#ifndef T736_CONTROL_STATE_NATIVE
 __asm__ volatile("mrc p15, 0, %0, c1, c0, 0":"=r"(sctlr));
#else
 sctlr=0xc50838;
#endif
 probe_report.sctlr=sctlr;
 if(sctlr&0x3005){probe_report.status=0x100;return -1;}
 for(i=0;i<6;i++)if(sample(context_address[i],&v)||v!=context_value[i])goto precondition;
 for(ch=0;ch<2;ch++){
  uint32_t base=0x0a110000+ch*0x400000;
  if(sample(base,&v)||v!=0x03081002)goto precondition;
  if(sample(base+0x14,&v)||v)goto precondition;
  if(sample(base+0x90,&v)||v)goto precondition;
 }
 for(ch=0;ch<2;ch++)for(i=0;i<NCONTROL;i++)
  if(sample(0x0a110000+ch*0x400000+control_offset[i],&v))goto failure;
 for(i=0;i<6;i++)if(sample(context_address[i],&v)||v!=context_value[i])goto failure;
 probe_report.status=2;return 0;
precondition:probe_report.status=0x101;return -1;
failure:probe_report.status=0x102;return -1;
}
