// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define T736_PHY_ZCAL_RAWCODE_NATIVE
#include "probe.c"
static uint32_t reads,progress_count,fail_progress,drift_at,clock_value,clock_drift,unknown[3];
static uint32_t seen_ops[CAP],seen_addr[CAP],read_addr[CAP],read_width[CAP];
int test_progress(unsigned int op,uint32_t a){
 assert(progress_count<CAP);assert(op==1 || op==3);
 seen_ops[progress_count]=op;seen_addr[progress_count]=a;progress_count++;
 return fail_progress==progress_count?-1:0;
}
uint32_t test_read(uint32_t a,unsigned int width){
 uint32_t i,ch,v=0;int found=0;
 assert(reads<CAP);read_addr[reads]=a;read_width[reads]=width;reads++;
 assert(progress_count==reads && seen_addr[reads-1]==a && (seen_ops[reads-1]==3?16u:32u)==width);
 for(i=0;i<12;i++)if(a==ca[i]){assert(width==32);v=cv[i];found=1;}
 for(ch=0;ch<2;ch++)for(i=0;i<11;i++)if(a==0xa110000+ch*0x400000+offsets[i]){assert(width==32);v=held[i];found=1;}
 if(a==0x0aaa0000){assert(width==16);v=0;found=1;}
 if(a==0x0aaa0132){assert(width==16);v=1;found=1;}
 if(a==0x0aa80100){assert(width==16);v=3;found=1;}
 for(i=0;i<NSTATIC;i++)if(a==phy_static[i]){assert(width==16);v=static_values[i];found=1;}
 if(a==extra[0]){assert(width==16);v=clock_value^(reads>79?clock_drift:0);found=1;}
 if(a==extra[1]){assert(width==16);v=0;found=1;}
 if(a==extra[2]){assert(width==16);v=1;found=1;}
 for(i=0;i<3;i++)if(a==observations[i]){assert(width==16);v=unknown[i];found=1;}
 assert(found);return v^(drift_at==reads?1u:0u);
}
static void reset(void){reads=progress_count=fail_progress=drift_at=clock_drift=0;clock_value=450;
 unknown[0]=0x2222;unknown[1]=0x55aa;unknown[2]=0xaa55;
 memset(seen_ops,0,sizeof(seen_ops));memset(seen_addr,0,sizeof(seen_addr));memset(read_addr,0,sizeof(read_addr));memset(read_width,0,sizeof(read_width));}
static void verify_success(void){
 uint32_t i;
 assert(!probe_main());assert(probe_report.status==2 && probe_report.phase==4);
 assert(reads==155 && progress_count==155 && probe_report.attempted==155 && probe_report.completed==155);
 assert(!probe_report.writes && !probe_report.uart_error);
 for(i=0;i<3;i++)assert(!probe_report.reserved[i]);
 for(i=0;i<155;i++){assert(probe_report.events[i].address==read_addr[i]);assert(probe_report.events[i].op==(read_width[i]==16?3u:1u));}
 assert(read_addr[34]==0x0aaa0000 && read_addr[35]==0x0aaa0132 && read_addr[36]==0x0aa80100);
 for(i=0;i<NSTATIC;i++)assert(read_addr[37+i]==phy_static[i]);
 for(i=0;i<3;i++)assert(read_addr[73+i]==extra[i] && read_addr[76+i]==observations[i]);
 for(i=0;i<76;i++)assert(read_addr[79+i]==read_addr[i]);
 for(i=155;i<CAP;i++)assert(!probe_report.events[i].op && !probe_report.events[i].address && !probe_report.events[i].value);
}
int main(int argc,char **argv){
 uint32_t i,v,old_reads,old_progress;FILE *f;
 reset();verify_success();assert(argc==2);f=fopen(argv[1],"wb");assert(f);
 assert(fwrite((const void *)&probe_report,1,sizeof(probe_report),f)==sizeof(probe_report));assert(!fclose(f));
 for(i=1;i<=155;i++){
  reset();fail_progress=i;assert(probe_main()==-1);
  assert(probe_report.uart_error && reads==i-1 && probe_report.completed==i-1 && probe_report.attempted==i && !probe_report.writes);
 }
 for(i=1;i<=155;i++)if(i<=76 || i>=80){
  reset();drift_at=i;assert(probe_main()==-1);assert(reads==i && probe_report.completed==i && !probe_report.writes);
 }
 reset();clock_value=0xf800u|450u;verify_success();
 reset();clock_drift=0x800;assert(probe_main()==-1 && reads==153 && probe_report.phase==3);
 /* Candidate values are raw observations, including values unlike1111. */
 reset();unknown[0]=unknown[1]=unknown[2]=0;verify_success();
 reset();unknown[0]=unknown[1]=unknown[2]=0xffff;verify_success();
 old_reads=reads;old_progress=progress_count;
 assert(sample(2,0x0a940620,&v));assert(sample(4,0x0aaa0132,&v));
 assert(sample(1,0x0a940628,&v));assert(sample(3,0x02002020,&v));
 assert(sample(3,0x0aa801e2,&v));assert(sample(3,0x0aa8010c,&v));
 assert(sample(3,0x0aa80242,&v));assert(sample(3,0x40000000,&v));
 assert(reads==old_reads && progress_count==old_progress);
 printf("PASS: 155 read-only events; 155 UART faults; 152 pre/post guard drifts; full clock high-bit stability; arbitrary three candidate values; strict widths and forbidden write/program/DDR rejection\n");
 return 0;
}
