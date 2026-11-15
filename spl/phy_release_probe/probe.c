// SPDX-License-Identifier: GPL-2.0-only
/* SRAM diagnostic: inhibit DFI handoff, release APB domain, observe owner.
 * Host-interface facts: phy/pre-release-controller-audit-20261114.json.
 * No vendor program, PHY write, MR command or DRAM content access. */
#include <stdint.h>
#define CAP 160u
struct event { uint32_t op, address, value; };
struct report {
 uint32_t magic,version,status,sctlr,attempted,completed,writes,uart_error;
 uint32_t phase,pre_ticks,release_ticks[2];
 struct event events[CAP];
};
_Static_assert(sizeof(struct report)==1968,"report ABI");
volatile struct report probe_report __attribute__((section(".report")));
static const uint32_t ca[]={0x02002020,0x02002c00,0x02002c0c,0x0a020010,0x0a020014,0x07010250,0x08020000,0x08020020,0x0a02021c,0x0a020220,0x0a020224,0x0a020228};
static const uint32_t cv[]={0xf9004a00,0x80000003,0x10001,0x2728,2,1,1,24000000,0,0,0,0};
static const uint32_t offsets[]={0,0x14,0x90,0x180,0x510,0x508,0x500,0x518,0xc80,0xc84,0x514};
static const uint32_t initial[]={0x03081002,0,0,0,1,0x8000,0x100000,0x80000001,1,1,0};
static const uint32_t held[]={0x03081002,0,0,0x800,0x10014,0xc0000000,0,0x25000000,1,1,0};
#ifdef T736_PHY_RELEASE_NATIVE
uint32_t test_read(uint32_t address,unsigned int width);
void test_write(uint32_t address,uint32_t value);
uint32_t test_counter(void);
int test_progress(unsigned int op,uint32_t address);
#define counter test_counter
#define progress test_progress
#else
static uint32_t counter(void){uint32_t lo,hi;__asm__ volatile("isb\n\tmrrc p15, 0, %0, %1, c14":"=r"(lo),"=r"(hi)::"memory");return lo;}
static uint32_t raw_read(uint32_t a){uint32_t v=*(volatile uint32_t *)(uintptr_t)a;__asm__ volatile("dsb sy":::"memory");return v;}
static int tx(unsigned int c){unsigned int n=1000000;if(raw_read(0x250000c)&0x80)return -1;while(!(raw_read(0x2500014)&0x20))if(!--n)return -1;*(volatile uint32_t *)0x2500000=c;__asm__ volatile("dsb sy":::"memory");return 0;}
static int progress(unsigned int op,uint32_t a){static const char h[]="0123456789abcdef";int s;unsigned int n=1000000;if(tx(op==1?'R':op==2?'W':'H')||tx('='))return -1;for(s=28;s>=0;s-=4)if(tx((unsigned int)h[(a>>(unsigned int)s)&15]))return -1;if(tx('\r')||tx('\n'))return -1;while(!(raw_read(0x2500014)&0x40))if(!--n)return -1;return 0;}
#endif
static int access_word(unsigned int op,uint32_t a,uint32_t *v){
 uint32_t i=probe_report.completed;
 if(i>=CAP)return -1;
 probe_report.events[i].op=op;probe_report.events[i].address=a;
 probe_report.attempted=i+1;
 if(progress(op,a)){probe_report.uart_error=1;return -1;}
 if(op==2){
  probe_report.events[i].value=*v;probe_report.writes++;
#ifdef T736_PHY_RELEASE_NATIVE
  test_write(a,*v);
#else
  *(volatile uint32_t *)(uintptr_t)a=*v;
#endif
 }else{
#ifdef T736_PHY_RELEASE_NATIVE
  *v=test_read(a,op==3?16u:32u);
#else
  *v=op==3?*(volatile uint16_t *)(uintptr_t)a:*(volatile uint32_t *)(uintptr_t)a;
#endif
  probe_report.events[i].value=*v;
 }
#ifndef T736_PHY_RELEASE_NATIVE
 __asm__ volatile("dsb sy":::"memory");
#endif
 probe_report.completed=i+1;return 0;
}
static int expect(uint32_t a,uint32_t expected){uint32_t v;return access_word(1,a,&v)||v!=expected?-1:0;}
static int set(uint32_t a,uint32_t v){return access_word(2,a,&v)||expect(a,v)?-1:0;}
static int delay(uint32_t us,volatile uint32_t *ticks){uint32_t begin=counter(),n;for(n=0;n<1000000;n++){*ticks=counter()-begin;if(*ticks>=us*24u)return 0;}return -1;}
int probe_main(void){
 uint32_t i,ch,v,sctlr=0xc50838;volatile uint32_t *p=(volatile uint32_t *)&probe_report;
 for(i=0;i<sizeof(probe_report)/4u;i++)p[i]=0;
 probe_report.magic=0x54375052;probe_report.version=1;probe_report.status=1;probe_report.phase=1;
#ifndef T736_PHY_RELEASE_NATIVE
 __asm__ volatile("mrc p15, 0, %0, c1, c0, 0":"=r"(sctlr));
#endif
 probe_report.sctlr=sctlr;if(sctlr&0x3005)goto fail;
 for(i=0;i<12;i++)if(expect(ca[i],cv[i]))goto fail;
 for(ch=0;ch<2;ch++)for(i=0;i<11;i++)if(expect(0xa110000+ch*0x400000+offsets[i],initial[i]))goto fail;
 if(delay(10,&probe_report.pre_ticks))goto fail;
 probe_report.phase=2;
 for(ch=0;ch<2;ch++){
  uint32_t base=0xa110000+ch*0x400000;
  if(set(base+0xc80,0))goto fail;
  for(i=3;i<=7;i++)if(set(base+offsets[i],held[i]))goto fail;
  if(set(base+0xc80,1))goto fail;
  for(i=0;i<10;i++){
   if(access_word(1,base+0xc84,&v))goto fail;
   if(v==1)break;
   if(v&~1u)goto fail;
   if(delay(10,&probe_report.pre_ticks))goto fail;
  }
  if(i==10)goto fail;
 }
 for(ch=0;ch<2;ch++)for(i=0;i<11;i++)if(expect(0xa110000+ch*0x400000+offsets[i],held[i]))goto fail;
 if(expect(0xa020010,0x2728))goto fail;
 probe_report.phase=3;
 if(set(0xa020010,0x273c)||set(0xa020010,0x273e)||delay(1,&probe_report.release_ticks[0])||set(0xa020010,0x273f)||delay(1,&probe_report.release_ticks[1]))goto fail;
 probe_report.phase=4;
 for(ch=0;ch<2;ch++)for(i=0;i<11;i++)if(expect(0xa110000+ch*0x400000+offsets[i],held[i]))goto fail;
 probe_report.phase=5;
 if(access_word(3,0x0aaa0000,&v))goto fail;
 if(v){probe_report.status=3;return 0;}
 if(access_word(3,0x0aaa0132,&v))goto fail;
 probe_report.phase=6;probe_report.status=2;return 0;
fail:probe_report.status=0x100+probe_report.phase;return -1;
}
