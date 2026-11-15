// SPDX-License-Identifier: GPL-2.0-only
/* V2-specific ledger: INITTMG1 is an independent read-only sentinel, not one
 * of the six configuration words. Do not derive expectations from probe.c.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define T736_CONTROLLER_STATIC_V2_NATIVE
#include "probe.c"
struct pair {uint32_t address,value;};
static const struct pair context_ref[12]={
 {0x2002020,0xf9004a00},{0x2002c00,0x80000003},{0x2002c0c,0x10001},{0xa020010,0x273f},
 {0xa020014,2},{0x7010250,1},{0x8020000,1},{0x8020020,24000000},
 {0xa02021c,0},{0xa020220,0},{0xa020224,0},{0xa020228,0}};
static const struct pair held_ref[14]={
 {0,0x03081002},{0x14,0},{0x90,0},{0x180,0x800},{0x510,0x10014},{0x508,0xc0000000},
 {0x500,0},{0x518,0x25000000},{0xc80,1},{0xc84,1},{0x514,0},{0xca4,0},{0x80,0x30},{0xd04,0}};
static const struct pair phy_ref[6]={{0xaaa0000,0},{0xaaa0132,1},{0xaa80100,3},{0xa9400c0,2},{0xa940620,0},{0xa940622,0}};
static const struct pair initial_fields[6]={{0xa110d00,0x2004e},{0xa100500,0x510},{0xa100504,0},{0xa100508,0},{0xa10050c,0},{0xa100d00,0}};
static const struct pair write_ref[20]={
 {0xa110c80,0},{0xa510c80,0},{0xa110ca4,1},{0xa510ca4,1},
 {0xa110d00,0x1036f},{0xa100500,0xfc003f},{0xa100504,0x330008},{0xa100508,0x40072},{0xa10050c,0x24001d},{0xa100d00,1},
 {0xa510d00,0x1036f},{0xa500500,0xfc003f},{0xa500504,0x330008},{0xa500508,0x40072},{0xa50050c,0x24001d},{0xa500d00,1},
 {0xa110ca4,0},{0xa510ca4,0},{0xa110c80,1},{0xa510c80,1}};
static struct pair memory[80],ledger[CAP];
static uint32_t count,ledger_count,mmios,progress_count,writes,fail_progress,drift_at,ticks;
static uint32_t stall_phase,stall_stage,delayed_reads,stuck_write,remaining[2],pending[2],desired[2];
static uint32_t final_init1_channel,final_init1_value,write_position[20];
static uint32_t *cell(uint32_t a){uint32_t i;for(i=0;i<count;i++)if(memory[i].address==a)return &memory[i].value;
 assert(count<80);memory[count]=(struct pair){a,0};return &memory[count++].value;}
static void add(uint32_t op,uint32_t a){assert(ledger_count<CAP);ledger[ledger_count++]=(struct pair){a,op};}
static void guard_ledger(void){uint32_t ch,i;
 for(i=0;i<12;i++)add(1,context_ref[i].address);
 for(ch=0;ch<2;ch++)for(i=0;i<14;i++)add(1,0xa110000+ch*0x400000+held_ref[i].address);
 for(i=0;i<6;i++)add(3,phy_ref[i].address);
}
static void prepare(uint32_t extra_reads){uint32_t ch,i,j;
 count=ledger_count=mmios=progress_count=writes=fail_progress=drift_at=ticks=stall_phase=stall_stage=stuck_write=final_init1_channel=final_init1_value=0;
 delayed_reads=extra_reads;
 memset(memory,0,sizeof(memory));memset(ledger,0,sizeof(ledger));memset(pending,0,sizeof(pending));
 memset(remaining,0,sizeof(remaining));memset(desired,0,sizeof(desired));memset(write_position,0,sizeof(write_position));
 for(i=0;i<12;i++)*cell(context_ref[i].address)=context_ref[i].value;
 for(ch=0;ch<2;ch++){
  for(i=0;i<14;i++)*cell(0xa110000+ch*0x400000+held_ref[i].address)=held_ref[i].value;
  for(i=0;i<6;i++)*cell(initial_fields[i].address+ch*0x400000)=initial_fields[i].value;
 }
 for(i=0;i<6;i++)*cell(phy_ref[i].address)=phy_ref[i].value;
 for(i=0;i<6;i++)add(1,context_ref[i].address);
 guard_ledger();
 for(ch=0;ch<2;ch++)for(i=0;i<6;i++)add(1,initial_fields[i].address+ch*0x400000);
 for(i=0;i<20;i++){
  write_position[i]=ledger_count;add(2,write_ref[i].address);add(1,write_ref[i].address);
  if(i==0 || i==1 || i==18 || i==19)for(j=0;j<=extra_reads;j++)add(1,write_ref[i].address+4);
 }
 guard_ledger();
 for(ch=0;ch<2;ch++)for(i=0;i<6;i++)add(1,initial_fields[i].address+ch*0x400000);
 assert(ledger_count==166+4*extra_reads);
}
int test_progress(unsigned int op,uint32_t a){assert(progress_count<ledger_count);
 assert(ledger[progress_count].address==a && ledger[progress_count].value==op);progress_count++;
 return fail_progress==progress_count?-1:0;
}
uint32_t test_counter(void){if(!(stall_phase==probe_report.phase && (!stall_stage || stall_stage==probe_report.component.stage)))ticks+=24;return ticks;}
uint32_t test_read(uint32_t a,unsigned int width){uint32_t ch,v;
 assert(progress_count==mmios+1 && (ledger[mmios].value==3?16u:32u)==width);mmios++;
 for(ch=0;ch<2;ch++)if(a==0xa110c84+ch*0x400000 && pending[ch]){
  if(remaining[ch])remaining[ch]--;else {*cell(a)=desired[ch];pending[ch]=0;}
 }
 v=*cell(a);
 if(writes==20 && final_init1_channel && a==0xa110d04+(final_init1_channel-1)*0x400000)v=final_init1_value;
 return v^(drift_at==mmios?1u:0u);
}
void test_write(uint32_t a,uint32_t v){uint32_t ch,mask,want;
 assert(writes<20 && a==write_ref[writes].address && a!=0xa110d04 && a!=0xa510d04);
 assert(progress_count==mmios+1 && ledger[mmios].value==2 && probe_report.pre_ticks>=240);
 want=write_ref[writes].value;
 if(writes>=4 && writes<16){
  mask=(writes==4 || writes==10)?0x03ff1fffu:(writes==9 || writes==15)?1u:UINT32_MAX;
  want=(*cell(a)&~mask)|want;
  assert(!*cell(0xa110c80) && !*cell(0xa510c80) && *cell(0xa110ca4)==1 && *cell(0xa510ca4)==1);
 }
 assert(v==want);*cell(a)=v;writes++;mmios++;
 for(ch=0;ch<2;ch++)if(a==0xa110c80+ch*0x400000){pending[ch]=1;remaining[ch]=writes==stuck_write?100u:delayed_reads;desired[ch]=v;}
}
static void verify(void){uint32_t i,ch;
 assert(!probe_main() && probe_report.status==2 && probe_report.phase==3 && probe_report.result==1);
 assert(mmios==ledger_count && progress_count==ledger_count && probe_report.completed==ledger_count && probe_report.attempted==ledger_count);
 assert(writes==20 && probe_report.writes==20 && !probe_report.uart_error && probe_report.pre_ticks>=240 && probe_report.settle_ticks>=480);
 for(i=0;i<ledger_count;i++)assert(probe_report.events[i].op==ledger[i].value && probe_report.events[i].address==ledger[i].address);
 for(i=ledger_count;i<CAP;i++)assert(!probe_report.events[i].op && !probe_report.events[i].address && !probe_report.events[i].value);
 assert(probe_report.component.reads_completed==140+4*delayed_reads && probe_report.component.writes_completed==20);
 assert(probe_report.component.stage==8 && probe_report.component.final_valid && !probe_report.component.swctl_maybe_open && !probe_report.component.static_maybe_open);
 assert(probe_report.component.waited_us==20+4*delayed_reads);
 for(ch=0;ch<2;ch++){
  assert(!probe_report.component.inittmg1_before[ch] && !probe_report.component.inittmg1_after[ch] && !*cell(0xa110d04+ch*0x400000));
  for(i=0;i<6;i++)assert(probe_report.component.programmed[ch][i]==*cell(initial_fields[i].address+ch*0x400000));
 }
}
int main(int argc,char **argv){uint32_t i,j,ch,n,writes_before,positions[20],v,old_progress,old_mmios;FILE *f;
 prepare(0);verify();assert(argc==2);f=fopen(argv[1],"wb");assert(f);
 assert(fwrite((const void *)&probe_report,1,sizeof(probe_report),f)==sizeof(probe_report));assert(!fclose(f));
 memcpy(positions,write_position,sizeof(positions));
 for(n=0;n<=3;n+=3){
  prepare(n);verify();
  for(i=1;i<=166+4*n;i++){
   prepare(n);writes_before=0;for(j=0;j<20;j++)if(write_position[j]<i-1)writes_before++;
   fail_progress=i;assert(probe_main()==-1 && mmios==i-1 && probe_report.completed==i-1 && probe_report.attempted==i);
   assert(probe_report.uart_error && writes==writes_before);
  }
 }
 for(i=1;i<=64;i++){prepare(0);drift_at=i;assert(probe_main()==-1 && mmios==i && !writes);}
 for(i=0;i<20;i++){prepare(0);drift_at=positions[i]+2;assert(probe_main()==-1 && writes==i+1 && probe_report.result==(uint32_t)T736_INIT_V2_VERIFY);}
 /* Every INITTMG1 bit is prohibited, independently before and after config. */
 for(ch=0;ch<2;ch++)for(i=0;i<32;i++){
  prepare(0);*cell(0xa110d04+ch*0x400000)=1u<<i;assert(probe_main()==-1 && !writes);
  assert(probe_report.component.inittmg1_before[ch]==(1u<<i));
  prepare(0);final_init1_channel=ch+1;final_init1_value=1u<<i;assert(probe_main()==-1 && writes==20);
  assert(probe_report.component.inittmg1_after[ch]==(1u<<i) && !probe_report.component.final_valid);
 }
 for(i=0;i<4;i++){
  static const uint32_t stuck[4]={1,2,19,20};
  prepare(3);stuck_write=stuck[i];assert(probe_main()==-1 && writes==stuck[i] && probe_report.result==(uint32_t)T736_INIT_V2_TIMEOUT);
 }
 prepare(0);stall_phase=1;assert(probe_main()==-1 && !writes && mmios==6);
 prepare(1);stall_phase=2;stall_stage=2;assert(probe_main()==-1 && writes==1 && probe_report.result==(uint32_t)T736_INIT_V2_TIMER);
 prepare(0);stall_phase=2;stall_stage=7;assert(probe_main()==-1 && writes==20 && probe_report.result==(uint32_t)T736_INIT_V2_TIMER);
 prepare(0);ticks=0xffffff00u;verify();
 prepare(0);*cell(0xa110d00)|=0x04008000;*cell(0xa100d00)=0x80000200;verify();
 assert(*cell(0xa110d00)==0x0401836f && *cell(0xa100d00)==0x80000201);
 old_mmios=mmios;old_progress=progress_count;v=0;
 assert(access_mmio(4,0xaaa0132,&v));assert(access_mmio(2,0xa110080,&v));assert(access_mmio(2,0xa110514,&v));
 assert(access_mmio(3,0xa110d04,&v));assert(access_mmio(1,0xaaa0000,&v));assert(access_mmio(1,0x40000000,&v));assert(access_mmio(3,0xaa801e2,&v));
 assert(mmios==old_mmios && progress_count==old_progress);
 probe_report.phase=2;before_valid=0xfff;
 for(i=0;i<20;i++){probe_report.writes=i;assert(!allowed_write(0xa110d04,0) && !allowed_write(0xa510d04,0));}
 probe_report.writes=0;before_valid=0;assert(!allowed_write(0xa110c80,0));before_valid=0xfff;assert(!allowed_write(0xa110c80,1));
 puts("PASS: independent166/178-event ledger,20 exact writes,344 UART faults,64 preflight drifts,20 readback faults,128 INITTMG1 before/after bit faults,4 SWSTAT timeouts,timer faults/wrap,strict read-only INITTMG1 gates,no cleanup,mask preservation");
 return 0;
}
