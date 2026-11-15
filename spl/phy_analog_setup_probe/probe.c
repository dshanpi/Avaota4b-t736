// SPDX-License-Identifier: GPL-2.0-only
/* Bounded SRAM analog-configuration experiment. Thirteen named 16-bit CSR
 * writes only; no training, ZCal trigger, DFI handoff, program RAM, MR command,
 * memory content access or storage access. Values are interface parameters,
 * not vendor instruction streams. Host owns recovery from a stalled bus.
 */
#include <stdint.h>
#define CAP 192u
#define NSTATIC 36u
#define NWRITE 13u
#define EXPECTED_EVENTS 179u
struct event { uint32_t op, address, value; };
struct report {
 uint32_t magic,version,status,sctlr,attempted,completed,writes,uart_error;
 uint32_t phase,pre_ticks,reset_ticks,settle_ticks;
 struct event events[CAP];
};
_Static_assert(sizeof(struct report)==2352,"report ABI");
_Static_assert(EXPECTED_EVENTS<=CAP,"event capacity");
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
/* Exact initial observations: t736-phy-static-read-sram-hardware-20261114.json. */
static const uint32_t initial[NSTATIC]={
 0x2c18,0x3c1f,0x124,0xbce,1,0,1,2,3,3,0x808,9,0x40,2,3,0,
 0x400,0x400,0x400,0x400,0,0,0,0,0,0,0,0,0,0,1,0,1,0,0,0
};
static const uint32_t extra[3]={0x0a940008,0x0a940116,0x0a940120};
static const uint32_t write_address[NWRITE]={
 0x0aaa0132,0x0a9400c0,0x0a940094,0x0a940096,0x0a940110,
 0x0a940098,0x0a94009a,0x0a940000,0x0a940604,0x0a940650,
 0x0a940008,0x0a940116,0x0a940120
};
/* ZCalClkInfo is decimal MHz: 450 == 0x1c2, not hexadecimal 0x450. */
static const uint32_t write_value[NWRITE]={9,2,0x402,0x123e,0x101,0x2a8b,0,2,0x129,1,450,0,1};

#ifdef T736_PHY_ANALOG_SETUP_NATIVE
uint32_t test_read(uint32_t address,unsigned int width);
void test_write(uint32_t address,uint32_t value,unsigned int width);
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

static int allowed_read(unsigned int op,uint32_t a){
 uint32_t i,ch;
 if(op==1){
  for(i=0;i<12;i++)if(a==ca[i])return 1;
  for(ch=0;ch<2;ch++)for(i=0;i<11;i++)if(a==0xa110000+ch*0x400000+offsets[i])return 1;
 }else if(op==3){
  if(a==0x0aaa0000 || a==0x0aaa0132 || a==0x0aa80100)return 1;
  for(i=0;i<NSTATIC;i++)if(a==phy_static[i])return 1;
  for(i=0;i<3;i++)if(a==extra[i])return 1;
 }
 return 0;
}

/* op4 means 16-bit write. No 32-bit write operation exists. */
static int access(unsigned int op,uint32_t a,uint32_t *v,uint32_t clock_before){
 uint32_t i=probe_report.completed,j;
 if(i>=CAP)return -1;
 if(op==4){
  if(*v>0xffffu)return -1;
  for(j=0;j<NWRITE;j++)if(a==write_address[j])break;
  if(j==NWRITE || *v!=(j==10?((clock_before&0xf800u)|450u):write_value[j]))return -1;
 }else if(!allowed_read(op,a))return -1;
 probe_report.events[i].op=op;probe_report.events[i].address=a;
 probe_report.attempted=i+1;
 if(progress(op,a)){probe_report.uart_error=1;return -1;}
 if(op==4){
  probe_report.events[i].value=*v;probe_report.writes++;
#ifdef T736_PHY_ANALOG_SETUP_NATIVE
  test_write(a,*v,16);
#else
  *(volatile uint16_t *)(uintptr_t)a=(uint16_t)*v;
  __asm__ volatile("dsb sy":::"memory");
#endif
 }else{
#ifdef T736_PHY_ANALOG_SETUP_NATIVE
  *v=test_read(a,op==3?16u:32u);
#else
  *v=op==3?*(volatile uint16_t *)(uintptr_t)a:*(volatile uint32_t *)(uintptr_t)a;
  __asm__ volatile("dsb sy":::"memory");
#endif
  probe_report.events[i].value=*v;
 }
 probe_report.completed=i+1;return 0;
}
static int sample(unsigned int op,uint32_t a,uint32_t *v){return access(op,a,v,0);}
static int expect(unsigned int op,uint32_t a,uint32_t expected){uint32_t v;return sample(op,a,&v)||v!=expected?-1:0;}
static int set(unsigned int index,uint32_t clock_before){uint32_t v;if(index>=NWRITE)return -1;v=index==10?((clock_before&0xf800u)|450u):write_value[index];return access(4,write_address[index],&v,clock_before)||expect(3,write_address[index],v)?-1:0;}
static int delay(uint32_t us,volatile uint32_t *ticks){uint32_t begin=counter(),n;for(n=0;n<1000000;n++){*ticks=counter()-begin;if(*ticks>=us*24u)return 0;}return -1;}
static int context_and_controllers(void){
 uint32_t i,ch;
 for(i=0;i<12;i++)if(expect(1,ca[i],cv[i]))return -1;
 for(ch=0;ch<2;ch++)for(i=0;i<11;i++)if(expect(1,0xa110000+ch*0x400000+offsets[i],held[i]))return -1;
 return 0;
}
static int outer(uint32_t micro){return expect(3,0x0aaa0000,0)||expect(3,0x0aaa0132,micro)?-1:0;}
static uint32_t final_static(uint32_t i){uint32_t j;for(j=0;j<NWRITE;j++)if(phy_static[i]==write_address[j])return write_value[j];return initial[i];}

int probe_main(void){
 uint32_t i,v,clock_before=0,sctlr=0xc50838;volatile uint32_t *p=(volatile uint32_t *)&probe_report;
 for(i=0;i<sizeof(probe_report)/4u;i++)p[i]=0;
 probe_report.magic=0x54375041;probe_report.version=1;probe_report.status=1;probe_report.phase=1;
#ifndef T736_PHY_ANALOG_SETUP_NATIVE
 __asm__ volatile("mrc p15, 0, %0, c1, c0, 0":"=r"(sctlr));
#endif
 probe_report.sctlr=sctlr;if(sctlr&0x3005)goto fail;
 if(context_and_controllers())goto fail;
 probe_report.phase=2;if(outer(1)||expect(3,0x0aa80100,3))goto fail;
 probe_report.phase=3;
 for(i=0;i<NSTATIC;i++)if(expect(3,phy_static[i],initial[i]))goto fail;
 for(i=0;i<3;i++){if(sample(3,extra[i],&v))goto fail;if(i==0)clock_before=v;}
 /* Establish a live timestamp source before any configuration write. */
 probe_report.phase=4;if(delay(10,&probe_report.pre_ticks))goto fail;
 probe_report.phase=5;if(set(0,clock_before)||expect(3,0x0aaa0000,0)||set(1,clock_before))goto fail;
 /* Reset-low dwell is measured. It does not prove CKE pin state or satisfy
  * the later reset-high tINIT3 / CKE tINIT5 requirements. */
 probe_report.phase=6;if(delay(200,&probe_report.reset_ticks))goto fail;
 probe_report.phase=7;for(i=2;i<NWRITE;i++)if(set(i,clock_before))goto fail;
 /* Analog settle only; this is not a PLL lock or ZCal completion test. */
 probe_report.phase=8;if(delay(100,&probe_report.settle_ticks))goto fail;
 probe_report.phase=9;
 if(context_and_controllers()||outer(9)||expect(3,0x0aa80100,3))goto fail;
 for(i=0;i<NSTATIC;i++)if(expect(3,phy_static[i],final_static(i)))goto fail;
 if(expect(3,extra[0],(clock_before&0xf800u)|450u)||expect(3,extra[1],0)||expect(3,extra[2],1))goto fail;
 if(probe_report.completed!=EXPECTED_EVENTS || probe_report.writes!=NWRITE)goto fail;
 probe_report.phase=10;probe_report.status=2;return 0;
fail:probe_report.status=0x100+probe_report.phase;return -1;
}
