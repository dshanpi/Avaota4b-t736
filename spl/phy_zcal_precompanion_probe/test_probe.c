// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define T736_PHY_ZCAL_PRECOMPANION_NATIVE
#include "probe.c"
static uint32_t actual_writes,ticks,counter_calls,stall_after,progress_count,fail_progress,drift_event;
static uint32_t mutate_edge,companion_reads,pupd_reads,zero_results;
static uint16_t reset_value,run_value,pu,pd,micro,rate,companion,companion_pre;
static unsigned int stuck_reset_clear,stuck_run_clear,bad_run_after_restore;
uint32_t test_counter(void){counter_calls++;if(!stall_after || counter_calls<stall_after)ticks+=24;return ticks;}
int test_progress(unsigned int op,uint32_t a){assert(op==1||op==3||op==4);assert(a);progress_count++;return fail_progress==progress_count?-1:0;}
static uint32_t value_at(uint32_t a,unsigned int width){uint32_t i,ch;
 for(i=0;i<12;i++)if(a==ca[i]){assert(width==32);return cv[i];}
 for(ch=0;ch<2;ch++)for(i=0;i<11;i++)if(a==0xa110000+ch*0x400000+offsets[i]){assert(width==32);return held[i];}
 assert(width==16);
 if(a==0xaaa0132)return micro;
 if(a==0xa940606)return rate;
 if(a==0xa940008)return 450;
 if(a==0xa940620)return reset_value;
 if(a==0xa940622)return run_value|((bad_run_after_restore&&actual_writes==11)?1u:0u);
 if(a==0xa94061a)return companion_pre;
 if(a==0xa940640){companion_reads++;return companion;}
 if(a==0xa94064c){pupd_reads++;return pu;}
 if(a==0xa94064e){pupd_reads++;return pd;}
 for(i=0;i<25;i++)if(a==pa[i])return pv[i];
 assert(!"unapproved read");return 0;
}
uint32_t test_read(uint32_t a,unsigned int width){return value_at(a,width)^(drift_event==progress_count?1u:0u);}
void test_write(uint32_t a,uint16_t v){
 assert(actual_writes<11);assert(a==wa[actual_writes]&&v==wv[actual_writes]);actual_writes++;
 if(a==0xa940606){assert(v==9||v==0x19);rate=v;}
 else if(a==0xa940620){if(v||!stuck_reset_clear)reset_value=v;}
 else if(a==0xa940622){if(v||!stuck_run_clear)run_value=v;if(!v&&!zero_results){pu=0x4500;pd=0x4700;}}
 else if(a==0xa94061a){
  assert(!run_value&&rate==0x19);
  companion_pre=(uint16_t)(v^(mutate_edge==actual_writes?1u:0u));
 }
 else if(a==0xa940640){
  assert(!reset_value&&!run_value&&rate==0x19);
  companion=(uint16_t)(v^(mutate_edge==actual_writes?1u:0u));
  if(actual_writes==10&&!zero_results){pu=0x5500;pd=0x5700;}
 }else assert(!"unapproved write");
}
static void reset(void){
 actual_writes=ticks=counter_calls=stall_after=progress_count=fail_progress=drift_event=0;
 mutate_edge=companion_reads=pupd_reads=zero_results=0;
 reset_value=run_value=pu=pd=companion=companion_pre=0;micro=1;rate=9;
 stuck_reset_clear=stuck_run_clear=bad_run_after_restore=0;
}
static void check_observation_groups(void){
 const uint32_t indices[3]={73,81,87};uint32_t group;
 for(group=0;group<3;group++){
  assert(probe_report.events[indices[group]].address==0xa94064c);
  assert(probe_report.events[indices[group]+1].address==0xa94064e);
  assert(probe_report.events[indices[group]].value==(group?0x5500u:0x4500u));
  assert(probe_report.events[indices[group]+1].value==(group?0x5700u:0x4700u));
 }
}
int main(int argc,char **argv){uint32_t i,n;FILE *f;
 reset();assert(!probe_main());assert(probe_report.status==2&&probe_report.component_result==T736_ZCAL_TIMED_ONLY&&actual_writes==11);
 assert(probe_report.run_ticks>=2400&&progress_count==129&&probe_report.completed==129);
 assert(companion_reads==5&&pupd_reads==8&&rate==9&&!companion&&!companion_pre);
 check_observation_groups();n=progress_count;
 assert(argc==2);f=fopen(argv[1],"wb");assert(f);assert(fwrite((const void *)&probe_report,1,sizeof(probe_report),f)==sizeof(probe_report));assert(!fclose(f));
 for(i=1;i<=n;i++){
  reset();fail_progress=i;assert(probe_main()==-1);
  assert(progress_count==i&&probe_report.uart_error&&probe_report.completed==i-1&&probe_report.attempted==i);
 }
 for(i=1;i<=60;i++){
  reset();drift_event=i;assert(probe_main()==-1);assert(progress_count==i&&!actual_writes);
 }
 for(i=90;i<=129;i++){
  reset();drift_event=i;assert(probe_main()==-1);assert(progress_count==i&&actual_writes==11);
 }
 reset();zero_results=1;assert(!probe_main());assert(!pu&&!pd&&probe_report.component_result==T736_ZCAL_TIMED_ONLY);
 reset();stall_after=1;assert(probe_main()==-1);assert(!actual_writes);
 reset();stall_after=3;assert(probe_main()==-1);assert(actual_writes==1&&reset_value==1);
 reset();stall_after=5;assert(probe_main()==-1);assert(actual_writes==6&&run_value==1);
 reset();stuck_reset_clear=1;assert(probe_main()==-1);assert(actual_writes==7&&rate==0x19&&reset_value==1&&!probe_report.component_result);
 reset();stuck_run_clear=1;assert(probe_main()==-1);assert(actual_writes==7&&rate==0x19&&run_value==1&&!probe_report.component_result);
 for(i=8;i<=10;i++){
  reset();mutate_edge=i;assert(probe_main()==-1);
  assert(actual_writes==i&&rate==0x19&&!probe_report.component_result);
  assert(probe_report.events[probe_report.completed-1].address==0xa940640);
 }
 for(i=4;i<=5;i++){
  reset();mutate_edge=i;assert(probe_main()==-1);
  assert(actual_writes==i&&rate==0x19&&!run_value&&!probe_report.component_result);
  assert(probe_report.events[probe_report.completed-1].address==0xa94061a);
 }
 /* i=9 models a possibly self-clearing1: exact readback0 is a mismatch,
  * so there is no final0 edge and no Rate restoration after this result. */
 reset();mutate_edge=9;assert(probe_main()==-1);assert(actual_writes==9&&!companion&&rate==0x19);
 reset();bad_run_after_restore=1;assert(probe_main()==-1);assert(actual_writes==11&&rate==9&&!probe_report.component_result);
 printf("PASS: %u ordered events,11 writes,three PU/PD pairs;129 UART faults;60 preflight/40 final drifts;three timer stalls;stuck reset/run;all2030d/20320 edge mismatches including self-clear;zero and changed results remain timed-only;no cleanup\n",n);
 return 0;
}
