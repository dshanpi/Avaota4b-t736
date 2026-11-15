// SPDX-License-Identifier: GPL-2.0-only
/* Single fixed rollback from the observed failed INITTMG1 checkpoint.
 * No retry, INITTMG1 write, PHY write, MR command or DRAM content access.
 */
#include <stdint.h>
#define CAP 160u
struct event {uint32_t op,address,value;};
struct report {
 uint32_t magic,version,status,sctlr,attempted,completed,writes,uart_error;
 uint32_t phase,pre_ticks,settle_ticks,poll_waits;
 struct event events[CAP];
};
_Static_assert(sizeof(struct report)==1968,"report ABI");
volatile struct report probe_report __attribute__((section(".report")));
static const uint32_t ca[12]={0x02002020,0x02002c00,0x02002c0c,0x0a020010,0x0a020014,0x07010250,0x08020000,0x08020020,0x0a02021c,0x0a020220,0x0a020224,0x0a020228};
static const uint32_t cv[12]={0xf9004a00,0x80000003,0x10001,0x273f,2,1,1,24000000,0,0,0,0};
static const uint32_t co[13]={0,0x14,0x90,0x180,0x510,0x508,0x500,0x518,0xc80,0xc84,0x514,0xca4,0x80};
static const uint32_t closed[13]={0x03081002,0,0,0x800,0x10014,0xc0000000,0,0x25000000,1,1,0,0,0x30};
static const uint32_t pa[6]={0x0aaa0000,0x0aaa0132,0x0aa80100,0x0a9400c0,0x0a940620,0x0a940622};
static const uint32_t pv[6]={0,1,3,2,0,0};
static const uint32_t config[7]={0x0a110d00,0x0a110d04,0x0a100500,0x0a100504,0x0a100508,0x0a10050c,0x0a100d00};
static const uint32_t baseline[7]={0x2004e,0,0x510,0,0,0,0};
static const uint32_t wa[5]={0x0a110d00,0x0a110ca4,0x0a510ca4,0x0a110c80,0x0a510c80};
static const uint32_t wv[5]={0x2004e,0,0,1,1};
#ifdef T736_CONTROLLER_STATIC_ROLLBACK_NATIVE
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
static int allowed(unsigned int op,uint32_t a,uint32_t value){uint32_t i,ch;
 if(op==2){i=probe_report.writes;return probe_report.phase==2 && i<5 && a==wa[i] && value==wv[i];}
 if(op==1){
  for(i=0;i<12;i++)if(a==ca[i])return 1;
  for(ch=0;ch<2;ch++){
   for(i=0;i<13;i++)if(a==0xa110000+ch*0x400000+co[i])return 1;
   for(i=0;i<7;i++)if(a==config[i]+ch*0x400000)return 1;
  }
 }else if(op==3){for(i=0;i<6;i++)if(a==pa[i])return 1;}
 return 0;
}
static int access_mmio(unsigned int op,uint32_t a,uint32_t *v){uint32_t n=probe_report.completed;
 if(n>=CAP || !allowed(op,a,op==2?*v:0))return -1;
 probe_report.events[n].op=op;probe_report.events[n].address=a;probe_report.attempted=n+1;
 if(progress(op,a)){probe_report.uart_error=1;return -1;}
 if(op==2){probe_report.events[n].value=*v;probe_report.writes++;
#ifdef T736_CONTROLLER_STATIC_ROLLBACK_NATIVE
  test_write(a,*v);
#else
  *(volatile uint32_t *)(uintptr_t)a=*v;
#endif
 }else{
#ifdef T736_CONTROLLER_STATIC_ROLLBACK_NATIVE
  *v=test_read(a,op==3?16u:32u);
#else
  *v=op==3?*(volatile uint16_t *)(uintptr_t)a:*(volatile uint32_t *)(uintptr_t)a;
#endif
  probe_report.events[n].value=*v;
 }
#ifndef T736_CONTROLLER_STATIC_ROLLBACK_NATIVE
 __asm__ volatile("dsb sy":::"memory");
#endif
 probe_report.completed=n+1;return 0;
}
static int expect(unsigned int op,uint32_t a,uint32_t want){uint32_t v;return access_mmio(op,a,&v)||v!=want?-1:0;}
static int set(uint32_t index){uint32_t v;if(index>=5)return -1;v=wv[index];return access_mmio(2,wa[index],&v)||expect(1,wa[index],v)?-1:0;}
static int guard(int restored){uint32_t ch,i,want;
 for(i=0;i<12;i++)if(expect(1,ca[i],cv[i]))return -1;
 for(ch=0;ch<2;ch++)for(i=0;i<13;i++){
  want=closed[i];if(!restored){if(i==8 || i==9)want=0;if(i==11)want=1;}
  if(expect(1,0xa110000+ch*0x400000+co[i],want))return -1;
 }
 for(i=0;i<6;i++)if(expect(3,pa[i],pv[i]))return -1;
 for(ch=0;ch<2;ch++)for(i=0;i<7;i++){
  want=(!restored && !ch && !i)?0x1036f:baseline[i];
  if(expect(1,config[i]+ch*0x400000,want))return -1;
 }
 return 0;
}
static int delay(uint32_t us,volatile uint32_t *ticks){uint32_t start=counter(),n;
 for(n=0;n<1000000;n++){*ticks=counter()-start;if(*ticks>=us*24u)return 0;}return -1;
}
static int commit_poll(uint32_t channel){uint32_t i,v;volatile uint32_t ticks=0;
 for(i=0;i<4;i++){
  if(access_mmio(1,0x0a110c84+channel*0x400000,&v))return -1;
  if(v==1)return 0;
  if(v || i==3 || delay(1,&ticks))return -1;
  probe_report.poll_waits++;
 }
 return -1;
}
int probe_main(void){uint32_t i,sctlr=0xc50838;volatile uint32_t *clear=(volatile uint32_t *)&probe_report;
 for(i=0;i<sizeof(probe_report)/4u;i++)clear[i]=0;
 probe_report.magic=0x54375242;probe_report.version=1;probe_report.status=1;probe_report.phase=1;
#ifndef T736_CONTROLLER_STATIC_ROLLBACK_NATIVE
 __asm__ volatile("mrc p15, 0, %0, c1, c0, 0":"=r"(sctlr));
#endif
 probe_report.sctlr=sctlr;if(sctlr&0x3005 || guard(0) || delay(10,&probe_report.pre_ticks))goto fail;
 probe_report.phase=2;
 for(i=0;i<5;i++){if(set(i))goto fail;if(i>=3 && commit_poll(i-3))goto fail;}
 probe_report.phase=3;if(delay(20,&probe_report.settle_ticks))goto fail;
 probe_report.phase=4;if(guard(1))goto fail;
 if(probe_report.writes!=5 || probe_report.poll_waits>6 || probe_report.completed!=128u+probe_report.poll_waits)goto fail;
 probe_report.phase=5;probe_report.status=2;return 0;
fail:probe_report.status=0x100+probe_report.phase;return -1;
}
