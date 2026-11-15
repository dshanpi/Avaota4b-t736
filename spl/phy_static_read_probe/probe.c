// SPDX-License-Identifier: GPL-2.0-only
/* Fixed physical PHY configuration CSR observation. UART TX is the only
 * peripheral write. No program RAM, mailbox, ACK, PHY or controller writes.
 * Facts: lpddr4/phy/static-analog-audit-20261114.json; four additional ordinary
 * ZCal control/result reads independently reviewed by the hardware owner.
 */
#include <stdint.h>
#define CAP 160u
#define NSTATIC 36u
#define EXPECTED_READS (12u + 22u + 2u + 1u + NSTATIC + 12u + 22u + 2u)
struct event { uint32_t op, address, value; };
struct report {
 uint32_t magic,version,status,sctlr,attempted,completed,writes,uart_error;
 uint32_t phase,reserved[3];
 struct event events[CAP];
};
_Static_assert(sizeof(struct report)==1968,"report ABI");
_Static_assert(EXPECTED_READS==109u && EXPECTED_READS<=CAP,"read count");
volatile struct report probe_report __attribute__((section(".report")));
static const uint32_t ca[12]={
 0x02002020,0x02002c00,0x02002c0c,0x0a020010,0x0a020014,0x07010250,
 0x08020000,0x08020020,0x0a02021c,0x0a020220,0x0a020224,0x0a020228
};
static const uint32_t cv[12]={0xf9004a00,0x80000003,0x10001,0x273f,2,1,1,24000000,0,0,0,0};
static const uint32_t offsets[11]={0,0x14,0x90,0x180,0x510,0x508,0x500,0x518,0xc80,0xc84,0x514};
static const uint32_t held[11]={0x03081002,0,0,0x800,0x10014,0xc0000000,0,0x25000000,1,1,0};
static const uint32_t phy_static[NSTATIC]={
 /* MASTER PLL configuration; named configuration words, not instruction RAM. */
 0x0a940094,0x0a940096,0x0a940110,0x0a940098,0x0a94009a,
 /* LP5Mode, DfiFreqRatio, PclkPtrInitVal, DfiMode, MemResetL (read only). */
 0x0a94014a,0x0a940000,0x0a940004,0x0a9400a2,0x0a9400c0,
 /* ZCal analog prerequisites. */
 0x0a940600,0x0a940606,0x0a940604,0x0a940650,0x0a940602,0x0a940616,
 /* Four DBYTE Rx power words, then four static PptCtl words. */
 0x0a920126,0x0a922126,0x0a924126,0x0a926126,
 0x0a920146,0x0a922146,0x0a924146,0x0a926146,
 /* Both AC lanes: Pclk disable, DFI clock disable, lane disable. */
 0x0a96015c,0x0a96215c,0x0a96015a,0x0a96215a,0x0a960158,0x0a962158,
 /* Named host-clock and performance configuration controls, read only. */
 0x0aaa006c,0x0a94017a,
 /* ZCalReset, ZCalRun, PU and PD result: no trigger or reset writes. */
 0x0a940620,0x0a940622,0x0a94064c,0x0a94064e
};

#ifdef T736_PHY_STATIC_READ_NATIVE
uint32_t test_read(uint32_t address,unsigned int width);
int test_progress(unsigned int op,uint32_t address);
#define progress test_progress
#else
static uint32_t raw_read(uint32_t a){uint32_t v=*(volatile uint32_t *)(uintptr_t)a;__asm__ volatile("dsb sy":::"memory");return v;}
static int tx(unsigned int c){unsigned int n=1000000;if(raw_read(0x250000c)&0x80)return -1;while(!(raw_read(0x2500014)&0x20))if(!--n)return -1;*(volatile uint32_t *)0x2500000=c;__asm__ volatile("dsb sy":::"memory");return 0;}
static int progress(unsigned int op,uint32_t a){static const char h[]="0123456789abcdef";int s;unsigned int n=1000000;if(tx(op==1?'R':'H')||tx('='))return -1;for(s=28;s>=0;s-=4)if(tx((unsigned int)h[(a>>(unsigned int)s)&15]))return -1;if(tx('\r')||tx('\n'))return -1;while(!(raw_read(0x2500014)&0x40))if(!--n)return -1;return 0;}
#endif

static int allowed_read(unsigned int op,uint32_t a){
 uint32_t i,ch;
 if(op==1){
  for(i=0;i<12;i++)if(a==ca[i])return 1;
  for(ch=0;ch<2;ch++)for(i=0;i<11;i++)if(a==0xa110000+ch*0x400000+offsets[i])return 1;
 }else if(op==3){
  if(a==0x0aaa0000 || a==0x0aaa0132 || a==0x0aa80100)return 1;
  for(i=0;i<NSTATIC;i++)if(a==phy_static[i])return 1;
 }
 return 0;
}

static int sample(unsigned int op,uint32_t a,uint32_t *v){
 uint32_t i=probe_report.completed;
 if(i>=CAP || !allowed_read(op,a))return -1;
 probe_report.events[i].op=op;probe_report.events[i].address=a;
 probe_report.attempted=i+1;
 /* Complete the UART line before the potentially non-returning bus read. */
 if(progress(op,a)){probe_report.uart_error=1;return -1;}
#ifdef T736_PHY_STATIC_READ_NATIVE
 *v=test_read(a,op==3?16u:32u);
#else
 *v=op==3?*(volatile uint16_t *)(uintptr_t)a:*(volatile uint32_t *)(uintptr_t)a;
 __asm__ volatile("dsb sy":::"memory");
#endif
 probe_report.events[i].value=*v;
 probe_report.completed=i+1;return 0;
}

static int expect(unsigned int op,uint32_t a,uint32_t expected){uint32_t v;return sample(op,a,&v)||v!=expected?-1:0;}
static int context_and_controllers(void){
 uint32_t i,ch;
 for(i=0;i<12;i++)if(expect(1,ca[i],cv[i]))return -1;
 for(ch=0;ch<2;ch++)for(i=0;i<11;i++)if(expect(1,0xa110000+ch*0x400000+offsets[i],held[i]))return -1;
 return 0;
}
static int outer(void){return expect(3,0x0aaa0000,0)||expect(3,0x0aaa0132,1)?-1:0;}

int probe_main(void){
 uint32_t i,v,sctlr=0xc50838;volatile uint32_t *p=(volatile uint32_t *)&probe_report;
 for(i=0;i<sizeof(probe_report)/4u;i++)p[i]=0;
 probe_report.magic=0x54375053;probe_report.version=1;probe_report.status=1;probe_report.phase=1;
#ifndef T736_PHY_STATIC_READ_NATIVE
 __asm__ volatile("mrc p15, 0, %0, c1, c0, 0":"=r"(sctlr));
#endif
 probe_report.sctlr=sctlr;if(sctlr&0x3005)goto fail;
 if(context_and_controllers())goto fail;
 probe_report.phase=2;if(outer())goto fail;
 probe_report.phase=3;
 /* Observation only: unknown/zero clock-enable bits do not invent a write. */
 if(sample(3,0x0aa80100,&v))goto fail;
 probe_report.phase=4;
 for(i=0;i<NSTATIC;i++)if(sample(3,phy_static[i],&v))goto fail;
 probe_report.phase=5;
 if(context_and_controllers()||outer())goto fail;
 if(probe_report.completed!=EXPECTED_READS || probe_report.writes)goto fail;
 probe_report.phase=6;probe_report.status=2;return 0;
fail:probe_report.status=0x100+probe_report.phase;return -1;
}
