// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define T736_CONTROLLER_INIT_READ_NATIVE
#include "probe.c"
static uint32_t reads,progress_count,fail_progress,drift_at,observation_xor;
static uint32_t seen_ops[CAP],seen_addr[CAP],read_addr[CAP],read_width[CAP];
int test_progress(unsigned int op,uint32_t a){
 assert(progress_count<CAP && (op==1 || op==3));
 seen_ops[progress_count]=op;seen_addr[progress_count]=a;progress_count++;
 return fail_progress==progress_count?-1:0;
}
uint32_t test_read(uint32_t a,unsigned int width){
 uint32_t i,ch,v=0;int found=0;
 assert(reads<CAP);read_addr[reads]=a;read_width[reads]=width;reads++;
 assert(progress_count==reads && seen_addr[reads-1]==a && (seen_ops[reads-1]==3?16u:32u)==width);
 for(ch=0;ch<2;ch++){
  for(i=0;i<NCTL;i++)if(a==0x0a110000+ch*0x400000+ctl_offsets[i]){assert(width==32);v=a^observation_xor;found=1;}
  for(i=0;i<NTIMING;i++)if(a==0x0a100000+ch*0x400000+timing_offsets[i]){assert(width==32);v=a^observation_xor;found=1;}
 }
 for(i=0;i<12;i++)if(a==ca[i]){assert(width==32);v=cv[i];found=1;}
 for(ch=0;ch<2;ch++)for(i=0;i<11;i++)if(a==0x0a110000+ch*0x400000+held_offsets[i]){assert(width==32);v=held[i];found=1;}
 for(i=0;i<6;i++)if(a==phy_guard[i]){assert(width==16);v=phy_expected[i];found=1;}
 assert(found);return v^(drift_at==reads?1u:0u);
}
static void reset(void){reads=progress_count=fail_progress=drift_at=observation_xor=0;memset(seen_ops,0,sizeof(seen_ops));memset(seen_addr,0,sizeof(seen_addr));memset(read_addr,0,sizeof(read_addr));memset(read_width,0,sizeof(read_width));}
static void verify_success(void){
 uint32_t i,ch,n=40;
 assert(!probe_main());assert(probe_report.status==2 && probe_report.phase==6);
 assert(reads==120 && progress_count==120 && probe_report.attempted==120 && probe_report.completed==120);
 assert(!probe_report.writes && !probe_report.uart_error);
 for(i=0;i<3;i++)assert(!probe_report.reserved[i]);
 for(i=0;i<120;i++){
  assert(probe_report.events[i].address==read_addr[i]);
  assert(probe_report.events[i].op==(read_width[i]==16?3u:1u));
 }
 for(i=0;i<6;i++)assert(read_addr[34+i]==phy_guard[i]);
 for(ch=0;ch<2;ch++){
  for(i=0;i<NCTL;i++)assert(read_addr[n++]==0x0a110000+ch*0x400000+ctl_offsets[i]);
  for(i=0;i<NTIMING;i++)assert(read_addr[n++]==0x0a100000+ch*0x400000+timing_offsets[i]);
 }
 assert(n==80);
 for(i=0;i<40;i++)assert(read_addr[80+i]==read_addr[i] && read_width[80+i]==read_width[i]);
 for(i=120;i<CAP;i++)assert(!probe_report.events[i].op && !probe_report.events[i].address && !probe_report.events[i].value);
}
int main(int argc,char **argv){
 uint32_t i,v,old_reads,old_progress;FILE *f;
 reset();verify_success();assert(argc==2);f=fopen(argv[1],"wb");assert(f);
 assert(fwrite((const void *)&probe_report,1,sizeof(probe_report),f)==sizeof(probe_report));assert(!fclose(f));
 for(i=1;i<=120;i++){
  reset();fail_progress=i;assert(probe_main()==-1);
  assert(probe_report.uart_error && reads==i-1 && probe_report.completed==i-1 && probe_report.attempted==i && !probe_report.writes);
 }
 for(i=1;i<=120;i++)if(i<=40 || i>=81){
  reset();drift_at=i;assert(probe_main()==-1);assert(reads==i && probe_report.completed==i && !probe_report.writes);
 }
 /* Observations have no invented expected presets, including MRSTAT inside
  * the middle sample group; guards still independently check before/after. */
 for(i=41;i<=80;i++){reset();drift_at=i;verify_success();}
 reset();observation_xor=0xffffffffu;verify_success();
 old_reads=reads;old_progress=progress_count;
 assert(sample(2,0x0a110ca4,&v));assert(sample(4,0x0a110080,&v));
 assert(sample(3,0x0a110ca4,&v));assert(sample(1,0x0aaa0000,&v));
 assert(sample(1,0x0a110ca5,&v));assert(sample(3,0x0aaa0133,&v));
 assert(sample(1,0x0a110ca0,&v));assert(sample(1,0x0a110d08,&v));
 assert(sample(3,0x0aa801e2,&v));assert(sample(1,0x40000000,&v));
 assert(reads==old_reads && progress_count==old_progress);
 probe_report.completed=CAP;assert(sample(1,0x0a110ca4,&v));
 assert(reads==old_reads && progress_count==old_progress);
 printf("PASS: 120 ordered read-only events; 40 init/MR/timing words; 120 UART faults; 80 guard drifts; 40 unconstrained observation drifts; exact widths and forbidden write/program/DDR rejection\n");
 return 0;
}
