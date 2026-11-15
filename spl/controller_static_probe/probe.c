// SPDX-License-Identifier: GPL-2.0-only
/* Fixed two-controller static-field experiment, never DRAM initialization.
 * Independent callback whitelist constrains the component's 22 word writes.
 */
#include "../lpddr4/phy/controller_static.h"
#define CAP 192u
#define MIN_EVENTS 170u
#define MAX_EVENTS 182u
struct event { uint32_t op,address,value; };
struct report {
 volatile uint32_t magic,version,status,sctlr,attempted,completed,writes,uart_error;
 volatile uint32_t phase,pre_ticks,settle_ticks,result;
 volatile struct event events[CAP];
 struct t736_init_report component;
};
_Static_assert(sizeof(struct t736_init_report)==164,"component ABI");
_Static_assert(sizeof(struct report)==2516,"report ABI");
_Static_assert(MAX_EVENTS<=CAP,"event capacity");
struct report probe_report __attribute__((section(".report")));
static uint32_t before[2][7],before_valid;
static const uint32_t ca[12]={0x02002020,0x02002c00,0x02002c0c,0x0a020010,0x0a020014,0x07010250,0x08020000,0x08020020,0x0a02021c,0x0a020220,0x0a020224,0x0a020228};
static const uint32_t cv[12]={0xf9004a00,0x80000003,0x10001,0x273f,2,1,1,24000000,0,0,0,0};
static const uint32_t co[13]={0,0x14,0x90,0x180,0x510,0x508,0x500,0x518,0xc80,0xc84,0x514,0xca4,0x80};
static const uint32_t pa[6]={0x0aaa0000,0x0aaa0132,0x0aa80100,0x0a9400c0,0x0a940620,0x0a940622};
static const uint32_t config[7]={0x0a110d00,0x0a110d04,0x0a100500,0x0a100504,0x0a100508,0x0a10050c,0x0a100d00};
static const uint32_t masks[7]={0x03ff1fff,0x03ff0000,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,1};
/* Independently audited scalars, not copied from the component's output. */
static const uint32_t values[7]={0x0001036f,0x00580000,0x00fc003f,0x00330008,0x00040072,0x0024001d,1};
static const uint32_t write_order[22]={
 0x0a110c80,0x0a510c80,0x0a110ca4,0x0a510ca4,
 0x0a110d00,0x0a110d04,0x0a100500,0x0a100504,0x0a100508,0x0a10050c,0x0a100d00,
 0x0a510d00,0x0a510d04,0x0a500500,0x0a500504,0x0a500508,0x0a50050c,0x0a500d00,
 0x0a110ca4,0x0a510ca4,0x0a110c80,0x0a510c80
};
#ifdef T736_CONTROLLER_STATIC_NATIVE
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
static int allowed_read(unsigned int op,uint32_t a){uint32_t i,ch;
 if(op==1){
  for(i=0;i<12;i++)if(a==ca[i])return 1;
  for(ch=0;ch<2;ch++){
   for(i=0;i<13;i++)if(a==0xa110000+ch*0x400000+co[i])return 1;
   for(i=0;i<7;i++)if(a==config[i]+ch*0x400000)return 1;
  }
 }else if(op==3){for(i=0;i<6;i++)if(a==pa[i])return 1;}
 return 0;
}
static int allowed_write(uint32_t a,uint32_t v){uint32_t n=probe_report.writes,ch,i,want;
 if(probe_report.phase!=2 || n>=22 || before_valid!=0x3fffu || a!=write_order[n])return 0;
 if(n<2 || n==18 || n==19)want=0;
 else if(n<4 || n>=20)want=1;
 else {ch=n>=11u;i=n-4u-ch*7u;want=(before[ch][i]&~masks[i])|values[i];}
 return v==want;
}
static int access_mmio(unsigned int op,uint32_t a,uint32_t *v){uint32_t n=probe_report.completed,ch,i;
 if(n>=CAP || (op==2?!allowed_write(a,*v):!allowed_read(op,a)))return -1;
 probe_report.events[n].op=op;probe_report.events[n].address=a;probe_report.attempted=n+1;
 if(progress(op,a)){probe_report.uart_error=1;return -1;}
 if(op==2){
  probe_report.events[n].value=*v;probe_report.writes++;
#ifdef T736_CONTROLLER_STATIC_NATIVE
  test_write(a,*v);
#else
  *(volatile uint32_t *)(uintptr_t)a=*v;
#endif
 }else{
#ifdef T736_CONTROLLER_STATIC_NATIVE
  *v=test_read(a,op==3?16u:32u);
#else
  *v=op==3?*(volatile uint16_t *)(uintptr_t)a:*(volatile uint32_t *)(uintptr_t)a;
#endif
  probe_report.events[n].value=*v;
  if(op==1 && !probe_report.writes)for(ch=0;ch<2;ch++)for(i=0;i<7;i++)if(a==config[i]+ch*0x400000){before[ch][i]=*v;before_valid|=1u<<(ch*7u+i);}
 }
#ifndef T736_CONTROLLER_STATIC_NATIVE
 __asm__ volatile("dsb sy":::"memory");
#endif
 probe_report.completed=n+1;return 0;
}
static int read32(void *ctx,uint32_t a,uint32_t *v){(void)ctx;return access_mmio(1,a,v);}
static int read16(void *ctx,uint32_t a,uint16_t *v){uint32_t word=0;int rc;(void)ctx;rc=access_mmio(3,a,&word);if(!rc)*v=(uint16_t)word;return rc;}
static int write32(void *ctx,uint32_t a,uint32_t v){(void)ctx;return access_mmio(2,a,&v);}
static int measured_wait(uint32_t us,volatile uint32_t *ticks){uint32_t start=counter(),n;
 for(n=0;n<1000000;n++){*ticks=counter()-start;if(*ticks>=us*24u)return 0;}return -1;
}
static int delay_us(void *ctx,uint32_t us){volatile uint32_t poll_ticks=0;(void)ctx;
 if(us==20)return measured_wait(us,&probe_report.settle_ticks);
 if(us==1)return measured_wait(us,&poll_ticks);
 return -1;
}
int probe_main(void){
 uint32_t i,ch,v=0,sctlr=0xc50838;int rc;volatile uint32_t *clear=(volatile uint32_t *)&probe_report;
 const struct t736_phy_io io={0,read16,0,read32,write32,delay_us};
 for(i=0;i<sizeof(probe_report)/4u;i++)clear[i]=0;
 before_valid=0;for(ch=0;ch<2;ch++)for(i=0;i<7;i++)before[ch][i]=0;
 probe_report.magic=0x54374353;probe_report.version=1;probe_report.status=1;probe_report.phase=1;
#ifndef T736_CONTROLLER_STATIC_NATIVE
 __asm__ volatile("mrc p15, 0, %0, c1, c0, 0":"=r"(sctlr));
#endif
 probe_report.sctlr=sctlr;if(sctlr&0x3005)goto fail;
 for(i=0;i<6;i++)if(access_mmio(1,ca[i],&v)||v!=cv[i])goto fail;
 if(measured_wait(10,&probe_report.pre_ticks))goto fail;
 probe_report.phase=2;rc=t736_init_apply_static(&io,1800,450,1,3,&probe_report.component);
 probe_report.result=(uint32_t)rc;
 if(rc!=T736_INIT_CONFIGURED_ONLY || !probe_report.component.final_valid || probe_report.component.stage!=8 ||
    probe_report.component.swctl_maybe_open || probe_report.component.static_maybe_open || probe_report.component.configured_channels!=3 ||
    probe_report.writes!=22 || probe_report.component.writes_completed!=22 || probe_report.component.writes_attempted!=22 ||
    probe_report.component.reads_completed!=probe_report.component.reads_attempted ||
    probe_report.completed!=6u+probe_report.component.reads_completed+22u ||
    probe_report.completed<MIN_EVENTS || probe_report.completed>MAX_EVENTS ||
    probe_report.component.waited_us!=20u+probe_report.completed-MIN_EVENTS || probe_report.settle_ticks<480u)goto fail;
 probe_report.phase=3;probe_report.status=2;return 0;
fail:probe_report.status=0x100+probe_report.phase;return -1;
}
