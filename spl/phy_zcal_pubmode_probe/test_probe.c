// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#define T736_PHY_ZCAL_PUBMODE_NATIVE
#include "probe.c"
static uint32_t nw,np,ticks,fail_uart,drift,stall,nt,fail_write,zero_codes;
static uint16_t mode,rate,zreset,run,pre,post;
uint32_t test_counter(void){nt++;if(!stall||nt<stall)ticks+=24;return ticks;}
int test_progress(unsigned int op,uint32_t a){assert(op==1||op==3||op==4);assert(a);np++;return np==fail_uart?-1:0;}
uint32_t test_read(uint32_t a,unsigned int width){
 uint32_t i,ch,v=0;int found=0;
 for(i=0;i<12;i++)if(a==ca[i]){assert(width==32);v=cv[i];found=1;}
 for(ch=0;ch<2;ch++)for(i=0;i<11;i++)if(a==0xa110000+ch*0x400000+offsets[i]){assert(width==32);v=held[i];found=1;}
 for(i=0;i<25;i++)if(a==pa[i]){assert(width==16);v=pv[i];found=1;}
 if(a==0xa940008){v=450;found=1;}
 if(a==0xa9400dc){v=mode;found=1;}
 if(a==0xa940628||a==0xa94062a){v=zero_codes?0u:(zreset?0xffu:(run?0x35u:0x47u));found=1;}
 if(a==0xa940606)v=rate;
 if(a==0xa940620)v=zreset;
 if(a==0xa940622)v=run;
 if(a==0xa94061a)v=pre;
 if(a==0xa940640)v=post;
 assert(found);return v^(np==drift?1u:0u);
}
void test_write(uint32_t a,uint16_t v){
 assert(nw<13&&a==wa[nw]&&v==wv[nw]);nw++;
 if(nw==fail_write)v^=1;
 if(a==0xa9400dc)mode=v;
 else if(a==0xa940606)rate=v;
 else if(a==0xa940620)zreset=v;
 else if(a==0xa940622)run=v;
 else if(a==0xa94061a)pre=v;
 else if(a==0xa940640)post=v;
 else assert(0);
}
static void reset(void){nw=np=ticks=nt=fail_uart=drift=stall=fail_write=zero_codes=0;mode=zreset=run=pre=post=0;rate=9;}
int main(int argc,char **argv){uint32_t i;FILE *f;
 reset();assert(!probe_main());assert(probe_report.status==2&&np==145&&nw==13);
 assert(probe_report.run_ticks>=2400&&probe_report.component_result==T736_ZCAL_TIMED_ONLY);
 assert(!mode&&!run&&!zreset&&!pre&&!post&&rate==9);
 assert(probe_report.events[61].address==0xa9400dc&&probe_report.events[61].value==1);
 assert(probe_report.events[103].address==0xa9400dc&&probe_report.events[103].value==0);
 assert(probe_report.events[77].address==0xa940628&&probe_report.events[77].value==0x35);
 assert(argc==2);f=fopen(argv[1],"wb");assert(f);assert(fwrite((const void*)&probe_report,1,sizeof(probe_report),f)==sizeof(probe_report));assert(!fclose(f));
 for(i=1;i<=145;i++){reset();fail_uart=i;assert(probe_main()==-1);assert(np==i&&probe_report.completed==i-1&&probe_report.uart_error);}
 for(i=1;i<=61;i++){reset();drift=i;assert(probe_main()==-1);assert(np==i&&!nw);}
 for(i=106;i<=145;i++){reset();drift=i;assert(probe_main()==-1);assert(np==i&&nw==13);}
 reset();fail_write=1;assert(probe_main()==-1&&nw==1&&mode==0);
 reset();fail_write=13;assert(probe_main()==-1&&nw==13&&mode==1);
 for(i=5;i<=6;i++){reset();fail_write=i;assert(probe_main()==-1&&nw==i&&mode==1&&!run);}
 for(i=9;i<=11;i++){reset();fail_write=i;assert(probe_main()==-1&&nw==i&&mode==1);}
 reset();stall=1;assert(probe_main()==-1&&!nw);
 reset();stall=3;assert(probe_main()==-1&&nw==2&&mode==1&&zreset==1);
 reset();stall=5;assert(probe_main()==-1&&nw==7&&mode==1&&run==1);
 reset();zero_codes=1;assert(!probe_main()&&probe_report.component_result==T736_ZCAL_TIMED_ONLY);
 puts("PASS:145 events/13 ordered writes;145 UART faults;61 initial and40 final guard faults;mode entry/restore mismatches;companion mismatches;3 timer stalls;zero codes stay timed-only;no failure cleanup");
 return 0;
}
