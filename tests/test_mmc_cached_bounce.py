#!/usr/bin/env python3
"""Compile actual driver DMA/command/remove functions against an ownership model."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
driver = (ROOT / 'boards/t736_mmc_cached_bounce_draft.c').read_text()
names = ('t736_payload_for_device', 't736_dma_stop', 't736_dma_start',
         't736_command', 't736_remove')
functions = '\n\n'.join(re.search(
    r'static (?:int|void) ' + name + r'\(.*?\n}', driver, re.S).group()
    for name in names)
registers = '\n'.join(line for line in driver.splitlines()
                      if line.startswith('#define ') and
                      line.split()[1] in {
                          'CTRL','CLKDIV','TIMEOUT','WIDTH','BLOCKSIZE','BYTECOUNT',
                          'COMMAND','ARGUMENT','RESPONSE','IRQSTATUS','STATUS',
                          'FIFOTHRESHOLD','CARD_THRESHOLD','CARD_THRESHOLD_MASK',
                          'DMAC','DLBA','IDST','IDIE','CTRL_DMA','CTRL_CPU',
                          'DMA_RESET','DMA_ENABLE','DMA_STATUS_CLEAR','CMD_START'})
PREFIX = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32; typedef uint64_t u64; typedef uint64_t dma_addr_t;
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((~0U << (l)) & (~0U >> (31-(h))))
#define MAX_REQUEST_BYTES 65536
#define T736_DMA_DESCS 64
#define T736_DMA_ERRORS BIT(4)
#define MMC_DATA_WRITE BIT(8)
#define MMC_DATA_READ BIT(9)
#define MMC_RSP_PRESENT BIT(0)
#define MMC_RSP_136 BIT(1)
#define MMC_RSP_CRC BIT(2)
#define MMC_RSP_BUSY BIT(3)
#define MMC_TIMING_MMC_HS200 9
#define MMC_TIMING_MMC_HS400 10
#define MMC_SEND_TUNING_BLOCK_HS200 21
#define MMC_SEND_EXT_CSD 8
#define DMA_BIDIRECTIONAL 0
struct device { int unused; };
struct mmc_host { struct device *parent; void *priv; struct { int timing; } ios; };
struct mmc_command { u32 opcode,flags,arg,resp[4]; int error; };
struct mmc_data { u32 flags,blksz,blocks; void *sg; int sg_len; void *stop; };
struct t736_dma_desc { u32 value; };
struct work_struct { int unused; };
#include "t736_mmc_timing_stats_draft.h"
struct t736_host {
    struct mmc_host *mmc;
    void *regs, *buffer;
    dma_addr_t buffer_dma, desc_dma;
    struct t736_dma_desc *desc;
    unsigned desc_count;
    bool dma_dead, payload_device_owned, tuning_running, tuning_error_reported;
    int ios_error;
    u32 tuning_opcode, last_irq, last_idst;
    u64 dma_requests, dma_bytes;
    struct t736_diag_sample *active_sample;
    struct work_struct work;
    void *workqueue, *timing_debugfs;
};
struct platform_device { struct device dev; struct mmc_host *drvdata; };
#define mmc_dev(m) ((m)->parent)
#define mmc_priv(m) ((m)->priv)
#define platform_get_drvdata(p) ((p)->drvdata)
static struct t736_host h;
static struct mmc_host mmc;
static struct platform_device pdev;
static unsigned char cpu_cache[MAX_REQUEST_BYTES], device_memory[MAX_REQUEST_BYTES];
static unsigned char sg_memory[MAX_REQUEST_BYTES], expected[MAX_REQUEST_BYTES];
static struct t736_dma_desc descriptors[T736_DMA_DESCS];
static u32 registers[256];
static unsigned reset_polls, fail_reset_at, sync_device_calls, sync_cpu_calls;
static unsigned copies_to, copies_from, free_payloads, free_descriptors;
static unsigned critical_logs, error_logs, info_logs;
static unsigned short_copy, device_read_limit;
static int command_result;
static bool engine_active;
static unsigned long jiffies;
'''
MOCKS = r'''
static u32 readl(void *p) { return *(u32 *)p; }
static void writel(u32 value, void *p)
{
    unsigned offset=(unsigned char *)p-(unsigned char *)h.regs;
    if(offset==IDST || offset==IRQSTATUS) *(u32 *)p &= ~value;
    else *(u32 *)p=value;
    if(offset==DMAC && value==DMA_ENABLE) {
        assert(h.payload_device_owned && sync_device_calls);
        engine_active=true;
    }
    if(offset==COMMAND && (value & BIT(9))) {
        unsigned bytes=registers[BYTECOUNT/4];
        assert(engine_active && h.payload_device_owned && bytes<=MAX_REQUEST_BYTES);
        if(value & BIT(10)) assert(!memcmp(device_memory,sg_memory,bytes));
        else memcpy(device_memory,expected,bytes<device_read_limit?bytes:device_read_limit);
    }
}
static int fake_poll(void *addr, unsigned delay, unsigned timeout)
{
    assert(delay==10);
    if(addr==(void *)((unsigned char *)h.regs+STATUS)) {
        assert(timeout==2000000);return 0;
    }
    assert(timeout==100000);
    reset_polls++;
    if(reset_polls==fail_reset_at) return -ETIMEDOUT;
    if(reset_polls%3==0) engine_active=false;
    return 0;
}
#define readl_poll_timeout(addr,value,condition,delay,timeout) \
    ({ int result=fake_poll(addr,delay,timeout); value=0; if(!result) assert(condition); result; })
static void dma_sync_single_for_device(struct device *dev,dma_addr_t dma,size_t bytes,int direction)
{
    assert(dev==&pdev.dev && dma==h.buffer_dma && bytes==MAX_REQUEST_BYTES);
    assert(direction==DMA_BIDIRECTIONAL && !engine_active && !h.dma_dead && !h.payload_device_owned);
    memcpy(device_memory,cpu_cache,bytes);sync_device_calls++;
}
static void dma_sync_single_for_cpu(struct device *dev,dma_addr_t dma,size_t bytes,int direction)
{
    assert(dev==&pdev.dev && dma==h.buffer_dma && bytes==MAX_REQUEST_BYTES);
    assert(direction==DMA_BIDIRECTIONAL && !engine_active && !h.dma_dead && h.payload_device_owned);
    assert(reset_polls && reset_polls%3==0);
    memcpy(cpu_cache,device_memory,bytes);sync_cpu_calls++;
}
static size_t sg_copy_to_buffer(void *sg,int count,void *buffer,size_t bytes)
{
    (void)count;assert(!h.payload_device_owned && !h.dma_dead && buffer==cpu_cache);
    copies_to++;memcpy(buffer,sg,short_copy?bytes-4:bytes);return short_copy?bytes-4:bytes;
}
static size_t sg_copy_from_buffer(void *sg,int count,void *buffer,size_t bytes)
{
    (void)count;assert(!h.payload_device_owned && !h.dma_dead && buffer==cpu_cache);
    copies_from++;memcpy(sg,buffer,bytes);return bytes;
}
static int t736_dma_describe(struct t736_dma_desc *desc,dma_addr_t d,dma_addr_t b,unsigned bytes)
{
    assert(desc==descriptors && d==h.desc_dma && b==h.buffer_dma);
    assert(!h.payload_device_owned && bytes<=MAX_REQUEST_BYTES);return (bytes+1023)/1024;
}
static int t736_dma_poll_result(u32 status,u32 dma_status,bool data,bool write,bool stop,struct t736_dma_desc *desc,unsigned count)
{
    (void)status;(void)dma_status;(void)data;(void)write;(void)stop;(void)desc;(void)count;
    return command_result;
}
static bool t736_tuning_idst_invalid(u32 value) { (void)value;return false; }
static void t736_dma_snapshot(struct t736_host *host,struct mmc_command *cmd)
{ (void)host;(void)cmd;error_logs++; }
static struct t736_diag_mark t736_diag_begin(struct t736_host *host)
{ (void)host;return (struct t736_diag_mark){0}; }
static void t736_diag_end(struct t736_host *host,enum t736_diag_segment seg,struct t736_diag_mark mark)
{ (void)host;(void)seg;(void)mark; }
#define dma_wmb() do { assert(h.payload_device_owned); } while(0)
#define dma_rmb() do { assert(!h.payload_device_owned && !h.dma_dead); } while(0)
#define dev_crit(...) do { critical_logs++; } while(0)
#define dev_err_ratelimited(...) do { error_logs++; } while(0)
#define dev_info(...) do { info_logs++; } while(0)
#define msecs_to_jiffies(x) (x)
#define time_after(a,b) ((a)>(b))
static void usleep_range(unsigned a,unsigned b) { assert(a==50 && b==100);jiffies++; }
static void debugfs_remove_recursive(void *p) { (void)p; }
static void mmc_remove_host(struct mmc_host *m) { (void)m; }
static void cancel_work_sync(struct work_struct *w) { (void)w; }
static void destroy_workqueue(void *w) { (void)w; }
static void mmc_free_host(struct mmc_host *m) { (void)m; }
static void dma_free_coherent(struct device *d,size_t bytes,void *p,dma_addr_t dma)
{
    assert(d==&pdev.dev && p==descriptors && dma==h.desc_dma);
    assert(bytes==sizeof(descriptors) && !h.dma_dead && !engine_active);free_descriptors++;
}
static void dma_free_noncoherent(struct device *d,size_t bytes,void *p,dma_addr_t dma,int direction)
{
    assert(d==&pdev.dev && p==cpu_cache && dma==h.buffer_dma && bytes==MAX_REQUEST_BYTES);
    assert(direction==DMA_BIDIRECTIONAL && !h.payload_device_owned && !h.dma_dead && !engine_active);
    free_payloads++;
}
'''
TESTS = r'''
static void reset_fixture(void)
{
    memset(&h,0,sizeof(h));memset(&mmc,0,sizeof(mmc));memset(registers,0,sizeof(registers));
    memset(cpu_cache,0xcc,sizeof(cpu_cache));memset(device_memory,0x77,sizeof(device_memory));
    memset(sg_memory,0,sizeof(sg_memory));
    for(unsigned i=0;i<sizeof(expected);i++)expected[i]=(i*37+11)&255;
    mmc.parent=&pdev.dev;mmc.priv=&h;pdev.drvdata=&mmc;h.mmc=&mmc;
    h.regs=registers;h.buffer=cpu_cache;h.desc=descriptors;h.buffer_dma=0x100000;h.desc_dma=0x200000;
    reset_polls=fail_reset_at=sync_device_calls=sync_cpu_calls=0;
    copies_to=copies_from=free_payloads=free_descriptors=0;
    critical_logs=error_logs=info_logs=short_copy=0;
    device_read_limit=MAX_REQUEST_BYTES;command_result=1;engine_active=false;jiffies=0;
}
static int request(unsigned bytes,bool write,bool tuning)
{
    struct mmc_command cmd={.opcode=write?24:17,.flags=MMC_RSP_PRESENT|MMC_RSP_CRC};
    struct mmc_data data={.blksz=512,.blocks=bytes/512,.flags=write?MMC_DATA_WRITE:MMC_DATA_READ,
                          .sg=sg_memory,.sg_len=1};
    if(tuning) {
        data.blksz=bytes;data.blocks=1;cmd.opcode=bytes==128?MMC_SEND_TUNING_BLOCK_HS200:MMC_SEND_EXT_CSD;
        h.tuning_opcode=cmd.opcode;h.tuning_running=true;mmc.ios.timing=bytes==128?MMC_TIMING_MMC_HS200:MMC_TIMING_MMC_HS400;
    } else if(bytes<512) {data.blksz=bytes;data.blocks=1;}
    return t736_command(&h,&cmd,&data);
}
int main(void)
{
    reset_fixture();assert(!h.payload_device_owned);
    assert(!t736_dma_stop(&h) && sync_cpu_calls==0); /* allocation starts CPU-owned */
    assert(!t736_payload_for_device(&h) && sync_device_calls==1);
    assert(t736_payload_for_device(&h)==-EIO && sync_device_calls==1);
    unsigned char cached=cpu_cache[0];unsigned polls=reset_polls;
    assert(request(128,false,true)==-EIO && cpu_cache[0]==cached && reset_polls==polls);
    assert(!t736_dma_stop(&h) && !h.payload_device_owned && sync_cpu_calls==1);
    for(unsigned bytes=128;bytes<=512;bytes+=384) {
        reset_fixture();assert(request(bytes,false,true)==0);
        assert(!memcmp(sg_memory,expected,bytes) && sync_device_calls==1 && sync_cpu_calls==1);
        memset(sg_memory,0,sizeof(sg_memory));device_read_limit=64;
        assert(request(bytes,false,true)==0);
        assert(!memcmp(sg_memory,expected,64));
        for(unsigned i=64;i<bytes;i++) assert(sg_memory[i]==0xa5); /* stale prior success cannot pass */
        assert(sync_device_calls==2 && sync_cpu_calls==2 && !h.payload_device_owned);
    }
    reset_fixture();memcpy(sg_memory,expected,MAX_REQUEST_BYTES);
    assert(!request(MAX_REQUEST_BYTES,true,false));
    assert(copies_to==1 && copies_from==0 && !memcmp(device_memory,expected,MAX_REQUEST_BYTES));
    assert(sync_device_calls==1 && sync_cpu_calls==1 && !h.payload_device_owned);
    reset_fixture();cpu_cache[MAX_REQUEST_BYTES-1]=0x3e;
    assert(!request(4,false,false) && cpu_cache[MAX_REQUEST_BYTES-1]==0x3e);
    assert(device_memory[MAX_REQUEST_BYTES-1]==0x3e); /* whole allocation, even tiny transfer */
    for(unsigned at=1;at<=6;at++) {
        reset_fixture();fail_reset_at=at;
        assert(request(128,false,true)==-ETIMEDOUT);
        assert(h.dma_dead && h.ios_error==-EIO && critical_logs==1 && sync_cpu_calls==0 && copies_from==0);
        assert(sync_device_calls==(at>3) && h.payload_device_owned==(at>3));
        unsigned syncs=sync_device_calls,polls=reset_polls;
        unsigned char saved[128];memcpy(saved,cpu_cache,sizeof(saved));
        assert(request(128,false,true)==-EIO && !memcmp(cpu_cache,saved,sizeof(saved)));
        assert(sync_device_calls==syncs && reset_polls==polls);
        t736_remove(&pdev);assert(free_payloads==0 && free_descriptors==0 && reset_polls==polls);
    }
    reset_fixture();short_copy=1;
    assert(request(512,true,false)==-EINVAL && copies_to==1);
    assert(sync_device_calls==0 && sync_cpu_calls==0 && !h.payload_device_owned && !h.dma_dead);
    short_copy=0;memcpy(sg_memory,expected,512);assert(!request(512,true,false));
    for(unsigned tuning=0;tuning<2;tuning++) {
        reset_fixture();command_result=-EILSEQ;device_read_limit=64;
        assert(request(512,false,tuning)==-EILSEQ && sync_cpu_calls==1 && !h.payload_device_owned && copies_from==0);
        command_result=1;device_read_limit=0;
        assert(!request(512,false,true));
        for(unsigned i=0;i<512;i++)assert(sg_memory[i]==0xa5);
        t736_remove(&pdev);assert(free_payloads==1 && free_descriptors==1);
    }
    reset_fixture();assert(!t736_payload_for_device(&h));engine_active=true;
    t736_remove(&pdev);assert(sync_cpu_calls==1 && free_payloads==1 && free_descriptors==1);
    reset_fixture();assert(!request(MAX_REQUEST_BYTES,false,false));
    assert(!memcmp(sg_memory,expected,MAX_REQUEST_BYTES));
    t736_remove(&pdev);assert(free_payloads==1 && free_descriptors==1);
    puts("PASS actual C start/command/stop/remove: first handoff, duplicate rejection,128/512 fresh poison,64KiB write/read,4B dirty-tail sync,all6 pre/post reset failures,no dead CPU access/free,short write copy,failed command reclaim and retry freshness");
}
'''
with tempfile.TemporaryDirectory(prefix='t736-cached-bounce-') as temporary:
    path = Path(temporary)
    source, binary = path / 'test.c', path / 'test'
    source.write_text(PREFIX + registers + '\n' + MOCKS + functions + TESTS)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1',
                    '-fsanitize=address,undefined', '-I', str(ROOT / 'boards'),
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
