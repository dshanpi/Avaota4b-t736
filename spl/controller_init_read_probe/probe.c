// SPDX-License-Identifier: GPL-2.0-only
/* Fixed SRAM observation of controller initialization, MR and timing CSRs.
 * UART TX is the only peripheral write. No static unlock, MR launch, reset
 * transition, PHY trigger, program-memory or DRAM-content access exists.
 */
#include <stdint.h>
#define CAP 160u
#define NCTL 8u
#define NTIMING 12u
#define EXPECTED_READS 120u
struct event { uint32_t op,address,value; };
struct report {
 uint32_t magic,version,status,sctlr,attempted,completed,writes,uart_error;
 uint32_t phase,reserved[3];
 struct event events[CAP];
};
_Static_assert(sizeof(struct report)==1968,"report ABI");
_Static_assert(EXPECTED_READS==2u*(12u+22u+6u)+2u*(NCTL+NTIMING),"event count");
_Static_assert(EXPECTED_READS<=CAP,"event capacity");
volatile struct report probe_report __attribute__((section(".report")));
static const uint32_t ca[12]={
 0x02002020,0x02002c00,0x02002c0c,0x0a020010,0x0a020014,0x07010250,
 0x08020000,0x08020020,0x0a02021c,0x0a020220,0x0a020224,0x0a020228
};
static const uint32_t cv[12]={0xf9004a00,0x80000003,0x10001,0x273f,2,1,1,24000000,0,0,0,0};
static const uint32_t held_offsets[11]={0,0x14,0x90,0x180,0x510,0x508,0x500,0x518,0xc80,0xc84,0x514};
static const uint32_t held[11]={0x03081002,0,0,0x800,0x10014,0xc0000000,0,0x25000000,1,1,0};
/* Mux ownership precedes every internal-PHY guard group. */
static const uint32_t phy_guard[6]={0x0aaa0000,0x0aaa0132,0x0aa80100,0x0a9400c0,0x0a940620,0x0a940622};
static const uint32_t phy_expected[6]={0,9,3,2,0,0};
static const uint32_t ctl_offsets[NCTL]={0xca4,0xd00,0xd04,0x80,0x84,0x90,0x94,0x98};
static const uint32_t timing_offsets[NTIMING]={0x500,0x504,0x508,0x50c,0x78,0x80,0xd00,0xd04,0xd08,0xd0c,0x800,0x804};

#ifdef T736_CONTROLLER_INIT_READ_NATIVE
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
  if(a&3u)return 0;
  for(i=0;i<12;i++)if(a==ca[i])return 1;
  for(ch=0;ch<2;ch++){
   for(i=0;i<11;i++)if(a==0x0a110000+ch*0x400000+held_offsets[i])return 1;
   for(i=0;i<NCTL;i++)if(a==0x0a110000+ch*0x400000+ctl_offsets[i])return 1;
   for(i=0;i<NTIMING;i++)if(a==0x0a100000+ch*0x400000+timing_offsets[i])return 1;
  }
 }else if(op==3){
  if(a&1u)return 0;
  for(i=0;i<6;i++)if(a==phy_guard[i])return 1;
 }
 return 0;
}
static int sample(unsigned int op,uint32_t a,uint32_t *v){
 uint32_t i=probe_report.completed;
 if(i>=CAP || !allowed_read(op,a))return -1;
 probe_report.events[i].op=op;probe_report.events[i].address=a;
 probe_report.attempted=i+1;
 /* A drained UART marker is preserved before any possibly stalled MMIO. */
 if(progress(op,a)){probe_report.uart_error=1;return -1;}
#ifdef T736_CONTROLLER_INIT_READ_NATIVE
 *v=test_read(a,op==3?16u:32u);
#else
 *v=op==3?*(volatile uint16_t *)(uintptr_t)a:*(volatile uint32_t *)(uintptr_t)a;
 __asm__ volatile("dsb sy":::"memory");
#endif
 probe_report.events[i].value=*v;probe_report.completed=i+1;return 0;
}
static int expect(unsigned int op,uint32_t a,uint32_t expected){uint32_t v;return sample(op,a,&v)||v!=expected?-1:0;}
static int context_and_controllers(void){
 uint32_t i,ch;
 for(i=0;i<12;i++)if(expect(1,ca[i],cv[i]))return -1;
 for(ch=0;ch<2;ch++)for(i=0;i<11;i++)if(expect(1,0x0a110000+ch*0x400000+held_offsets[i],held[i]))return -1;
 return 0;
}
static int phy_guards(void){uint32_t i;for(i=0;i<6;i++)if(expect(3,phy_guard[i],phy_expected[i]))return -1;return 0;}
int probe_main(void){
 uint32_t i,ch,v,sctlr=0xc50838;volatile uint32_t *p=(volatile uint32_t *)&probe_report;
 for(i=0;i<sizeof(probe_report)/4u;i++)p[i]=0;
 probe_report.magic=0x54374349;probe_report.version=1;probe_report.status=1;probe_report.phase=1;
#ifndef T736_CONTROLLER_INIT_READ_NATIVE
 __asm__ volatile("mrc p15, 0, %0, c1, c0, 0":"=r"(sctlr));
#endif
 probe_report.sctlr=sctlr;if(sctlr&0x3005)goto fail;
 if(context_and_controllers())goto fail;
 probe_report.phase=2;if(phy_guards())goto fail;
 probe_report.phase=3;
 for(ch=0;ch<2;ch++){
  for(i=0;i<NCTL;i++)if(sample(1,0x0a110000+ch*0x400000+ctl_offsets[i],&v))goto fail;
  for(i=0;i<NTIMING;i++)if(sample(1,0x0a100000+ch*0x400000+timing_offsets[i],&v))goto fail;
 }
 probe_report.phase=4;if(context_and_controllers())goto fail;
 probe_report.phase=5;if(phy_guards())goto fail;
 if(probe_report.completed!=EXPECTED_READS || probe_report.writes)goto fail;
 probe_report.phase=6;probe_report.status=2;return 0;
fail:probe_report.status=0x100+probe_report.phase;return -1;
}
