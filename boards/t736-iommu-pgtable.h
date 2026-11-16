/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T736_IOMMU_PGTABLE_H
#define T736_IOMMU_PGTABLE_H

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#define T736_IOMMU_PAGE_SIZE 4096UL
#define T736_IOMMU_PD_SIZE 16384UL
#define T736_IOMMU_PT_ENTRIES 1024U
#define T736_IOMMU_PD_ENTRIES 4096U
/* The hardware supports 34-bit VA, but the Cedar userspace ABI is 32-bit. */
#define T736_IOMMU_USED_PD_ENTRIES 1024U

struct t736_iommu_l2 {
	u32 *cpu;
	dma_addr_t dma;
	bool published;
};

struct t736_iommu_pgtable {
	struct device *dev;
	u32 *pd_cpu;
	dma_addr_t pd_dma;
	struct t736_iommu_l2 *l2;
	spinlock_t lock;
	bool hardware_attached;
};

int t736_iommu_pgtable_init(struct t736_iommu_pgtable *table, struct device *dev);
/* Caller must first switch the hardware TTB away and prove/quiesce walkers. */
int t736_iommu_pgtable_destroy(struct t736_iommu_pgtable *table);
int t736_iommu_pgtable_map(struct t736_iommu_pgtable *table, unsigned long iova,
			 phys_addr_t physical, size_t page_count, int prot,
			 size_t *mapped);
size_t t736_iommu_pgtable_unmap(struct t736_iommu_pgtable *table,
			      unsigned long iova, size_t page_count);
phys_addr_t t736_iommu_pgtable_translate(struct t736_iommu_pgtable *table,
				       dma_addr_t iova);
void t736_iommu_pgtable_clear(struct t736_iommu_pgtable *table);

#endif
