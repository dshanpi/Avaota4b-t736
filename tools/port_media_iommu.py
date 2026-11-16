#!/usr/bin/env python3
"""Apply the first compile-only Linux 6.18 IOMMU-v20 API port.

Does not provide a deployable IOMMU: domain switching, distributed power, page
table DMA ownership, TLB completion and probe unwind still require review.
"""
import difflib
from pathlib import Path

ROOT = Path('/home/ubuntu/t736-mainline')
PORT = ROOT / 'sources/work/linux-media-v17/drivers/iommu/t736'


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError('Unexpected donor text: ' + old[:100])
    return text.replace(old, new, 1)


def main():
    p = PORT / 'sunxi-iommu-v2.c'
    old = p.read_text()
    text = old
    text = replace_once(text, '#include <linux/pm_domain.h>',
                        '#include <linux/pm_domain.h>\n#include <linux/overflow.h>')
    text = replace_once(text,
        'static int sunxi_iommu_map(struct iommu_domain *domain, unsigned long iova,\n'
        '\t\t\t   phys_addr_t paddr, size_t size, int prot, gfp_t gfp)',
        'static int sunxi_iommu_map_pages(struct iommu_domain *domain, unsigned long iova,\n'
        '\t\t\t   phys_addr_t paddr, size_t pgsize, size_t pgcount,\n'
        '\t\t\t   int prot, gfp_t gfp, size_t *mapped)')
    start, end = text.index('static int sunxi_iommu_map_pages('), text.index('static size_t sunxi_iommu_unmap(')
    fn = text[start:end]
    fn = replace_once(fn, '\tint ret;\n', '\tint ret;\n\tsize_t size;\n')
    fn = replace_once(fn, '\tsunxi_domain = container_of',
        '\tif (pgsize != SZ_4K || !pgcount ||\n'
        '\t    check_mul_overflow(pgsize, pgcount, &size) ||\n'
        '\t    !IS_ALIGNED(iova, SZ_4K) || !IS_ALIGNED(paddr, SZ_4K))\n'
        '\t\treturn -EINVAL;\n'
        '\tif (iova > U32_MAX || size - 1 > U32_MAX - iova ||\n'
        '\t    paddr > DMA_BIT_MASK(34) || size - 1 > DMA_BIT_MASK(34) - paddr)\n'
        '\t\treturn -ERANGE;\n\n'
        '\tsunxi_domain = container_of')
    fn = replace_once(fn, '\treturn 0;\n}', '\t*mapped += size;\n\treturn 0;\n}')
    text = text[:start] + fn + text[end:]
    text = replace_once(text,
        'static size_t sunxi_iommu_unmap(struct iommu_domain *domain, unsigned long iova,\n'
        '\t\t\t\tsize_t size, struct iommu_iotlb_gather *gather)',
        'static size_t sunxi_iommu_unmap_pages(struct iommu_domain *domain, unsigned long iova,\n'
        '\t\t\t\tsize_t pgsize, size_t pgcount,\n'
        '\t\t\t\tstruct iommu_iotlb_gather *gather)')
    start, end = text.index('static size_t sunxi_iommu_unmap_pages('), text.index('void sunxi_iommu_iotlb_sync_map(')
    fn = text[start:end]
    fn = replace_once(fn, '\tu32 iova_tail_size;', '\tint iova_tail_size;\n\tsize_t size;')
    fn = replace_once(fn, '\tsunxi_domain = container_of',
        '\tif (pgsize != SZ_4K || !pgcount ||\n'
        '\t    check_mul_overflow(pgsize, pgcount, &size) ||\n'
        '\t    !IS_ALIGNED(iova, SZ_4K) || iova > U32_MAX ||\n'
        '\t    size - 1 > U32_MAX - iova)\n\t\treturn 0;\n\n'
        '\tsunxi_domain = container_of')
    fn = replace_once(fn, '\t\t\t sunxi_domain->pgtable, iova_start, iova_end);',
                        '\t\t\t sunxi_domain->pgtable, iova_start, iova_end);\n'
                        '\t\tif (iova_tail_size <= 0)\n\t\t\tbreak;')
    fn = replace_once(fn, '\treturn size;', '\treturn iova_start - iova;')
    text = text[:start] + fn + text[end:]
    start, end = text.index('void sunxi_iommu_iotlb_sync_map('), text.index('void sunxi_iommu_iotlb_sync(')
    fn = text[start:end].replace('void sunxi_iommu_iotlb_sync_map(', 'static int sunxi_iommu_iotlb_sync_map(', 1)
    fn = replace_once(fn, '\treturn;', '\treturn 0;')
    text = text[:start] + fn + text[end:]
    text = replace_once(text, 'static struct iommu_domain *sunxi_iommu_domain_alloc(unsigned type)',
                        'static struct iommu_domain *sunxi_iommu_domain_alloc_paging(struct device *dev)')
    text = replace_once(text,
        '\tif (type != IOMMU_DOMAIN_DMA && type != IOMMU_DOMAIN_UNMANAGED)\n\t\treturn NULL;\n\n'
        '\t/* we just use one domain */\n\tif (global_iommu_domain)\n\t\treturn global_iommu_domain;',
        '\t/* The donor has one shared hardware page table. Never hand the same\n'
        '\t * allocation to multiple independent core-owned domains. */\n'
        '\tif (global_iommu_domain)\n\t\treturn ERR_PTR(-EBUSY);')
    text = replace_once(text, '\tsunxi_domain->domain.geometry.aperture_end = (1ULL << 34) - 1;',
                        '\tsunxi_domain->domain.pgsize_bitmap = SZ_4K;\n'
                        '\tsunxi_domain->domain.geometry.aperture_end = DMA_BIT_MASK(32);')
    text = replace_once(text,
        '\tfree_pages((unsigned long)sunxi_domain->sg_buffer,\n\t\t   get_order(MAX_SG_TABLE_SIZE));',
        '\tif (global_iommu_domain == domain) {\n'
        '\t\tglobal_iommu_domain = NULL;\n\t\tiommu_hw_init_flag = false;\n\t}\n'
        '\tfree_pages((unsigned long)sunxi_domain->sg_buffer,\n\t\t   get_order(MAX_SG_TABLE_SIZE));')
    text = replace_once(text,
        'static int sunxi_iommu_attach_dev(struct iommu_domain *domain,\n'
        '\t\t\t\t  struct device *dev)\n{\n\treturn 0;\n}',
        'static int sunxi_iommu_attach_dev(struct iommu_domain *domain,\n'
        '\t\t\t\t  struct device *dev, struct iommu_domain *old)\n{\n'
        '\t/* No domain-switch implementation is claimed by this first port. */\n'
        '\tif (domain != global_iommu_domain || (old && old != domain))\n'
        '\t\treturn -EBUSY;\n\treturn 0;\n}')
    text = replace_once(text, '\tdev->iommu_group = NULL;\n', '')
    text = replace_once(text, '\tgroup = global_group;', '\tgroup = iommu_group_ref_get(global_group);')
    text = replace_once(text,
        'static int sunxi_iommu_of_xlate(struct device *dev,\n\t\t\t\tstruct of_phandle_args *args)',
        'static int sunxi_iommu_of_xlate(struct device *dev,\n\t\t\t\tconst struct of_phandle_args *args)')
    start = text.index('#ifdef SEPERATE_DOMAIN_API\nstatic const struct iommu_domain_ops')
    end = text.index('static int __init_plat_data_from_ofnode', start)
    text = text[:start] + '''static const struct iommu_domain_ops sunxi_iommu_domain_ops = {
	.attach_dev = sunxi_iommu_attach_dev,
	.map_pages = sunxi_iommu_map_pages,
	.unmap_pages = sunxi_iommu_unmap_pages,
	.iotlb_sync_map = sunxi_iommu_iotlb_sync_map,
	.iova_to_phys = sunxi_iommu_iova_to_phys,
	.iotlb_sync = sunxi_iommu_iotlb_sync,
	.free = sunxi_iommu_domain_free,
};
static const struct iommu_ops sunxi_iommu_ops = {
	.domain_alloc_paging = sunxi_iommu_domain_alloc_paging,
	.probe_device = sunxi_iommu_probe_device,
	.probe_finalize = sunxi_iommu_probe_device_finalize,
	.release_device = sunxi_iommu_release_device,
	.device_group = sunxi_iommu_device_group,
	.of_xlate = sunxi_iommu_of_xlate,
	.default_domain_ops = &sunxi_iommu_domain_ops,
	.owner = THIS_MODULE,
	.get_resv_regions = sunxi_iommu_get_resv_regions,
};

''' + text[end:]
    start, end = text.index('static int sunxi_iommu_remove('), text.index('static int sunxi_iommu_suspend(')
    fn = text[start:end].replace('static int sunxi_iommu_remove(', 'static void sunxi_iommu_remove(', 1)
    fn = replace_once(fn, '\treturn 0;', '\treturn;')
    text = text[:start] + fn + text[end:]
    p.write_text(text)
    patch = ''.join(difflib.unified_diff(old.splitlines(True), text.splitlines(True),
                    'a/drivers/iommu/t736/sunxi-iommu-v2.c', 'b/drivers/iommu/t736/sunxi-iommu-v2.c'))
    pg = PORT / 'sunxi-iommu-pgtable.c'
    pg_old = pg.read_text()
    pg_new = replace_once(pg_old, '\tfor (; iova_start <= iova_end; iova_start += SPD_SIZE) {',
        '\t/* Align to L1 coverage before stepping: a mapping may cross an L1\n'
        '\t * boundary without being aligned to that boundary itself. */\n'
        '\tiova_start = round_down(iova_start, (dma_addr_t)SPD_SIZE);\n'
        '\tfor (; iova_start < iova_end; iova_start += SPD_SIZE) {')
    pg.write_text(pg_new)
    patch += ''.join(difflib.unified_diff(pg_old.splitlines(True), pg_new.splitlines(True),
                    'a/drivers/iommu/t736/sunxi-iommu-pgtable.c', 'b/drivers/iommu/t736/sunxi-iommu-pgtable.c'))
    out = ROOT / 'patches/linux/media-v17-iommu-api.patch'
    out.write_text(patch)
    print(out)


if __name__ == '__main__':
    main()
