#!/usr/bin/env python3
"""Execute actual Cedar WC C helpers under ASan/UBSan; no hardware access."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def extract(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth = 1
    pos = brace + 1
    while depth:
        depth += (text[pos] == '{') - (text[pos] == '}')
        pos += 1
    return text[start:pos]


PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint64_t dma_addr_t;
typedef int irqreturn_t;
typedef int atomic_t;
#define __user
#define BIT(n) (1u << (n))
#define READ_ONCE(x) (x)
#define IRQ_NONE 0
#define IRQ_HANDLED 1
#define GFP_KERNEL 0
#define VE_ALLOC_ATTRS 1
#define PAGE_SHIFT 12
#define ERR_PTR(x) ((void *)(intptr_t)(x))
#define PTR_ERR(x) ((intptr_t)(x))
#define IS_ERR(x) ((uintptr_t)(x) >= (uintptr_t)-4095)
#define check_add_overflow(a,b,p) __builtin_add_overflow((a),(b),(p))
enum dma_data_direction { DMA_BIDIRECTIONAL, DMA_TO_DEVICE, DMA_FROM_DEVICE, DMA_NONE };
struct device { int unused; };
struct file { int refs; };
struct mutex { int unused; };
struct t736_ve {
    struct device *dev;
    void *regs;
    bool powered, poisoned;
    int mode, pending, vcu_pending, wait;
    struct mutex lock;
};
struct ve_buffer { struct t736_ve *ve; void *cpu; dma_addr_t dma; size_t size; };
struct dma_buf { void *priv; struct file *file; size_t size; };
struct dma_buf_attachment { struct dma_buf *dmabuf; struct device *dev; };
struct scatterlist { u64 physical; unsigned int length; u64 dma; unsigned int dma_len; };
struct sg_table { struct scatterlist *sgl; unsigned int orig_nents, nents; };
struct ve_mapping { struct dma_buf *buffer; struct ve_mapping *next; };
struct list_head { struct ve_mapping *first; };
struct ve_file { struct t736_ve *ve; struct list_head mappings; };
struct ve_cache_arg { u64 start, end; };
struct vm_area_struct { unsigned long vm_start, vm_end, vm_pgoff; struct file *vm_file; };
struct task { void *mm; } task = { (void *)1 }, *current = &task;
static int barriers, wakes, mmap_locked, ve_locked, map_error, free_count;
static struct vm_area_struct fixture_vma;
static struct scatterlist fixture_sg[4];
static unsigned int fixture_nents;
#define list_for_each_entry(m,h,member) for ((m)=(h)->first;(m);(m)=(m)->next)
#define for_each_sg(sgl,sg,n,i) for ((i)=0,(sg)=(sgl);(i)<(n);(i)++,(sg)++)
#define sg_phys(sg) ((sg)->physical)
#define sg_dma_address(sg) ((sg)->dma)
#define sg_dma_len(sg) ((sg)->dma_len)
static void *kzalloc(size_t n,int flags) { return calloc(1,n); }
static void kfree(void *p) { free(p); }
static int dma_get_sgtable_attrs(struct device *d,struct sg_table *t,void *cpu,u64 dma,size_t size,int attrs) {
    if (map_error) return map_error;
    t->orig_nents=fixture_nents;
    t->sgl=calloc(fixture_nents,sizeof(*t->sgl));
    memcpy(t->sgl,fixture_sg,fixture_nents*sizeof(*t->sgl));
    return 0;
}
static void sg_free_table(struct sg_table *t) { free(t->sgl); free_count++; }
static void dma_wmb(void) { barriers++; }
static void mb(void) { barriers++; }
static u32 readl(void *p) { return *(u32 *)p; }
static void writel(u32 v,void *p) { *(u32 *)p=v; }
static void atomic_set(atomic_t *p,int v) { *p=v; }
static void wake_up_all(int *q) { wakes++; }
static int copy_from_user(void *dst,void *src,size_t size) { memcpy(dst,src,size); return 0; }
static void mmap_read_lock(void *m) { assert(!ve_locked); mmap_locked++; }
static void mmap_read_unlock(void *m) { assert(mmap_locked==1); mmap_locked--; }
static void mutex_lock(struct mutex *m) { assert(!mmap_locked); ve_locked++; }
static void mutex_unlock(struct mutex *m) { assert(ve_locked==1); ve_locked--; }
static struct vm_area_struct *find_vma(void *m,u64 start) { return &fixture_vma; }
static struct file *get_file(struct file *f) { f->refs++; return f; }
static void fput(struct file *f) { assert(f->refs>0); f->refs--; }
'''

TESTS = r'''
int main(void) {
    struct device dev={0}, foreign={0};
    u32 registers[1024]={0};
    struct t736_ve ve={.dev=&dev,.regs=registers,.powered=true,.mode=2};
    struct ve_buffer b={.ve=&ve,.cpu=(void *)1,.dma=0x80000000,.size=8192};
    struct file file={0}, bad_file={0};
    struct dma_buf db={.priv=&b,.file=&file,.size=8192};
    struct dma_buf_attachment a={.dmabuf=&db,.dev=&dev};
    struct sg_table *t;
    struct ve_mapping mapping={.buffer=&db};
    struct ve_file f={.ve=&ve,.mappings={&mapping}};
    struct ve_cache_arg range={0x100000,0x102000};
    int saved;

    fixture_nents=2;
    fixture_sg[0]=(struct scatterlist){.physical=0x80000000,.length=4096};
    fixture_sg[1]=(struct scatterlist){.physical=0x80001000,.length=4096};
    t=ve_buffer_map(&a,DMA_BIDIRECTIONAL);
    assert(!IS_ERR(t) && t->nents==2 && t->sgl[1].dma==0x80001000 && t->sgl[1].dma_len==4096);
    sg_free_table(t); free(t);
    fixture_sg[1].physical+=4096; /* discontiguous physical extent */
    assert(PTR_ERR(ve_buffer_map(&a,DMA_BIDIRECTIONAL))==-ERANGE);
    fixture_sg[1].physical-=4096;
    fixture_sg[1].length=8192; /* full extent larger than buffer */
    assert(PTR_ERR(ve_buffer_map(&a,DMA_BIDIRECTIONAL))==-ERANGE);
    fixture_sg[1].length=2048; /* incomplete extent */
    assert(PTR_ERR(ve_buffer_map(&a,DMA_BIDIRECTIONAL))==-ERANGE);
    fixture_sg[1].length=0;
    assert(PTR_ERR(ve_buffer_map(&a,DMA_BIDIRECTIONAL))==-ERANGE);
    fixture_sg[1].length=4096;
    a.dev=&foreign;
    assert(PTR_ERR(ve_buffer_map(&a,DMA_BIDIRECTIONAL))==-EINVAL);
    a.dev=&dev;
    assert(PTR_ERR(ve_buffer_map(&a,DMA_NONE))==-EINVAL);
    map_error=-ENOMEM;
    assert(PTR_ERR(ve_buffer_map(&a,DMA_BIDIRECTIONAL))==-ENOMEM);
    map_error=0;

    fixture_vma=(struct vm_area_struct){.vm_start=0x100000,.vm_end=0x102000,.vm_file=&file};
    saved=barriers;
    assert(ve_cache_ioctl(&f,(unsigned long)&range)==0 && barriers==saved+1 && file.refs==0);
    range.end=range.start;
    assert(ve_cache_ioctl(&f,(unsigned long)&range)==-EINVAL);
    range.end=0x102001;
    assert(ve_cache_ioctl(&f,(unsigned long)&range)==-EINVAL);
    range.end=0x102000;
    fixture_vma.vm_file=&bad_file;
    assert(ve_cache_ioctl(&f,(unsigned long)&range)==-EINVAL && bad_file.refs==0);
    fixture_vma.vm_file=&file;
    fixture_vma.vm_pgoff=1; /* matching file, extent exceeds allocation */
    assert(ve_cache_ioctl(&f,(unsigned long)&range)==-EINVAL);
    fixture_vma.vm_pgoff=0;
    ve.poisoned=true;
    assert(ve_cache_ioctl(&f,(unsigned long)&range)==-EIO && file.refs==0);
    ve.poisoned=false;
    assert(!mmap_locked && !ve_locked);

    registers[0]=1; registers[0x220/4]=0xff; registers[0x228/4]=9;
    assert(ve_irq(0,&ve)==IRQ_HANDLED && ve.pending==1);
    assert(registers[0x220/4]==0xf0 && registers[0x228/4]==9); /* status retained */
    ve.pending=0;
    assert(ve_irq(0,&ve)==IRQ_NONE && !ve.pending); /* masked old completion */
    registers[0]=4; registers[0x530/4]=0x8000000f; registers[0x538/4]=1;
    assert(ve_irq(0,&ve)==IRQ_HANDLED && registers[0x530/4]==0x80000000);
    registers[0x530/4]=0xf; registers[0x538/4]=0; ve.pending=0;
    assert(ve_irq(0,&ve)==IRQ_NONE && !ve.pending);
    ve.powered=false;
    assert(ve_irq(0,&ve)==IRQ_NONE);
    ve.powered=true; ve.mode=4; registers[0x05c/4]=4; registers[0x628/4]=0;
    assert(ve_irq(0,&ve)==IRQ_NONE && !ve.vcu_pending); /* no fabricated VCU IRQ */
    registers[0x628/4]=0x81;
    assert(ve_irq(0,&ve)==IRQ_HANDLED && ve.vcu_pending==1 && registers[0x628/4]==1);
    puts("PASS: actual C DMA extent, cache VMA ownership, and decoder IRQ tests");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'boards/t736-cedar-wc.c')
    args = parser.parse_args()
    source = args.source.read_text()
    functions = [extract(source, name) for name in (
        'static struct sg_table *ve_buffer_map(',
        'static long ve_cache_ioctl(',
        'static irqreturn_t ve_irq(')]
    with tempfile.TemporaryDirectory(prefix='t736-cedar-wc-') as directory:
        c = Path(directory) / 'test.c'
        exe = Path(directory) / 'test'
        c.write_text(PRELUDE + '\n'.join(functions) + TESTS)
        subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', str(c), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
