// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define T736_CONTROL_STATE_NATIVE
#include "probe.c"
static uint32_t reads, fail_uart, writes_uart, drift, drains, fail_drain;
uint32_t control_test_read(uint32_t address) {
 uint32_t i;
 reads++;
 for(i=0;i<6;i++)if(address==context_address[i])return context_value[i]^(drift==i+1?1u:0u);
 if(address==0xa110000 || address==0xa510000)return 0x03081002;
 if(address==0xa110014 || address==0xa110090 || address==0xa510014 || address==0xa510090)return 0;
 for(i=0;i<NCONTROL;i++)if(address==0xa110000+control_offset[i] || address==0xa510000+control_offset[i])return address^0x1234;
 assert(!"unexpected read");return 0;
}
int control_test_putc(unsigned int value){assert(value<128);writes_uart++;return fail_uart==writes_uart?-1:0;}
int control_test_drain(void){drains++;return fail_drain==drains?-1:0;}
static void reset(void){reads=fail_uart=writes_uart=drift=drains=fail_drain=0;}
int main(int argc,char **argv){
 uint32_t i;FILE *f;
 reset();assert(!probe_main());assert(reads==NWORDS);assert(probe_report.completed==NWORDS);assert(writes_uart==NWORDS*12);
 assert(argc==2);f=fopen(argv[1],"wb");assert(f);assert(fwrite((const void *)&probe_report,1,sizeof(probe_report),f)==sizeof(probe_report));assert(!fclose(f));
 for(i=1;i<=NWORDS*12;i++){reset();fail_uart=i;assert(probe_main()==-1);assert(probe_report.uart_error);assert(reads==(i-1)/12);}
 for(i=1;i<=NWORDS;i++){reset();fail_drain=i;assert(probe_main()==-1);assert(probe_report.uart_error);assert(reads==i-1);}
 for(i=1;i<=6;i++){reset();drift=i;assert(probe_main()==-1);assert(reads==i);assert(probe_report.status==0x101);}
 puts("PASS: 58 fixed controller/context reads, every 696 UART failure position stops before MMIO, six context drifts");return 0;
}
