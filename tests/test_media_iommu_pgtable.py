#!/usr/bin/env python3
"""Host-test actual donor page-table functions at L1 and 4 GiB boundaries.

This tests arithmetic and table coverage only. Mocks intentionally cannot test
DMA visibility, hardware invalidation, power sequencing or kernel lifetimes.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def function(source, declaration):
    start = source.index(declaration)
    body = source.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        if source[end] == '{':
            depth += 1
        elif source[end] == '}':
            depth -= 1
        end += 1
    return source[start:end] + '\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('source', type=Path, help='Actual sunxi-iommu-pgtable.c under review')
    ap.add_argument('--cc', default='cc')
    args = ap.parse_args()
    source = args.source.read_text()
    functions = ''.join(function(source, declaration) for declaration in (
        'static inline u32 sunxi_mk_pte(',
        'int sunxi_pgtable_prepare_l1_tables(',
        'int sunxi_pgtable_prepare_l2_tables('))
    prefix = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint64_t phys_addr_t;
typedef uint64_t dma_addr_t;
#define BIT(n) (1UL << (n))
#define AW_IOMMU_PGTABLE_V2
#define IOMMU_READ 1
#define IOMMU_WRITE 2
#define IOMMU_PT_MASK (~4095UL)
#define SPAGE_SIZE 4096UL
#define SPD_SIZE (4UL * 1024 * 1024)
#define NUM_ENTRIES_PTE 1024
#define IOPTE_INDEX(v) (((v) >> 12) & 1023)
#define SUNXI_PTE_PAGE_WRITABLE BIT(3)
#define SUNXI_PTE_PAGE_READABLE BIT(2)
#define SUNXI_PTE_PAGE_VALID BIT(1)
#define SUNXI_PTE_PAGE_ADDR_HIGH_SHIFT 8
#define SUNXI_PTE_PAGE_ADDR_HIGH_MASK (3 << 8)
#define IS_VALID(v) (((v) & 3) == 1)
#define round_down(v, step) ((v) & ~((step) - 1))
#define DMA_TO_DEVICE 1
#define WARN_ON(v) do { if (v) abort(); } while (0)
#define virt_to_phys(p) ((uintptr_t)(p))
static u32 pd[4], pt[4][1024];
static struct { void *dma_dev; } sunxi_pgtable_params;
static u32 *iopde_offset(u32 *table, dma_addr_t iova)
{ return table + iova / SPD_SIZE; }
static u32 *iopte_offset(u32 *dent, dma_addr_t iova)
{ return &pt[dent-pd][IOPTE_INDEX(iova)]; }
static int sunxi_alloc_iopte(u32 *dent, int prot)
{ (void)prot; *dent = 1; return 1; }
static phys_addr_t cpu_phy_to_iommu_phy(phys_addr_t pa)
{ return pa > (1ULL << 34) ? pa - (1ULL << 34) : pa; }
static void dma_sync_single_for_device(void *dev, uintptr_t addr, size_t size, int direction)
{ (void)dev; (void)addr; (void)size; (void)direction; }
'''
    suffix = r'''
static int check(const char *name, int ok)
{ printf("%s: %s\n", name, ok ? "PASS" : "FAIL"); return !ok; }
int main(void)
{
    int fail = 0;
    memset(pd, 0, sizeof(pd));
    sunxi_pgtable_prepare_l1_tables(pd, SPD_SIZE - SPAGE_SIZE,
                                   SPD_SIZE + SPAGE_SIZE, IOMMU_READ | IOMMU_WRITE);
    fail += check("L1 crossing includes both directories", IS_VALID(pd[0]) && IS_VALID(pd[1]) && !pd[2]);
    memset(pd, 0, sizeof(pd));
    sunxi_pgtable_prepare_l1_tables(pd, 0, SPD_SIZE, IOMMU_READ | IOMMU_WRITE);
    fail += check("L1 exact end excludes next directory", IS_VALID(pd[0]) && !pd[1]);
    memset(pt, 0, sizeof(pt));
    sunxi_pgtable_prepare_l2_tables(pd, 0, 2 * SPAGE_SIZE, 0xfffff000ULL,
                                   IOMMU_READ | IOMMU_WRITE);
    fail += check("PTE 4GiB physical carry", pt[0][0] == 0xfffff00eU && pt[0][1] == 0x0000010eU);
    memset(pt, 0, sizeof(pt));
    sunxi_pgtable_prepare_l2_tables(pd, 0, 2 * SPAGE_SIZE, 0x1fffff000ULL,
                                   IOMMU_READ | IOMMU_WRITE);
    fail += check("PTE 8GiB physical carry", pt[0][0] == 0xfffff10eU && pt[0][1] == 0x0000020eU);
    return fail ? 1 : 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='t736-iommu-pgtable-') as temporary:
        test = Path(temporary) / 'pgtable-test.c'
        binary = Path(temporary) / 'pgtable-test'
        test.write_text(prefix + functions + suffix)
        subprocess.run([args.cc, '-std=c11', '-Wall', '-Wextra', '-Wno-sign-compare',
                        str(test), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)])
        raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
