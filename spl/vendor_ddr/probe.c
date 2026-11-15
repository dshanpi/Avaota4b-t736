// SPDX-License-Identifier: GPL-2.0-only
/* Source FEL host for the explicitly authorized sun60iw2 libdram.
 * References: SyterKit 7f539e17 dram-sun60iw2.c and init_dram/main.c;
 * local Tina arisc sun60iw2 SMC conditional workaround and dram parameter ABI.
 */
#include "platform.h"
#include "twi.h"
#include "axp8191.h"
#include <stdarg.h>
#include <stddef.h>
#include "params.h"
#ifdef T736_PERIPHERAL_POWER
#include "board_power.h"
#endif

#define UART 0x02500000u
#define TWI 0x07083000u
volatile uint32_t probe_report[128] __attribute__((section(".report")));
/* 32 seeds + space for the library's extended training output, not just 32. */
static uint32_t parameters[160] = { T736_DRAM_SEEDS };
static uint32_t deadline;
extern int init_DRAM(int type, void *parameters);

static void tx(char c) {
 for (uint32_t n=0;n<T736_POLL_LIMIT;n++)
  if(t736_read32(UART+0x14)&0x20) { t736_write32(UART,(uint8_t)c); return; }
}
static void emit(char c) { if(c=='\n')tx('\r'); tx(c); }
static void string(const char *s) { while(*s)emit(*s++); }
static void number(uint32_t v,unsigned base,unsigned width,char pad) {
 char b[32]; unsigned n=0; do {b[n++]="0123456789abcdef"[v%base];v/=base;}while(v);
 while(width>n){emit(pad);width--;} while(n)emit(b[--n]);
}
int printf(const char *fmt,...) {
 va_list ap; va_start(ap,fmt);
 while(*fmt) {
  if(*fmt!='%'){emit(*fmt++);continue;}
  fmt++; unsigned width=0; char pad=' ';
  if(*fmt=='0'){pad='0';fmt++;}
  while(*fmt>='0'&&*fmt<='9')width=width*10+(*fmt++-'0');
  if(*fmt=='l')fmt++;
  switch(*fmt++) {
   case 's':string(va_arg(ap,const char*));break;
   case 'd':{int v=va_arg(ap,int);if(v<0){emit('-');number(0u-(uint32_t)v,10,width,pad);}else number(v,10,width,pad);break;}
   case 'u':number(va_arg(ap,unsigned),10,width,pad);break;
   case 'x':number(va_arg(ap,unsigned),16,width,pad);break;
   case 'c':emit(va_arg(ap,int));break;
   case '%':emit('%');break;
   default:string("[FORMAT]");break;
  }
 }
 va_end(ap);return 0;
}
void *memcpy(void *d,const void *s,size_t n) {unsigned char *a=d;const unsigned char *b=s;while(n--)*a++=*b++;return d;}
void *memset(void *d,int c,size_t n) {unsigned char *a=d;while(n--)*a++=c;return d;}
static void fail(uint32_t code) __attribute__((noreturn));
static void fail(uint32_t code) {probe_report[2]=code;printf("HOST FAIL %x\n",code);__asm__ volatile("dsb sy");for(;;)__asm__ volatile("wfi");}
int raise(int signal) { (void)signal; fail(0xe003); }
void udelay(unsigned int us) {
 if(t736_delay_us(us))fail(0xe001);
 if((int32_t)(t736_counter_low()-deadline)>0)fail(0xe002);
}
static uint32_t br(void *ctx,uint32_t reg){(void)ctx;return t736_read32(TWI+reg);}
static void bw(void *ctx,uint32_t reg,uint32_t val){(void)ctx;t736_write32(TWI+reg,val);}
static uint32_t ticks(void *ctx){(void)ctx;return t736_counter_low();}
static struct t736_twi bus={0,br,bw,ticks,240000,T736_POLL_LIMIT,0,0};
static int pr(void *ctx,uint8_t reg,uint8_t *v){(void)ctx;return t736_twi_read8(&bus,0x36,reg,v);}
static int pw(void *ctx,uint8_t reg,uint8_t v){(void)ctx;probe_report[7]++;return t736_twi_write8(&bus,0x36,reg,v);}
static const struct t736_axp_bus pmic={0,pr,pw};
static int equal(const char *a,const char *b){while(*a&&*a==*b){a++;b++;}return *a==*b;}
int set_ddr_voltage_ext(char *name,int mv,int on) {
 enum t736_axp_rail rail;unsigned effective;
 if(!on)fail(0xe010);
 if(equal(name,"dcdc8")&&mv==1100){rail=T736_AXP_DCDC8;effective=1100;}
 /* AXP8191 ELDO1 has 25mV steps; vendor driver floors 660 to 650mV.
  * Preserve that documented behavior and report both requested/effective. */
 else if(equal(name,"eldo1")&&mv==660){rail=T736_AXP_ELDO1;effective=650;}
 else fail(0xe011);
 int rc=t736_axp_set_rail(&pmic,rail,effective);
 printf("DDR rail %s requested=%d effective=%u rc=%d\n",name,mv,effective,rc);
 if(rc)fail(0xe012);
 udelay(1000);return 0;
}
void sunxi_smc_en_with_glitch_workaround(void) {
 /* SDK: workaround only for secure-fused chips. Refuse unreviewed secure use. */
 uint32_t secure=t736_read32(0x030060a0)&1;
 probe_report[8]=secure;
 if(secure)fail(0xe020);
}
static int memory_test(void) {
 static const uint32_t bases[]={0x40000000,0x48000000,0x60000000,0x80000000,0xa0000000,0xc0000000,0xe0000000,0xfff00000};
 static const uint32_t salts[]={0,0xffffffff,0xaaaaaaaa,0x55555555};
 /* Write all eight 64KiB windows before reading, catching cross-window aliases. */
 for(unsigned s=0;s<4;s++) {
  for(unsigned j=0;j<8;j++)for(unsigned i=0;i<16384;i++)
   ((volatile uint32_t*)bases[j])[i]=(bases[j]+i*4)^salts[s];
  __asm__ volatile("dsb sy" ::: "memory");
  for(unsigned j=0;j<8;j++)for(unsigned i=0;i<16384;i++) {
   uint32_t want=(bases[j]+i*4)^salts[s],got=((volatile uint32_t*)bases[j])[i];
   if(got!=want){probe_report[9]=bases[j]+i*4;probe_report[10]=want;probe_report[11]=got;return -1;}
   probe_report[5]++;
  }
 }
 return 0;
}
int probe_main(void) {
 for(unsigned i=0;i<128;i++)probe_report[i]=0;
 probe_report[0]=0x54374444;probe_report[1]=1;probe_report[2]=1;
 t736_clrset32(0x02000104,0xff0,0x220);
 t736_clrset32(0x02002538,0x0700001f,0);
 t736_clrset32(0x02002e00,0,0x10001);
 t736_write32(UART+4,0);t736_write32(UART+12,0x80);t736_write32(UART,13);
 t736_write32(UART+4,0);t736_write32(UART+12,3);t736_write32(UART+8,7);
 printf("T736 source SRAM host + authorized libdram v1\n");
 if(t736_timer_init())fail(0xe030);
 deadline=t736_counter_low()+24000000u*25;
 t736_clrset32(0x0701019c,0,0x10001);
 if((br(0,TWI_DRV_CTRL)&0x80000001)||(br(0,TWI_PMU_CTRL)&0xff))fail(0xe031);
 t736_clrset32(0x07010010,0x0700001f,4u<<24);t736_clrset32(0x07025000,0xff,0x22);
 if(t736_twi_init(&bus))fail(0xe032);
 uint8_t id=0;if(pr(0,0x0e,&id)||id!=3)fail(0xe033);
 /* LPDDR4 VDD1 is board-fixed 1.8V, in addition to the two library hooks. */
 if(t736_axp_set_rail(&pmic,T736_AXP_ALDO4,1800))fail(0xe034);
 uint32_t version=t736_read32(0x03000024)&7;probe_report[12]=version;
 if(version==1)t736_write32(0x02000340,0x01155550);
 if(version<2){uint32_t v=version?0xa7060025:0xa7070025;t736_write32(0x08000144,v);t736_write32(0x08000144,v);}
 /* Select fixed 24MHz DDR PLL reference. Library owns DDR clocks/reset/DFI. */
 t736_clrset32(0x02002020,0,1u<<24);
 printf("init_DRAM begin seed tpr3=%x tpr13=%x\n",parameters[25],parameters[30]);
 probe_report[2]=2;uint32_t begin=t736_counter_low();
 int mb=init_DRAM(0,parameters);
 probe_report[3]=mb;probe_report[4]=t736_counter_low()-begin;
 for(unsigned i=0;i<96;i++)probe_report[32+i]=parameters[i];
 printf("init_DRAM returned %d MiB, ticks=%u\n",mb,probe_report[4]);
 if(mb!=8192){probe_report[2]=0xe040;return -1;}
 probe_report[2]=3;
 if(memory_test()){printf("MEM FAIL addr=%x want=%x got=%x\n",probe_report[9],probe_report[10],probe_report[11]);probe_report[2]=0xe041;return -1;}
 probe_report[2]=4;printf("MEM PASS: 8 x 64KiB windows, 4 address/data patterns, %u words checked; upper physical RAM pending\n",probe_report[5]);
#ifdef T736_PERIPHERAL_POWER
 struct t736_board_power_report power = {0};
 int power_rc = t736_board_power_enable(&pmic, &power);
 printf("BOARD power rc=%d DCDC1 en=%x voltage=%x DC1SW1=%x->%x DLDO2=%x->%x LDO_EN3=%x->%x\n",
        power_rc,power.dcdc_enable,power.dcdc1_voltage,power.switch_before,
        power.switch_after,power.dldo2_before,power.dldo2_after,
        power.ldo_before,power.ldo_after);
 if(power_rc)printf("BOARD peripheral supply setup failed; console boot continues\n");
 else udelay(10000);
#endif
 __asm__ volatile("dsb sy" ::: "memory");return 0;
}
