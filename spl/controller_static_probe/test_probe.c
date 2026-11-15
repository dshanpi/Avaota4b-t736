// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define T736_CONTROLLER_STATIC_NATIVE
#include "probe.c"
struct slot {uint32_t address,value;};
static struct slot memory[128];
static uint32_t count,mmios,actual_writes,progress_count,fail_progress,drift_at;
static uint32_t ticks,stall_phase,stall_stage,delay_reads,stall_sw_write;
static uint32_t pending[2],remaining[2],desired[2];
static uint32_t seen_ops[CAP],seen_addr[CAP],written_at[22];
static uint32_t *slot(uint32_t a){uint32_t i;for(i=0;i<count;i++)if(memory[i].address==a)return &memory[i].value;
 assert(count<128);memory[count].address=a;memory[count].value=0;return &memory[count++].value;}
int test_progress(unsigned int op,uint32_t a){assert(progress_count<CAP && (op==1 || op==2 || op==3));
 seen_ops[progress_count]=op;seen_addr[progress_count]=a;progress_count++;
 return fail_progress==progress_count?-1:0;
}
uint32_t test_counter(void){
 if(!(stall_phase==probe_report.phase && (!stall_stage || stall_stage==probe_report.component.stage)))ticks+=24;
 return ticks;
}
uint32_t test_read(uint32_t a,unsigned int width){uint32_t ch,v;
 assert(progress_count==mmios+1 && seen_addr[mmios]==a && (seen_ops[mmios]==3?16u:32u)==width);
 assert(mmios<CAP);mmios++;
 for(ch=0;ch<2;ch++)if(a==0x0a110c84+ch*0x400000 && pending[ch]){
  if(remaining[ch])remaining[ch]--;else {*slot(a)=desired[ch];pending[ch]=0;}
 }
 v=*slot(a);if(drift_at==mmios)v^=(a==0x0a110d04 || a==0x0a510d04)?0x10000u:1u;
 return v;
}
void test_write(uint32_t a,uint32_t v){uint32_t n=actual_writes,ch,i,want;
 static const uint32_t expected[7]={0x1036f,0x580000,0xfc003f,0x330008,0x40072,0x24001d,1};
 assert(n<22 && a==write_order[n] && !(a&3u));
 assert(progress_count==mmios+1 && seen_addr[mmios]==a && seen_ops[mmios]==2);
 assert(probe_report.pre_ticks>=240);
 if(n<2 || n==18 || n==19)want=0;else if(n<4 || n>=20)want=1;
 else {ch=n>=11u;i=n-4u-ch*7u;want=(*slot(a)&~masks[i])|expected[i];
  assert(!*slot(0x0a110c80) && !*slot(0x0a510c80));
  assert(*slot(0x0a110ca4)==1 && *slot(0x0a510ca4)==1);
 }
 assert(v==want);written_at[n]=mmios;actual_writes++;mmios++;*slot(a)=v;
 for(ch=0;ch<2;ch++)if(a==0x0a110c80+ch*0x400000){
  pending[ch]=1;remaining[ch]=actual_writes==stall_sw_write?100u:delay_reads;desired[ch]=v;
 }
}
static void setup(void){uint32_t ch,i;
 static const uint32_t hv[13]={0x03081002,0,0,0x800,0x10014,0xc0000000,0,0x25000000,1,1,0,0,0x30};
 static const uint32_t pv[6]={0,1,3,2,0,0};
 static const uint32_t baseline[7]={0x2004e,0,0x510,0,0,0,0};
 count=mmios=actual_writes=progress_count=fail_progress=drift_at=ticks=stall_phase=stall_stage=delay_reads=stall_sw_write=0;
 memset(memory,0,sizeof(memory));memset(pending,0,sizeof(pending));memset(remaining,0,sizeof(remaining));
 memset(desired,0,sizeof(desired));memset(seen_ops,0,sizeof(seen_ops));memset(seen_addr,0,sizeof(seen_addr));memset(written_at,0,sizeof(written_at));
 for(i=0;i<12;i++)*slot(ca[i])=cv[i];
 for(ch=0;ch<2;ch++){
  for(i=0;i<13;i++)*slot(0x0a110000+ch*0x400000+co[i])=hv[i];
  for(i=0;i<7;i++)*slot(config[i]+ch*0x400000)=baseline[i];
 }
 for(i=0;i<6;i++)*slot(pa[i])=pv[i];
}
static void success(uint32_t events){uint32_t i;
 assert(!probe_main());assert(probe_report.status==2 && probe_report.phase==3 && probe_report.result==1);
 assert(mmios==events && progress_count==events && probe_report.attempted==events && probe_report.completed==events);
 assert(actual_writes==22 && probe_report.writes==22 && !probe_report.uart_error);
 assert(probe_report.pre_ticks>=240 && probe_report.settle_ticks>=480);
 assert(probe_report.component.stage==8 && probe_report.component.final_valid && probe_report.component.waited_us==20+events-170);
 for(i=0;i<events;i++)assert(probe_report.events[i].op==seen_ops[i] && probe_report.events[i].address==seen_addr[i]);
 for(i=0;i<22;i++)assert(seen_ops[written_at[i]+1]==1 && seen_addr[written_at[i]+1]==write_order[i]);
 for(i=events;i<CAP;i++)assert(!probe_report.events[i].op && !probe_report.events[i].address && !probe_report.events[i].value);
 assert(!*slot(0x0a110ca4) && !*slot(0x0a510ca4) && *slot(0x0a110c80)==1 && *slot(0x0a510c80)==1);
}
int main(int argc,char **argv){uint32_t i,v,native_writes[22],old_mmios,old_progress;FILE *f;
 setup();success(170);assert(argc==2);f=fopen(argv[1],"wb");assert(f);
 assert(fwrite((const void *)&probe_report,1,sizeof(probe_report),f)==sizeof(probe_report));assert(!fclose(f));
 memcpy(native_writes,written_at,sizeof(native_writes));
 for(i=1;i<=170;i++){
  uint32_t j,want=0;for(j=0;j<22;j++)if(native_writes[j]<i-1)want++;
  setup();fail_progress=i;assert(probe_main()==-1);
  assert(probe_report.uart_error && mmios==i-1 && probe_report.completed==i-1 && probe_report.attempted==i && actual_writes==want);
 }
 /* Six wrapper context reads plus all58 component preflight reads. */
 for(i=1;i<=64;i++){setup();drift_at=i;assert(probe_main()==-1 && mmios==i && !actual_writes);}
 for(i=0;i<22;i++){setup();drift_at=native_writes[i]+2;assert(probe_main()==-1 && actual_writes==i+1 && probe_report.result==(uint32_t)T736_INIT_VERIFY);}
 setup();delay_reads=3;success(182); /* Maximum permitted successful polls. */
 for(i=1;i<=182;i++){
  setup();delay_reads=3;fail_progress=i;assert(probe_main()==-1);
  assert(probe_report.uart_error && mmios==i-1 && probe_report.completed==i-1 && probe_report.attempted==i);
 }
 for(i=0;i<4;i++){
  static const uint32_t stuck[4]={1,2,21,22};
  setup();stall_sw_write=stuck[i];assert(probe_main()==-1 && actual_writes==stuck[i]);
  assert(probe_report.result==(uint32_t)T736_INIT_TIMEOUT && probe_report.component.swctl_maybe_open);
 }
 setup();stall_phase=1;assert(probe_main()==-1 && !actual_writes && mmios==6);
 setup();stall_phase=2;stall_stage=2;delay_reads=1;assert(probe_main()==-1 && actual_writes==1 && probe_report.result==(uint32_t)T736_INIT_TIMER);
 setup();stall_phase=2;stall_stage=7;assert(probe_main()==-1 && actual_writes==22 && probe_report.result==(uint32_t)T736_INIT_TIMER);
 setup();ticks=0xffffff00u;success(170);
 setup();*slot(config[0])|=0x04008000;*slot(config[1])|=0x40000055;*slot(config[6])=0x80000200;success(170);
 assert(*slot(config[0])==0x0401836f && *slot(config[1])==0x40580055 && *slot(config[6])==0x80000201);
 old_mmios=mmios;old_progress=progress_count;v=0;
 assert(access_mmio(4,0x0aaa0132,&v));assert(access_mmio(2,0x0a110080,&v));assert(access_mmio(2,0x0a110514,&v));
 assert(access_mmio(3,0x0a110d00,&v));assert(access_mmio(1,0x0aaa0000,&v));
 assert(access_mmio(1,0x40000000,&v));assert(access_mmio(3,0x0aa801e2,&v));
 assert(mmios==old_mmios && progress_count==old_progress);
 probe_report.phase=2;probe_report.writes=0;before_valid=0;assert(!allowed_write(write_order[0],0));
 before_valid=0x3fff;assert(!allowed_write(write_order[0],1));assert(!allowed_write(write_order[1],0));
 probe_report.writes=4;assert(!allowed_write(write_order[4],0));
 puts("PASS: 170/182 events,22 independent exact word writes,352 UART fault positions,64 preflight drifts,22 readback failures,4 SWSTAT timeouts,timer faults/wrap,no cleanup,unrelated-bit preservation,forbidden writes/width/program/DDR rejection");
 return 0;
}
