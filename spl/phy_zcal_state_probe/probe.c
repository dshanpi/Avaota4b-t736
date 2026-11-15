// SPDX-License-Identifier: GPL-2.0-only
/* Fixed read-only state observation after the reset-only-release ZCal pulse.
 * No PHY/controller writes, program memory, MR command or DRAM content access.
 * UART TX and SRAM diagnostic records are the only writes.
 */
#include <stdint.h>
#define CAP 160u
#define NSTATIC 36u
#define EXPECTED_READS 155u
struct event { uint32_t op,address,value; };
struct report {
 uint32_t magic,version,status,sctlr,attempted,completed,writes,uart_error;
 uint32_t phase,reserved[3];
 struct event events[CAP];
};
_Static_assert(sizeof(struct report)==1968,"report ABI");
_Static_assert(EXPECTED_READS<=CAP,"capacity");
volatile struct report probe_report __attribute__((section(".report")));
static const uint32_t ca[12]={
 0x02002020,0x02002c00,0x02002c0c,0x0a020010,0x0a020014,0x07010250,
 0x08020000,0x08020020,0x0a02021c,0x0a020220,0x0a020224,0x0a020228
};
static const uint32_t cv[12]={0xf9004a00,0x80000003,0x10001,0x273f,2,1,1,24000000,0,0,0,0};
static const uint32_t offsets[11]={0,0x14,0x90,0x180,0x510,0x508,0x500,0x518,0xc80,0xc84,0x514};
static const uint32_t held[11]={0x03081002,0,0,0x800,0x10014,0xc0000000,0,0x25000000,1,1,0};
static const uint32_t phy_static[NSTATIC]={
 0x0a940094,0x0a940096,0x0a940110,0x0a940098,0x0a94009a,
 0x0a94014a,0x0a940000,0x0a940004,0x0a9400a2,0x0a9400c0,
 0x0a940600,0x0a940606,0x0a940604,0x0a940650,0x0a940602,0x0a940616,
 0x0a920126,0x0a922126,0x0a924126,0x0a926126,
 0x0a920146,0x0a922146,0x0a924146,0x0a926146,
 0x0a96015c,0x0a96215c,0x0a96015a,0x0a96215a,0x0a960158,0x0a962158,
 0x0aaa006c,0x0a94017a,0x0a940620,0x0a940622,0x0a94064c,0x0a94064e
};
/* Source-configured analog state; later pulse left ZCalReset/Run/PU/PD zero. */
static const uint32_t static_values[NSTATIC]={
 0x402,0x123e,0x101,0x2a8b,0,0,2,2,3,2,0x808,9,0x129,1,3,0,
 0x400,0x400,0x400,0x400,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0
};
static const uint32_t extra[3]={0x0a940008,0x0a940116,0x0a940120};
/* PclkDivRatio is a named static host configuration CSR. Logical2030d/20320
 * occur in independent SiMa/NXP CSR SAVE lists read by host code. Their names,
 * field semantics, freshness and clear-on-read behavior are not established.
 * All three are raw observations, never completion or success flags.
 */
static const uint32_t observations[3]={0x0a940002,0x0a94061a,0x0a940640};

#ifdef T736_PHY_ZCAL_STATE_NATIVE
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
  for(i=0;i<3;i++)if(a==extra[i] || a==observations[i])return 1;
 }
 return 0;
}
static int sample(unsigned int op,uint32_t a,uint32_t *v){
 uint32_t i=probe_report.completed;
 if(i>=CAP || !allowed_read(op,a))return -1;
 probe_report.events[i].op=op;probe_report.events[i].address=a;
 probe_report.attempted=i+1;
 if(progress(op,a)){probe_report.uart_error=1;return -1;}
#ifdef T736_PHY_ZCAL_STATE_NATIVE
 *v=test_read(a,op==3?16u:32u);
#else
 *v=op==3?*(volatile uint16_t *)(uintptr_t)a:*(volatile uint32_t *)(uintptr_t)a;
 __asm__ volatile("dsb sy":::"memory");
#endif
 probe_report.events[i].value=*v;probe_report.completed=i+1;return 0;
}
static int expect(unsigned int op,uint32_t a,uint32_t expected){uint32_t v;return sample(op,a,&v)||v!=expected?-1:0;}
static int guard(uint32_t *clock_value,int final){
 uint32_t i,ch,v;
 for(i=0;i<12;i++)if(expect(1,ca[i],cv[i]))return -1;
 for(ch=0;ch<2;ch++)for(i=0;i<11;i++)if(expect(1,0xa110000+ch*0x400000+offsets[i],held[i]))return -1;
 if(expect(3,0x0aaa0000,0)||expect(3,0x0aaa0132,1)||expect(3,0x0aa80100,3))return -1;
 for(i=0;i<NSTATIC;i++)if(expect(3,phy_static[i],static_values[i]))return -1;
 if(sample(3,extra[0],&v)||(v&0x7ffu)!=450u)return -1;
 if(final){if(v!=*clock_value)return -1;}else *clock_value=v;
 return expect(3,extra[1],0)||expect(3,extra[2],1)?-1:0;
}
int probe_main(void){
 uint32_t i,v,clock_value=0,sctlr=0xc50838;volatile uint32_t *p=(volatile uint32_t *)&probe_report;
 for(i=0;i<sizeof(probe_report)/4u;i++)p[i]=0;
 probe_report.magic=0x54375a4f;probe_report.version=1;probe_report.status=1;probe_report.phase=1;
#ifndef T736_PHY_ZCAL_STATE_NATIVE
 __asm__ volatile("mrc p15, 0, %0, c1, c0, 0":"=r"(sctlr));
#endif
 probe_report.sctlr=sctlr;if(sctlr&0x3005 || guard(&clock_value,0))goto fail;
 probe_report.phase=2;
 for(i=0;i<3;i++)if(sample(3,observations[i],&v))goto fail;
 probe_report.phase=3;if(guard(&clock_value,1))goto fail;
 if(probe_report.completed!=EXPECTED_READS || probe_report.writes)goto fail;
 probe_report.phase=4;probe_report.status=2;return 0;
fail:probe_report.status=0x100+probe_report.phase;return -1;
}
