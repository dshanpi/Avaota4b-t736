// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#define T736_PHY_ZCAL_DFI_UPDATE_NATIVE
#include "probe.c"
static uint32_t nw,np,ticks,fail_uart,drift,stall,nt,stuck,clock_word;
uint32_t test_counter(void){nt++;if(!stall||nt<stall)ticks+=24;return ticks;}
int test_progress(unsigned int op,uint32_t a){assert(op==1||op==3||op==2);assert(a);np++;return np==fail_uart?-1:0;}
uint32_t test_read(uint32_t a,unsigned int width){
 uint32_t i,ch,v=0;int found=0;
 for(i=0;i<12;i++)if(a==ca[i]){assert(width==32);v=cv[i];found=1;}
 for(ch=0;ch<2;ch++)for(i=0;i<11;i++)if(a==0xa110000+ch*0x400000+offsets[i]){assert(width==32);v=held[i];found=1;}
 for(i=0;i<25;i++)if(a==pa[i]){assert(width==16);v=pv[i];found=1;}
 if(a==0xa940008){v=450;found=1;}
 if(a==0x02002c00){if(!stuck)clock_word&=~0x08000000u;v=clock_word;}
 assert(found);return v^(np==drift?1u:0u);
}
void test_write(uint32_t a,uint32_t v){assert(!nw&&a==0x02002c00&&v==0x88000003);nw++;clock_word=v;}
static void reset(void){nw=np=ticks=nt=fail_uart=drift=stall=stuck=0;clock_word=0x80000003;}
int main(int argc,char **argv){uint32_t i;FILE *f;
 reset();assert(!probe_main());assert(probe_report.status==2&&np==102&&nw==1);
 assert(probe_report.run_ticks>=24000&&probe_report.component_result==T736_ZCAL_TIMED_ONLY);
 assert(clock_word==0x80000003);
 assert(probe_report.events[60].op==2&&probe_report.events[60].value==0x88000003);
 assert(probe_report.events[61].op==1&&probe_report.events[61].value==0x80000003);
 assert(argc==2);f=fopen(argv[1],"wb");assert(f);assert(fwrite((const void*)&probe_report,1,sizeof(probe_report),f)==sizeof(probe_report));assert(!fclose(f));
 for(i=1;i<=102;i++){reset();fail_uart=i;assert(probe_main()==-1);assert(np==i&&probe_report.completed==i-1&&probe_report.uart_error);}
 for(i=1;i<=60;i++){reset();drift=i;assert(probe_main()==-1);assert(np==i&&!nw);}
 for(i=62;i<=102;i++){reset();drift=i;assert(probe_main()==-1);assert(np==i&&nw==1);}
 reset();stuck=1;assert(probe_main()==-1&&nw==1&&np==62&&clock_word==0x88000003);
 reset();stall=1;assert(probe_main()==-1&&!nw);
 reset();stall=3;assert(probe_main()==-1&&nw==1&&np==61);
 puts("PASS:102 events/one32-bit clock update;102 UART faults;101 read guards;update stuck;two timer stalls;no failure cleanup");return 0;
}
