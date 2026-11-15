// SPDX-License-Identifier: GPL-2.0-only
/* Controlled source-host ZCal pulse. Completion means TIMED_ONLY, not trained. */
#include <stdint.h>
#include "../lpddr4/phy_calibration_diag.h"
#define CAP 160u
struct event { uint32_t op,address,value; };
struct report {
 uint32_t magic,version,status,sctlr,attempted,completed,writes,uart_error;
 uint32_t phase,reset_ticks,run_ticks,component_result;
 struct event events[CAP];
};
_Static_assert(sizeof(struct report)==1968,"report ABI");
volatile struct report probe_report __attribute__((section(".report")));
static const uint32_t ca[]={0x02002020,0x02002c00,0x02002c0c,0x0a020010,0x0a020014,0x07010250,0x08020000,0x08020020,0x0a02021c,0x0a020220,0x0a020224,0x0a020228};
static const uint32_t cv[]={0xf9004a00,0x80000003,0x10001,0x273f,2,1,1,24000000,0,0,0,0};
static const uint32_t offsets[]={0,0x14,0x90,0x180,0x510,0x508,0x500,0x518,0xc80,0xc84,0x514};
static const uint32_t held[]={0x03081002,0,0,0x800,0x10014,0xc0000000,0,0x25000000,1,1,0};
static const uint32_t pa[]={0x0aaa0000,0x0aaa0132,0x0aa80100,0x0a94014a,0x0a9400c0,
 0x0a940094,0x0a940096,0x0a940110,0x0a940098,0x0a94009a,0x0a940000,
 0x0a940600,0x0a940606,0x0a940604,0x0a940650,0x0a940602,0x0a940616,
 0x0a940116,0x0a940120,0x0a940620,0x0a940622,0x0a94064c,0x0a94064e};
static const uint32_t pv[]={0,1,3,0,2,0x402,0x123e,0x101,0x2a8b,0,2,0x808,9,0x129,1,3,0,0,1,0,0,0,0};
static const uint32_t wa[]={0x0a940620,0x0a940606,0x0a940620,0x0a940622,0x0a940622,0x0a940606};
static const uint16_t wv[]={1,0x19,0,1,0,9};
#ifdef T736_PHY_ZCAL_RATE_NATIVE
uint32_t test_read(uint32_t address,unsigned int width);
void test_write(uint32_t address,uint16_t value);
uint32_t test_counter(void);
int test_progress(unsigned int op,uint32_t address);
#define counter test_counter
#define progress test_progress
#else
static uint32_t counter(void){uint32_t lo,hi;__asm__ volatile("isb\n\tmrrc p15, 0, %0, %1, c14":"=r"(lo),"=r"(hi)::"memory");return lo;}
static uint32_t raw_read(uint32_t a){uint32_t v=*(volatile uint32_t *)(uintptr_t)a;__asm__ volatile("dsb sy":::"memory");return v;}
static int tx(unsigned int c){unsigned int n=1000000;if(raw_read(0x250000c)&0x80)return -1;while(!(raw_read(0x2500014)&0x20))if(!--n)return -1;*(volatile uint32_t *)0x2500000=c;__asm__ volatile("dsb sy":::"memory");return 0;}
static int progress(unsigned int op,uint32_t a){static const char h[]="0123456789abcdef";int s;unsigned int n=1000000;if(tx(op==1?'R':op==3?'H':'S')||tx('='))return -1;for(s=28;s>=0;s-=4)if(tx((unsigned int)h[(a>>(unsigned int)s)&15]))return -1;if(tx('\r')||tx('\n'))return -1;while(!(raw_read(0x2500014)&0x40))if(!--n)return -1;return 0;}
#endif
static int allowed(unsigned int op,uint32_t a,uint32_t v){uint32_t i,ch;
 if(op==4){i=probe_report.writes;return probe_report.phase==3 && i<6 && a==wa[i] && v==wv[i];}
 if(op==1){for(i=0;i<12;i++)if(a==ca[i])return 1;for(ch=0;ch<2;ch++)for(i=0;i<11;i++)if(a==0xa110000+ch*0x400000+offsets[i])return 1;}
 if(op==3){if(a==0xa940008)return 1;for(i=0;i<23;i++)if(a==pa[i])return 1;}
 return 0;
}
static int access_word(unsigned int op,uint32_t a,uint32_t *v){uint32_t i=probe_report.completed;
 if(i>=CAP || !allowed(op,a,*v))return -1;
 probe_report.events[i].op=op;probe_report.events[i].address=a;probe_report.attempted=i+1;
 if(progress(op,a)){probe_report.uart_error=1;return -1;}
 if(op==4){probe_report.events[i].value=*v;probe_report.writes++;
#ifdef T736_PHY_ZCAL_RATE_NATIVE
 test_write(a,(uint16_t)*v);
#else
 *(volatile uint16_t *)(uintptr_t)a=(uint16_t)*v;
#endif
 }else{
#ifdef T736_PHY_ZCAL_RATE_NATIVE
 *v=test_read(a,op==3?16u:32u);
#else
 *v=op==3?*(volatile uint16_t *)(uintptr_t)a:*(volatile uint32_t *)(uintptr_t)a;
#endif
 probe_report.events[i].value=*v;}
#ifndef T736_PHY_ZCAL_RATE_NATIVE
 __asm__ volatile("dsb sy":::"memory");
#endif
 probe_report.completed=i+1;return 0;
}
static int expect(unsigned int op,uint32_t a,uint32_t expected){uint32_t v=0;return access_word(op,a,&v)||v!=expected?-1:0;}
static int context(void){uint32_t i,ch;for(i=0;i<12;i++)if(expect(1,ca[i],cv[i]))return -1;for(ch=0;ch<2;ch++)for(i=0;i<11;i++)if(expect(1,0xa110000+ch*0x400000+offsets[i],held[i]))return -1;return 0;}
static int wait_us(void *ctx,uint32_t us){uint32_t n,begin=counter();volatile uint32_t *ticks;(void)ctx;
 if(us!=1 && us!=100)return -1;
 ticks=us==1?&probe_report.reset_ticks:&probe_report.run_ticks;
 for(n=0;n<1000000;n++){*ticks=counter()-begin;if(*ticks>=us*24u)return 0;}return -1;
}
int probe_main(void){uint32_t i,v=0,sctlr=0xc50838;
 volatile uint32_t *p=(volatile uint32_t *)&probe_report;
 for(i=0;i<sizeof(probe_report)/4u;i++)p[i]=0;
 probe_report.magic=0x54375a52;probe_report.version=1;probe_report.status=1;probe_report.phase=1;
#ifndef T736_PHY_ZCAL_RATE_NATIVE
 __asm__ volatile("mrc p15, 0, %0, c1, c0, 0":"=r"(sctlr));
#endif
 probe_report.sctlr=sctlr;if(sctlr&0x3005 || context())goto fail;
 probe_report.phase=2;for(i=0;i<23;i++)if(expect(3,pa[i],pv[i]))goto fail;
 if(access_word(3,0xa940008,&v)||(v&0x7ff)!=450)goto fail;
 /* Confirm the counter advances BEFORE the first trigger write. */
 if(wait_us(0,1))goto fail;
 probe_report.phase=3;
 /* Single-variable ZCalRate bit4 experiment. Its exact field name is
  * unknown; the host-interface operation is corroborated separately.
  * Restoration is an explicit successful-path step, never failure cleanup. */
 v=1;if(access_word(4,0xa940620,&v)||wait_us(0,1)||expect(3,0xa940620,1))goto fail;
 v=0x19;if(access_word(4,0xa940606,&v)||expect(3,0xa940606,0x19))goto fail;
 v=0;if(access_word(4,0xa940620,&v))goto fail;
 v=1;if(access_word(4,0xa940622,&v)||wait_us(0,100))goto fail;
 v=0;if(access_word(4,0xa940622,&v))goto fail;
 /* Do not restore Rate while the pulse still reads asserted. PU/PD stay
  * raw observations; neither zero nor a changed code is a success test. */
 for(i=19;i<23;i++)if(i<21?expect(3,pa[i],0):access_word(3,pa[i],&v))goto fail;
 v=9;if(access_word(4,0xa940606,&v)||expect(3,0xa940606,9))goto fail;
 for(i=19;i<23;i++)if(i<21?expect(3,pa[i],0):access_word(3,pa[i],&v))goto fail;
 probe_report.component_result=T736_ZCAL_TIMED_ONLY;
 probe_report.phase=4;
 if(context()||expect(3,0xaaa0000,0)||expect(3,0xaaa0132,1)||expect(3,0xaa80100,3)||expect(3,0xa9400c0,2))goto fail;
 probe_report.phase=5;probe_report.status=2;return 0;
fail:probe_report.status=0x100+probe_report.phase;return -1;
}
