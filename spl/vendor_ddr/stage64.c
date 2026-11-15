// SPDX-License-Identifier: GPL-2.0-only
#include <stdint.h>
static void tx(char c){volatile uint32_t *u=(void*)0x02500000;for(unsigned n=0;n<1000000;n++)if(u[5]&32){u[0]=c;return;}}
static void puts(const char *s){while(*s){if(*s=='\n')tx('\r');tx(*s++);}}
static void hex(uint64_t x){for(int s=60;s>=0;s-=4)tx("0123456789abcdef"[(x>>s)&15]);}
static void halt(void){for(;;)__asm__ volatile("wfi");}
void stage_main(void){
 uint64_t el,sctlr;__asm__ volatile("mrs %0,CurrentEL":"=r"(el));
 puts("T736 A64 source handoff EL=");hex(el);puts("\n");if(el!=12)halt();
 __asm__ volatile("mrs %0,sctlr_el3":"=r"(sctlr));if(sctlr&5){puts("MMU/cache unexpected\n");halt();}
 static const uint64_t bases[]={0x40000000ULL,0x60000000ULL,0x100000000ULL,0x140000000ULL,0x180000000ULL,0x1c0000000ULL,0x200000000ULL,0x23fff0000ULL};
 static const uint64_t salts[]={0,~0ULL,0xaaaaaaaaaaaaaaaaULL,0x5555555555555555ULL};
 for(unsigned s=0;s<4;s++){
  for(unsigned b=0;b<8;b++)for(unsigned i=0;i<8192;i++)((volatile uint64_t*)bases[b])[i]=(bases[b]+8*i)^salts[s];
  __asm__ volatile("dsb sy" ::: "memory");
  for(unsigned b=0;b<8;b++)for(unsigned i=0;i<8192;i++){
   uint64_t want=(bases[b]+8*i)^salts[s],got=((volatile uint64_t*)bases[b])[i];
   if(want!=got){puts("A64 MEM FAIL addr=");hex(bases[b]+8*i);puts(" want=");hex(want);puts(" got=");hex(got);puts("\n");halt();}
  }
 }
 puts("A64 MEM PASS 262144 x 64-bit checks, including RAM above 4GiB and final 64KiB\nEntering source TF-A 0x48000000\n");
}
