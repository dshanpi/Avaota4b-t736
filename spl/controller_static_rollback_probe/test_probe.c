// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define T736_CONTROLLER_STATIC_ROLLBACK_NATIVE
#include "probe.c"
struct slot {uint32_t address,value;};
static struct slot memory[80];
static uint32_t count,mmios,actual_writes,progress_count,fail_progress,drift_at;
static uint32_t ticks,stall_phase,delay_reads,stuck_channel;
static uint32_t pending[2],remaining[2];
static uint32_t seen_ops[CAP],seen_addr[CAP],written_at[5];
static uint32_t *slot(uint32_t a){uint32_t i;for(i=0;i<count;i++)if(memory[i].address==a)return &memory[i].value;
 assert(count<80);memory[count].address=a;memory[count].value=0;return &memory[count++].value;}
int test_progress(unsigned int op,uint32_t a){assert(progress_count<CAP && (op==1 || op==2 || op==3));
 seen_ops[progress_count]=op;seen_addr[progress_count]=a;progress_count++;
 return fail_progress==progress_count?-1:0;}
uint32_t test_counter(void){if(stall_phase!=probe_report.phase)ticks+=24;return ticks;}
uint32_t test_read(uint32_t a,unsigned int width){uint32_t ch,v;
 assert(progress_count==mmios+1 && seen_addr[mmios]==a && (seen_ops[mmios]==3?16u:32u)==width);
 assert(mmios<CAP);mmios++;
 for(ch=0;ch<2;ch++)if(a==0x0a110c84+ch*0x400000 && pending[ch]){
  if(remaining[ch])remaining[ch]--;else {*slot(a)=1;pending[ch]=0;}
 }
 v=*slot(a);return v^(drift_at==mmios?1u:0u);
}
void test_write(uint32_t a,uint32_t v){uint32_t n=actual_writes,ch;
 static const uint32_t addresses[5]={0x0a110d00,0x0a110ca4,0x0a510ca4,0x0a110c80,0x0a510c80};
 static const uint32_t expected[5]={0x2004e,0,0,1,1};
 assert(n<5 && a==addresses[n] && v==expected[n] && !(a&3u));
 assert(progress_count==mmios+1 && seen_addr[mmios]==a && seen_ops[mmios]==2);
 assert(probe_report.pre_ticks>=240);
 if(!n)assert(!*slot(0x0a110c80) && !*slot(0x0a510c80) && *slot(0x0a110ca4)==1 && *slot(0x0a510ca4)==1);
 if(n>=3)assert(!*slot(0x0a110ca4) && !*slot(0x0a510ca4) && *slot(0x0a110d00)==0x2004e);
 written_at[n]=mmios;actual_writes++;mmios++;*slot(a)=v;
 for(ch=0;ch<2;ch++)if(a==0x0a110c80+ch*0x400000){pending[ch]=1;remaining[ch]=stuck_channel==ch+1?100u:delay_reads;}
}
static void setup(void){uint32_t ch,i,v;
 count=mmios=actual_writes=progress_count=fail_progress=drift_at=ticks=stall_phase=delay_reads=stuck_channel=0;
 memset(memory,0,sizeof(memory));memset(pending,0,sizeof(pending));memset(remaining,0,sizeof(remaining));
 memset(seen_ops,0,sizeof(seen_ops));memset(seen_addr,0,sizeof(seen_addr));memset(written_at,0,sizeof(written_at));
 for(i=0;i<12;i++)*slot(ca[i])=cv[i];
 for(ch=0;ch<2;ch++){
  for(i=0;i<13;i++){v=closed[i];if(i==8 || i==9)v=0;if(i==11)v=1;*slot(0x0a110000+ch*0x400000+co[i])=v;}
  for(i=0;i<7;i++)*slot(config[i]+ch*0x400000)=(!ch && !i)?0x1036f:baseline[i];
 }
 for(i=0;i<6;i++)*slot(pa[i])=pv[i];
}
static void success(uint32_t events){uint32_t i;
 assert(!probe_main());assert(probe_report.status==2 && probe_report.phase==5);
 assert(mmios==events && progress_count==events && probe_report.attempted==events && probe_report.completed==events);
 assert(actual_writes==5 && probe_report.writes==5 && !probe_report.uart_error);
 assert(probe_report.pre_ticks>=240 && probe_report.settle_ticks>=480 && probe_report.poll_waits==events-128);
 for(i=0;i<events;i++)assert(probe_report.events[i].op==seen_ops[i] && probe_report.events[i].address==seen_addr[i]);
 for(i=0;i<5;i++)assert(seen_ops[written_at[i]+1]==1 && seen_addr[written_at[i]+1]==wa[i]);
 for(i=0;i<58;i++)assert(seen_addr[events-58+i]==seen_addr[i] && seen_ops[events-58+i]==seen_ops[i]);
 for(i=events;i<CAP;i++)assert(!probe_report.events[i].op && !probe_report.events[i].address && !probe_report.events[i].value);
 assert(*slot(0x0a110d00)==0x2004e && !*slot(0x0a110d04));
 assert(!*slot(0x0a110ca4) && !*slot(0x0a510ca4) && *slot(0x0a110c80)==1 && *slot(0x0a510c80)==1);
}
int main(int argc,char **argv){uint32_t i,j,v,want,native_writes[5],old_mmios,old_progress;FILE *f;
 setup();success(128);assert(argc==2);f=fopen(argv[1],"wb");assert(f);
 assert(fwrite((const void *)&probe_report,1,sizeof(probe_report),f)==sizeof(probe_report));assert(!fclose(f));
 memcpy(native_writes,written_at,sizeof(native_writes));
 assert(native_writes[0]==58 && native_writes[1]==60 && native_writes[2]==62 && native_writes[3]==64 && native_writes[4]==67);
 for(i=1;i<=128;i++){
  want=0;for(j=0;j<5;j++)if(native_writes[j]<i-1)want++;
  setup();fail_progress=i;assert(probe_main()==-1);
  assert(probe_report.uart_error && mmios==i-1 && probe_report.completed==i-1 && probe_report.attempted==i && actual_writes==want);
 }
 for(i=1;i<=128;i++)if(i<=58 || i>=71){setup();drift_at=i;assert(probe_main()==-1 && mmios==i);if(i<=58)assert(!actual_writes);}
 for(i=0;i<5;i++){setup();drift_at=native_writes[i]+2;assert(probe_main()==-1 && actual_writes==i+1 && probe_report.phase==2);}
 setup();delay_reads=3;success(134);
 for(i=1;i<=134;i++){
  setup();delay_reads=3;fail_progress=i;assert(probe_main()==-1);
  assert(probe_report.uart_error && mmios==i-1 && probe_report.completed==i-1 && probe_report.attempted==i);
 }
 for(i=1;i<=2;i++){setup();stuck_channel=i;assert(probe_main()==-1 && actual_writes==i+3 && probe_report.phase==2 && probe_report.poll_waits==3);}
 setup();stall_phase=1;assert(probe_main()==-1 && !actual_writes && mmios==58);
 setup();stall_phase=2;delay_reads=1;assert(probe_main()==-1 && actual_writes==4 && !probe_report.poll_waits);
 setup();stall_phase=3;assert(probe_main()==-1 && actual_writes==5 && probe_report.phase==3);
 setup();ticks=0xffffff00u;success(128);
 actual_writes=mmios=progress_count=0;assert(probe_main()==-1 && !actual_writes); /* Cannot replay on restored state. */
 setup();success(128);old_mmios=mmios;old_progress=progress_count;v=0;
 assert(access_mmio(2,0x0a110d04,&v));assert(access_mmio(2,0x0a510d04,&v));
 assert(access_mmio(2,0x0a110080,&v));assert(access_mmio(2,0x0a110514,&v));
 assert(access_mmio(4,0x0aaa0132,&v));assert(access_mmio(1,0x0aaa0000,&v));
 assert(access_mmio(3,0x0a110d00,&v));assert(access_mmio(1,0x40000000,&v));assert(access_mmio(3,0x0aa801e2,&v));
 assert(mmios==old_mmios && progress_count==old_progress);
 probe_report.phase=2;probe_report.writes=0;assert(!allowed(2,wa[0],0));assert(!allowed(2,wa[1],0));
 puts("PASS: 128/134 events,5 fixed rollback writes,262 UART faults,116 exact pre/post guard drifts,5 readback failures,2 SWSTAT timeouts,timer faults/wrap,no cleanup,no replay,INITTMG1/PHY/MR/DFI/program/DDR write rejection");
 return 0;
}
