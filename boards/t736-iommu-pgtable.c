// SPDX-License-Identifier: GPL-2.0-only
/* T736 v20 page-table format from Tina's GPL sunxi-iommu-pgtable.c.
 * CPU pointers and device DMA addresses are deliberately kept separately.
 * L2 allocations remain owned until a detached domain is destroyed, so an
 * unmap never frees memory underneath a hardware walker or cached L1 entry.
 */
#include <linux/errno.h>
#include <linux/iommu.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include "t736-iommu-pgtable.h"

#define T736_PDE_VALID BIT(0)
#define T736_PTE_VALID BIT(1)
#define T736_PTE_READ BIT(2)
#define T736_PTE_WRITE BIT(3)
#define T736_ADDR_HIGH GENMASK(9, 8)
#define T736_ADDR_LOW GENMASK(31, 12)
#define T736_IOVA_MAX DMA_BIT_MASK(32)
#define T736_PHYS_MAX DMA_BIT_MASK(34)

static u32 t736_encode_address(u64 address)
{
	return (address & T736_ADDR_LOW) | ((address >> 24) & T736_ADDR_HIGH);
}

static u64 t736_decode_address(u32 descriptor)
{
	return (descriptor & T736_ADDR_LOW) |
	       ((u64)(descriptor & T736_ADDR_HIGH) << 24);
}

static bool t736_valid_range(u64 start, size_t page_count, u64 limit,
			     size_t *bytes)
{
	return page_count && IS_ALIGNED(start, T736_IOMMU_PAGE_SIZE) &&
	       !check_mul_overflow(page_count, T736_IOMMU_PAGE_SIZE, bytes) &&
	       start <= limit && *bytes - 1 <= limit - start;
}

int t736_iommu_pgtable_init(struct t736_iommu_pgtable *table, struct device *dev)
{
	memset(table, 0, sizeof(*table));
	table->dev = get_device(dev);
	spin_lock_init(&table->lock);
	table->l2 = kcalloc(T736_IOMMU_USED_PD_ENTRIES, sizeof(*table->l2), GFP_KERNEL);
	if (!table->l2)
		goto fail;
	table->pd_cpu = dma_alloc_coherent(dev, T736_IOMMU_PD_SIZE,
					 &table->pd_dma, GFP_KERNEL);
	if (!table->pd_cpu)
		goto fail;
	if (!IS_ALIGNED(table->pd_dma, T736_IOMMU_PD_SIZE) ||
	    table->pd_dma > T736_PHYS_MAX - T736_IOMMU_PD_SIZE + 1)
		goto fail;
	memset(table->pd_cpu, 0, T736_IOMMU_PD_SIZE);
	dma_wmb();
	return 0;
fail:
	if (table->pd_cpu)
		dma_free_coherent(dev, T736_IOMMU_PD_SIZE, table->pd_cpu, table->pd_dma);
	kfree(table->l2);
	put_device(table->dev);
	memset(table, 0, sizeof(*table));
	return -ENOMEM;
}

int t736_iommu_pgtable_destroy(struct t736_iommu_pgtable *table)
{
	unsigned int i;

	/* No caller is allowed to turn an attached table into freed DMA memory. */
	if (WARN_ON_ONCE(table->hardware_attached))
		return -EBUSY;
	for (i = 0; i < T736_IOMMU_USED_PD_ENTRIES; i++)
		if (table->l2[i].cpu)
			dma_free_coherent(table->dev, T736_IOMMU_PAGE_SIZE,
					  table->l2[i].cpu, table->l2[i].dma);
	dma_free_coherent(table->dev, T736_IOMMU_PD_SIZE,
			  table->pd_cpu, table->pd_dma);
	kfree(table->l2);
	put_device(table->dev);
	memset(table, 0, sizeof(*table));
	return 0;
}

static int t736_alloc_l2(struct t736_iommu_pgtable *table, unsigned int index)
{
	struct t736_iommu_l2 *l2 = &table->l2[index];

	if (l2->cpu)
		return l2->published ? 0 : -ERANGE;
	/* map_pages can be atomic. Allocation failure leaves only empty tables. */
	l2->cpu = dma_alloc_coherent(table->dev, T736_IOMMU_PAGE_SIZE,
				    &l2->dma, GFP_ATOMIC);
	if (!l2->cpu)
		return -ENOMEM;
	if (!IS_ALIGNED(l2->dma, T736_IOMMU_PAGE_SIZE) ||
	    l2->dma > T736_PHYS_MAX - T736_IOMMU_PAGE_SIZE + 1) {
		/* Keep ownership until process-context destruction. Coherent frees
		 * must not run under this IRQ-disabling page-table lock. */
		return -ERANGE;
	}
	memset(l2->cpu, 0, T736_IOMMU_PAGE_SIZE);
	/* Publish the entire zeroed L2 before making its DMA address visible. */
	dma_wmb();
	WRITE_ONCE(table->pd_cpu[index], t736_encode_address(l2->dma) | T736_PDE_VALID);
	dma_wmb();
	l2->published = true;
	return 0;
}

int t736_iommu_pgtable_map(struct t736_iommu_pgtable *table, unsigned long iova,
			 phys_addr_t physical, size_t page_count, int prot,
			 size_t *mapped)
{
	size_t bytes, physical_bytes, page;
	unsigned long flags;
	unsigned int first, last, index;
	u32 access = 0;
	int ret = 0;

	if (!t736_valid_range(iova, page_count, T736_IOVA_MAX, &bytes) ||
	    !t736_valid_range(physical, page_count, T736_PHYS_MAX, &physical_bytes))
		return -ERANGE;
	if (!(prot & (IOMMU_READ | IOMMU_WRITE)))
		return -EINVAL;
	if (prot & IOMMU_READ)
		access |= T736_PTE_READ;
	if (prot & IOMMU_WRITE)
		access |= T736_PTE_WRITE;
	first = iova >> 22;
	last = (iova + bytes - 1) >> 22;
	spin_lock_irqsave(&table->lock, flags);
	for (index = first; index <= last; index++) {
		ret = t736_alloc_l2(table, index);
		if (ret)
			goto out;
	}
	/* Validate the full interval before publishing any new mapping. */
	for (page = 0; page < page_count; page++) {
		u64 address = iova + page * T736_IOMMU_PAGE_SIZE;
		u32 *entry = &table->l2[address >> 22].cpu[(address >> 12) & 1023];
		if (READ_ONCE(*entry) & T736_PTE_VALID) {
			ret = -EEXIST;
			goto out;
		}
	}
	for (page = 0; page < page_count; page++) {
		u64 address = iova + page * T736_IOMMU_PAGE_SIZE;
		u32 *entry = &table->l2[address >> 22].cpu[(address >> 12) & 1023];
		WRITE_ONCE(*entry, t736_encode_address(physical + page * T736_IOMMU_PAGE_SIZE) |
			   access | T736_PTE_VALID);
	}
	dma_wmb();
	*mapped += bytes;
out:
	spin_unlock_irqrestore(&table->lock, flags);
	return ret;
}

size_t t736_iommu_pgtable_unmap(struct t736_iommu_pgtable *table,
			      unsigned long iova, size_t page_count)
{
	size_t bytes, page;
	unsigned long flags;

	if (!t736_valid_range(iova, page_count, T736_IOVA_MAX, &bytes))
		return 0;
	spin_lock_irqsave(&table->lock, flags);
	for (page = 0; page < page_count; page++) {
		u64 address = iova + page * T736_IOMMU_PAGE_SIZE;
		struct t736_iommu_l2 *l2 = &table->l2[address >> 22];
		u32 *entry;
		if (!l2->cpu)
			break;
		entry = &l2->cpu[(address >> 12) & 1023];
		if (!(READ_ONCE(*entry) & T736_PTE_VALID))
			break;
		WRITE_ONCE(*entry, 0);
	}
	/* The caller performs invalidation after this publication barrier.
	 * Keep L1 and L2 allocated; no table can be freed before sync completes. */
	dma_wmb();
	spin_unlock_irqrestore(&table->lock, flags);
	return page * T736_IOMMU_PAGE_SIZE;
}

phys_addr_t t736_iommu_pgtable_translate(struct t736_iommu_pgtable *table,
				       dma_addr_t iova)
{
	unsigned long flags;
	phys_addr_t address = 0;
	u32 entry;

	if (iova > T736_IOVA_MAX)
		return 0;
	spin_lock_irqsave(&table->lock, flags);
	if (table->l2[iova >> 22].cpu) {
		entry = READ_ONCE(table->l2[iova >> 22].cpu[(iova >> 12) & 1023]);
		if (entry & T736_PTE_VALID)
			address = t736_decode_address(entry) | (iova & 4095);
	}
	spin_unlock_irqrestore(&table->lock, flags);
	return address;
}

void t736_iommu_pgtable_clear(struct t736_iommu_pgtable *table)
{
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&table->lock, flags);
	for (i = 0; i < T736_IOMMU_USED_PD_ENTRIES; i++)
		if (table->l2[i].cpu)
			memset(table->l2[i].cpu, 0, T736_IOMMU_PAGE_SIZE);
	dma_wmb();
	spin_unlock_irqrestore(&table->lock, flags);
}
