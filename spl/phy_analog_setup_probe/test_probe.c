// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define T736_PHY_ANALOG_SETUP_NATIVE
#include "probe.c"

static uint32_t mmios,progress_count,write_count,fail_progress,drift_at;
static uint32_t timer,stall_phase,clock_initial,pstate_initial,por_initial;
static uint32_t configured[NWRITE],seen_ops[CAP],seen_addr[CAP];
static uint32_t writes_at[CAP];
int test_progress(unsigned int op,uint32_t a){
 assert(progress_count<CAP);assert(op==1 || op==3 || op==4);
 seen_ops[progress_count]=op;seen_addr[progress_count]=a;progress_count++;
 return fail_progress==progress_count?-1:0;
}
uint32_t test_counter(void){if(stall_phase!=probe_report.phase)timer+=24;return timer;}
uint32_t test_read(uint32_t a,unsigned int width){
 uint32_t i,ch,v=0;int found=0;
 assert(mmios<CAP && progress_count==mmios+1 && seen_addr[mmios]==a);
 assert((seen_ops[mmios]==3?16u:32u)==width);mmios++;
 for(i=0;i<12;i++)if(a==ca[i]){assert(width==32);v=cv[i];found=1;}
 for(ch=0;ch<2;ch++)for(i=0;i<11;i++)if(a==0xa110000+ch*0x400000+offsets[i]){assert(width==32);v=held[i];found=1;}
 if(a==0x0aaa0000){assert(width==16);v=0;found=1;}
 if(a==0x0aaa0132){assert(width==16);v=1;found=1;}
 if(a==0x0aa80100){assert(width==16);v=3;found=1;}
 for(i=0;i<NSTATIC;i++)if(a==phy_static[i]){assert(width==16);v=initial[i];found=1;}
 if(a==extra[0]){assert(width==16);v=clock_initial;found=1;}
 if(a==extra[1]){assert(width==16);v=pstate_initial;found=1;}
 if(a==extra[2]){assert(width==16);v=por_initial;found=1;}
 for(i=0;i<write_count;i++)if(a==write_address[i])v=configured[i];
 assert(found);return v^(drift_at==mmios?1u:0u);
}
void test_write(uint32_t a,uint32_t v,unsigned int width){
 assert(width==16 && write_count<NWRITE && mmios<CAP);
 assert(progress_count==mmios+1 && seen_addr[mmios]==a && seen_ops[mmios]==4);
 assert(a==write_address[write_count]);
 assert(v==(write_count==10?((clock_initial&0xf800u)|450u):write_value[write_count]));
 if(a==0x0a940008)assert((v&0x7ffu)==450u && (v&0xf800u)==(clock_initial&0xf800u));
 assert(probe_report.pre_ticks>=240);
 if(write_count>=2)assert(probe_report.reset_ticks>=4800);
 writes_at[write_count]=mmios;configured[write_count]=v;write_count++;mmios++;
}
static void reset(void){
 mmios=progress_count=write_count=fail_progress=drift_at=timer=stall_phase=0;
 clock_initial=0xa123;pstate_initial=5;por_initial=3;
 memset(configured,0,sizeof(configured));memset(seen_ops,0,sizeof(seen_ops));
 memset(seen_addr,0,sizeof(seen_addr));memset(writes_at,0,sizeof(writes_at));
}
static void verify_success(void){
 uint32_t i;
 assert(!probe_main());assert(probe_report.status==2 && probe_report.phase==10);
 assert(mmios==179 && progress_count==179 && probe_report.attempted==179 && probe_report.completed==179);
 assert(probe_report.writes==13 && write_count==13 && !probe_report.uart_error);
 assert(probe_report.pre_ticks>=240 && probe_report.reset_ticks>=4800 && probe_report.settle_ticks>=2400);
 for(i=0;i<179;i++)assert(probe_report.events[i].op==seen_ops[i] && probe_report.events[i].address==seen_addr[i]);
 assert(seen_addr[34]==0x0aaa0000 && seen_addr[35]==0x0aaa0132 && seen_addr[36]==0x0aa80100);
 for(i=0;i<NSTATIC;i++)assert(seen_addr[37+i]==phy_static[i]);
 for(i=0;i<3;i++)assert(seen_addr[73+i]==extra[i]);
 assert(writes_at[0]==76 && writes_at[1]==79);
 assert(seen_ops[78]==3 && seen_addr[78]==0x0aaa0000);
 for(i=2;i<NWRITE;i++)assert(writes_at[i]==81+(i-2)*2);
 for(i=0;i<76;i++)assert(seen_ops[103+i]==seen_ops[i] && seen_addr[103+i]==seen_addr[i]);
 for(i=0;i<NWRITE;i++){
  uint32_t n=writes_at[i];
  assert(seen_ops[n]==4 && seen_ops[n+1]==3 && seen_addr[n]==seen_addr[n+1]);
  assert(probe_report.events[n].value==probe_report.events[n+1].value);
 }
 for(i=179;i<CAP;i++)assert(!probe_report.events[i].op && !probe_report.events[i].address && !probe_report.events[i].value);
}
int main(int argc,char **argv){
 uint32_t i,v,before,ref_ops[179],ref_writes[179],count=0;FILE *f;
 reset();verify_success();assert(argc==2);f=fopen(argv[1],"wb");assert(f);
 assert(fwrite((const void *)&probe_report,1,sizeof(probe_report),f)==sizeof(probe_report));assert(!fclose(f));
 for(i=0;i<179;i++){ref_ops[i]=seen_ops[i];ref_writes[i]=count;if(seen_ops[i]==4)count++;}
 for(i=1;i<=179;i++){
  reset();fail_progress=i;assert(probe_main()==-1);
  assert(probe_report.uart_error && mmios==i-1 && probe_report.completed==i-1 && probe_report.attempted==i);
  assert(write_count==ref_writes[i-1] && probe_report.writes==write_count);
 }
 for(i=1;i<=179;i++)if(ref_ops[i-1]!=4 && !(i>=74 && i<=76)){
  reset();drift_at=i;assert(probe_main()==-1);assert(mmios==i && probe_report.completed==i);
  assert(write_count==ref_writes[i-1]);
 }
 /* The three newly observed values have no invented initial preset. */
 reset();clock_initial=0;pstate_initial=0;por_initial=0;verify_success();
 reset();clock_initial=0xffff;pstate_initial=0xffff;por_initial=0xffff;verify_success();
 reset();timer=0xfffff000u;verify_success(); /* Modular counter wrap. */
 reset();stall_phase=4;assert(probe_main()==-1 && !write_count && probe_report.phase==4 && mmios==76);
 reset();stall_phase=6;assert(probe_main()==-1 && write_count==2 && probe_report.phase==6 && mmios==81);
 reset();stall_phase=8;assert(probe_main()==-1 && write_count==13 && probe_report.phase==8 && mmios==103);
 reset();verify_success();before=mmios;
 v=1;assert(access(2,0x0a940620,&v,clock_initial)); /* No word writes. */
 assert(sample(1,0x0a940620,&v));assert(sample(3,0x02002020,&v));
 assert(sample(3,0x0aa801e2,&v));assert(sample(3,0x0aa8010c,&v));
 assert(sample(3,0x0aa80242,&v));assert(sample(3,0x40000000,&v));
 v=1;assert(access(4,0x0a940620,&v,clock_initial)); /* No ZCal reset writes. */
 assert(access(4,0x0a940622,&v,clock_initial)); /* No ZCal run writes. */
 assert(access(4,0x0aaa0000,&v,clock_initial)); /* No owner writes. */
 assert(access(4,0x0aa80100,&v,clock_initial)); /* No clock-enable writes. */
 assert(access(4,0x0a110514,&v,clock_initial)); /* No DFI handoff. */
 v=3;assert(access(4,0x0a9400c0,&v,clock_initial)); /* No reset deassertion. */
 v=450;assert(access(4,0x0a940008,&v,clock_initial)); /* High bits preserved. */
 v=(clock_initial&0xf800u)|0x450u;assert(access(4,0x0a940008,&v,clock_initial)); /* MHz is decimal. */
 v=0x10009;assert(access(4,0x0aaa0132,&v,clock_initial));assert(set(NWRITE,clock_initial));
 assert(mmios==before && progress_count==before && write_count==13);
 printf("PASS: 179 ordered MMIO events, exactly 13 halfword writes/readbacks, 179 UART faults, 163 strict-read drifts, high-bit preservation, bounded 10/200/100us waits and counter wrap, forbidden address/value/width rejection\n");
 return 0;
}
