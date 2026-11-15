// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define T736_PHY_RELEASE_NATIVE
#include "probe.c"
static uint32_t ctl[2][11],sys,clock_ticks,stall,fail_progress,progress_count,actual_writes,owner;
static uint32_t seen_ops[CAP],seen_addr[CAP];
uint32_t test_counter(void){if(!stall)clock_ticks+=24;return clock_ticks;}
int test_progress(unsigned int op,uint32_t a){assert(progress_count<CAP);seen_ops[progress_count]=op;seen_addr[progress_count]=a;progress_count++;return fail_progress==progress_count?-1:0;}
uint32_t test_read(uint32_t a,unsigned int width){uint32_t i,ch;
 for(i=0;i<12;i++)if(a==ca[i]){assert(width==32);return i==3?sys:cv[i];}
 for(ch=0;ch<2;ch++)for(i=0;i<11;i++)if(a==0xa110000+ch*0x400000+offsets[i]){assert(width==32);return ctl[ch][i];}
 assert(width==16);if(a==0xaaa0000)return owner;assert(a==0xaaa0132);return 9;
}
void test_write(uint32_t a,uint32_t v){uint32_t ch,i;actual_writes++;
 if(a==0xa020010){assert(ctl[0][4]==0x10014&&ctl[1][4]==0x10014&&ctl[0][3]==0x800&&ctl[1][3]==0x800);assert(v==(sys==0x2728?0x273c:sys==0x273c?0x273e:0x273f));sys=v;return;}
 for(ch=0;ch<2;ch++)for(i=3;i<=8;i++)if(a==0xa110000+ch*0x400000+offsets[i]){
  assert(sys==0x2728);assert(i==8||ctl[ch][8]==0);assert(i==8?(v<=1):(v==held[i]));ctl[ch][i]=v;if(i==8)ctl[ch][9]=v;return;}
 assert(!"unapproved write");
}
static void reset(void){memcpy(ctl[0],initial,sizeof(initial));memcpy(ctl[1],initial,sizeof(initial));sys=0x2728;clock_ticks=stall=fail_progress=progress_count=actual_writes=owner=0;}
int main(int argc,char **argv){uint32_t i,n;FILE *f;reset();assert(!probe_main());assert(probe_report.status==2&&actual_writes==17);n=progress_count;assert(argc==2);f=fopen(argv[1],"wb");assert(f);assert(fwrite((const void *)&probe_report,1,sizeof(probe_report),f)==sizeof(probe_report));assert(!fclose(f));
 for(i=1;i<=n;i++){reset();fail_progress=i;assert(probe_main()==-1);assert(probe_report.uart_error&&probe_report.completed==i-1);assert(probe_report.attempted==i);}
 reset();stall=1;assert(probe_main()==-1);assert(!actual_writes);
 reset();owner=1;assert(!probe_main());assert(probe_report.status==3);assert(seen_addr[progress_count-1]==0xaaa0000);
 for(i=0;i<22;i++){reset();ctl[i/11][i%11]^=1;assert(probe_main()==-1);assert(!actual_writes);}
 printf("PASS: %u ordered MMIO operations, 17 fixed writes, all UART fault positions, 22 precondition drifts, stalled timer, owner refusal\n",n);return 0;
}
