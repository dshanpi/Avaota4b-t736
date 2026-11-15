// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define T736_PHY_ZCAL_NATIVE
#include "probe.c"
static uint32_t actual_writes,ticks,counter_calls,stall_after,progress_count,fail_progress,drift;
static uint16_t reset_value,run_value,pu,pd;
uint32_t test_counter(void){counter_calls++;if(!stall_after || counter_calls<stall_after)ticks+=24;return ticks;}
int test_progress(unsigned int op,uint32_t a){assert(op==1||op==3||op==4);assert(a);progress_count++;return fail_progress==progress_count?-1:0;}
uint32_t test_read(uint32_t a,unsigned int width){uint32_t i,ch;
 for(i=0;i<12;i++)if(a==ca[i]){assert(width==32);return cv[i];}
 for(ch=0;ch<2;ch++)for(i=0;i<11;i++)if(a==0xa110000+ch*0x400000+offsets[i]){assert(width==32);return held[i];}
 assert(width==16);
 if(a==0xa940008)return 450;
 if(a==0xa940620)return reset_value;
 if(a==0xa940622)return run_value;
 if(a==0xa94064c)return pu;
 if(a==0xa94064e)return pd;
 for(i=0;i<23;i++)if(a==pa[i])return pv[i]^(drift==i+1?1u:0u);
 assert(!"unapproved read");return 0;
}
void test_write(uint32_t a,uint16_t v){assert(actual_writes<4);assert(a==wa[actual_writes]&&v==wv[actual_writes]);actual_writes++;
 if(a==0xa940620)reset_value=v;
 else {run_value=v;if(!v){pu=0x4500;pd=0x4700;}}
}
static void reset(void){actual_writes=ticks=counter_calls=stall_after=progress_count=fail_progress=drift=0;reset_value=1;run_value=pu=pd=0;}
int main(int argc,char **argv){uint32_t i,n;FILE *f;reset();assert(!probe_main());assert(probe_report.status==2&&probe_report.component_result==T736_ZCAL_TIMED_ONLY&&actual_writes==4);assert(probe_report.run_ticks>=2400);n=progress_count;
 assert(argc==2);f=fopen(argv[1],"wb");assert(f);assert(fwrite((const void *)&probe_report,1,sizeof(probe_report),f)==sizeof(probe_report));assert(!fclose(f));
 for(i=1;i<=n;i++){reset();fail_progress=i;assert(probe_main()==-1);assert(probe_report.uart_error&&probe_report.completed==i-1&&probe_report.attempted==i);}
 reset();stall_after=1;assert(probe_main()==-1);assert(!actual_writes);
 reset();stall_after=5;assert(probe_main()==-1);assert(actual_writes==3&&run_value==1); /* no cleanup after timer failure */
 for(i=1;i<=19;i++){reset();drift=i;assert(probe_main()==-1);assert(!actual_writes);}
 reset();pu=1;assert(probe_main()==-1);assert(!actual_writes);
 printf("PASS: %u ordered events; four pulse writes; every UART fault; stall before/during pulse; analog/owner preconditions; no cleanup on failure\n",n);return 0;
}
