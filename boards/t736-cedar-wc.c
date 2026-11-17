// SPDX-License-Identifier: GPL-2.0-only
/*
 * T736 decoder bring-up: Cedar 2.0 ABI and a device-private WC DMA heap.
 * Register/ABI definitions are from Allwinner's GPL sun60iw2 Cedar driver.
 * This is deliberately a built-in, root-only, single-process decoder service.
 * No IOMMU or foreign dma-buf is accepted. Userspace still owns codec commands.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/log2.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/overflow.h>
#include <linux/pid.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/uaccess.h>

#define VE_MAX_BUFFER SZ_64M
#define VE_MAX_HEAP (128UL * SZ_1M)
#define VE_REG_USER_SIZE SZ_4K
#define VE_ALLOC_ATTRS DMA_ATTR_WRITE_COMBINE

struct t736_ve;
struct ve_buffer {
	struct t736_ve *ve;
	void *cpu;
	dma_addr_t dma;
	size_t size;
};

struct ve_mapping {
	struct list_head node;
	struct dma_buf *buffer;
	struct dma_buf_attachment *attachment;
	struct sg_table *table;
	u32 address;
	unsigned int refs;
};

struct ve_file {
	struct t736_ve *ve;
	struct list_head mappings;
	unsigned int engine_refs;
};

struct t736_ve {
	struct device *dev;
	void __iomem *regs;
	resource_size_t reg_start;
	struct clk *clk;
	struct reset_control *reset;
	struct miscdevice misc;
	struct dma_heap *heap;
	struct mutex lock;
	wait_queue_head_t wait;
	struct pid *owner;
	struct ve_file *locks[5];
	struct pid *lock_tasks[5];
	struct list_head retired;
	unsigned int files, engine_refs;
	atomic_t pending, vcu_pending;
	atomic_long_t allocated;
	bool powered, poisoned, irq_enabled;
	int mode;
	int irq;
};

struct ve_dma_arg { s32 fd; u32 address; };
struct ve_cache_arg { u64 start, end; };
struct ve_env_arg { u32 start; s32 size; u64 address; u32 from_kernel, padding; };

static const struct dma_buf_ops ve_buffer_ops;

static void ve_poison(struct t736_ve *ve)
{
	WRITE_ONCE(ve->poisoned, true);
	wake_up_all(&ve->wait);
}

/* Only the allocating device can consume this already mapped DMA allocation.
 * No dma_map_sg() is used: it could create a second mapping or a bounce buffer.
 */
static int ve_buffer_attach(struct dma_buf *db, struct dma_buf_attachment *a)
{
	struct ve_buffer *b = db->priv;

	return a->dev == b->ve->dev ? 0 : -EXDEV;
}

static struct sg_table *ve_buffer_map(struct dma_buf_attachment *a,
				    enum dma_data_direction direction)
{
	struct ve_buffer *b = a->dmabuf->priv;
	struct sg_table *sgt;
	struct scatterlist *sg;
	size_t offset = 0;
	unsigned int i;
	int ret;

	if (direction == DMA_NONE || a->dev != b->ve->dev)
		return ERR_PTR(-EINVAL);
	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);
	ret = dma_get_sgtable_attrs(a->dev, sgt, b->cpu, b->dma, b->size,
				  VE_ALLOC_ATTRS);
	if (ret)
		goto fail;
	for_each_sg(sgt->sgl, sg, sgt->orig_nents, i) {
		/* This board's private heap promises an identity, contiguous path.
		 * Check all pages, not merely the first scatterlist entry.
		 */
		if (!sg->length || sg->length > b->size - offset ||
		    sg_phys(sg) != b->dma + offset) {
			ret = -ERANGE;
			goto free_sg;
		}
		sg_dma_address(sg) = b->dma + offset;
		sg_dma_len(sg) = sg->length;
		offset += sg->length;
	}
	if (offset != b->size) {
		ret = -ERANGE;
		goto free_sg;
	}
	sgt->nents = sgt->orig_nents;
	dma_wmb();
	return sgt;
free_sg:
	sg_free_table(sgt);
fail:
	kfree(sgt);
	return ERR_PTR(ret);
}

static void ve_buffer_unmap(struct dma_buf_attachment *a, struct sg_table *sgt,
			    enum dma_data_direction direction)
{
	/* The original dma_alloc_wc handle remains valid until buffer release. */
	sg_free_table(sgt);
	kfree(sgt);
}

static int ve_buffer_begin_cpu(struct dma_buf *db, enum dma_data_direction dir)
{
	/* WC has no dirty CPU cachelines; the caller must first wait for VE. */
	mb();
	return 0;
}

static int ve_buffer_end_cpu(struct dma_buf *db, enum dma_data_direction dir)
{
	/* Order WC payload writes before subsequent userspace MMIO submission. */
	mb();
	return 0;
}

static int ve_buffer_mmap(struct dma_buf *db, struct vm_area_struct *vma)
{
	struct ve_buffer *b = db->priv;
	u64 offset = (u64)vma->vm_pgoff << PAGE_SHIFT;
	size_t length = vma->vm_end - vma->vm_start;

	if (!(vma->vm_flags & VM_SHARED) || (vma->vm_flags & VM_EXEC) ||
	    offset > b->size || length > b->size - offset)
		return -EINVAL;
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
	return dma_mmap_wc(b->ve->dev, vma, b->cpu, b->dma, b->size);
}

static void ve_buffer_release(struct dma_buf *db)
{
	struct ve_buffer *b = db->priv;

	dma_free_wc(b->ve->dev, b->size, b->cpu, b->dma);
	atomic_long_sub(b->size, &b->ve->allocated);
	kfree(b);
}

static const struct dma_buf_ops ve_buffer_ops = {
	.attach = ve_buffer_attach,
	.map_dma_buf = ve_buffer_map,
	.unmap_dma_buf = ve_buffer_unmap,
	.begin_cpu_access = ve_buffer_begin_cpu,
	.end_cpu_access = ve_buffer_end_cpu,
	.mmap = ve_buffer_mmap,
	.release = ve_buffer_release,
};

static struct dma_buf *ve_heap_allocate(struct dma_heap *heap, unsigned long len,
				       u32 fd_flags, u64 heap_flags)
{
	struct t736_ve *ve = dma_heap_get_drvdata(heap);
	DEFINE_DMA_BUF_EXPORT_INFO(info);
	struct ve_buffer *b;
	struct dma_buf *db;
	int err = -ENOMEM;

	if (!len || len > VE_MAX_BUFFER || !IS_ALIGNED(len, PAGE_SIZE) || heap_flags)
		return ERR_PTR(-EINVAL);
	if (READ_ONCE(ve->poisoned))
		return ERR_PTR(-EIO);
	if (atomic_long_add_return(len, &ve->allocated) > VE_MAX_HEAP) {
		atomic_long_sub(len, &ve->allocated);
		return ERR_PTR(-ENOSPC);
	}
	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		goto subtract;
	b->ve = ve;
	b->size = len;
	b->cpu = dma_alloc_wc(ve->dev, len, &b->dma, GFP_KERNEL);
	if (!b->cpu)
		goto free_buffer;
	if (b->dma > U32_MAX || len - 1 > U32_MAX - b->dma) {
		err = -ERANGE;
		goto free_dma;
	}
	memset(b->cpu, 0, len);
	dma_wmb();
	info.exp_name = "t736-ve-wc";
	info.ops = &ve_buffer_ops;
	info.size = len;
	info.flags = fd_flags;
	info.priv = b;
	db = dma_buf_export(&info);
	if (!IS_ERR(db))
		return db;
	err = PTR_ERR(db);
free_dma:
	dma_free_wc(ve->dev, len, b->cpu, b->dma);
free_buffer:
	kfree(b);
subtract:
	atomic_long_sub(len, &ve->allocated);
	return ERR_PTR(err);
}

static const struct dma_heap_ops ve_heap_ops = { .allocate = ve_heap_allocate };

/* The ioctl's "IOMMU" name is historical. Return the actual DMA handle, never
 * manufacture a physical address for a system-heap or foreign buffer.
 */
static long ve_map_ioctl(struct ve_file *f, unsigned long arg, bool release)
{
	struct ve_dma_arg data;
	struct dma_buf *db;
	struct ve_mapping *m;
	struct ve_buffer *b;
	long ret = 0;
	bool fresh = false;

	if (copy_from_user(&data, (void __user *)arg, sizeof(data)))
		return -EFAULT;
	db = dma_buf_get(data.fd);
	if (IS_ERR(db))
		return PTR_ERR(db);
	if (db->ops != &ve_buffer_ops ||
	    ((struct ve_buffer *)db->priv)->ve != f->ve) {
		ret = -EXDEV;
		goto put;
	}
	list_for_each_entry(m, &f->mappings, node) {
		if (m->buffer == db)
			goto found;
	}
	if (release) {
		ret = -ENOENT;
		goto put;
	}
	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m) {
		ret = -ENOMEM;
		goto put;
	}
	m->attachment = dma_buf_attach(db, f->ve->dev);
	if (IS_ERR(m->attachment)) {
		ret = PTR_ERR(m->attachment);
		goto free;
	}
	m->table = dma_buf_map_attachment_unlocked(m->attachment, DMA_BIDIRECTIONAL);
	if (IS_ERR(m->table)) {
		ret = PTR_ERR(m->table);
		dma_buf_detach(db, m->attachment);
		goto free;
	}
	b = db->priv;
	m->buffer = db;
	m->address = b->dma;
	get_dma_buf(db);
	list_add_tail(&m->node, &f->mappings);
	fresh = true;
found:
	if (release) {
		if (--m->refs == 0)
			list_move_tail(&m->node, &f->ve->retired);
		goto put;
	}
	data.address = m->address;
	if (copy_to_user((void __user *)arg, &data, sizeof(data))) {
		/* Even a partial userspace copy may have disclosed the DMA address.
		 * Keep a new mapping pinned until reboot, never recycle it.
		 */
		if (fresh)
			list_move_tail(&m->node, &f->ve->retired);
		ret = -EFAULT;
		goto put;
	}
	m->refs++;
	goto put;
free:
	kfree(m);
put:
	dma_buf_put(db);
	return ret;
}

/* No arbitrary VA cache maintenance: prove that the entire range belongs to a
 * WC dma-buf VMA imported by this Cedar file, then issue ordering only.
 */
static long ve_cache_ioctl(struct ve_file *f, unsigned long arg)
{
	struct ve_cache_arg range;
	struct vm_area_struct *vma;
	struct ve_mapping *m;
	struct file *mapped_file = NULL;
	u64 last = 0, offset;
	long ret = -EINVAL;

	if (copy_from_user(&range, (void __user *)arg, sizeof(range)))
		return -EFAULT;
	if (!current->mm || range.end <= range.start || range.end > ULONG_MAX)
		return -EINVAL;
	mmap_read_lock(current->mm);
	vma = find_vma(current->mm, range.start);
	if (vma && range.start >= vma->vm_start && range.end <= vma->vm_end &&
	    vma->vm_file) {
		offset = (u64)vma->vm_pgoff << PAGE_SHIFT;
		if (!check_add_overflow(offset, range.end - vma->vm_start, &last))
			mapped_file = get_file(vma->vm_file);
	}
	mmap_read_unlock(current->mm);
	if (!mapped_file)
		return -EINVAL;
	/* We never dereference this VA. Pin the file, release mmap_lock before
	 * taking ve->lock, then validate against the still-pinned DMA allocation.
	 */
	mutex_lock(&f->ve->lock);
	if (f->ve->poisoned) {
		ret = -EIO;
		goto out;
	}
	list_for_each_entry(m, &f->mappings, node) {
		if (mapped_file != m->buffer->file)
			continue;
		if (last > m->buffer->size)
			break;
		mb();
		ret = 0;
		break;
	}
out:
	mutex_unlock(&f->ve->lock);
	fput(mapped_file);
	return ret;
}

static irqreturn_t ve_irq(int irq, void *data)
{
	struct t736_ve *ve = data;
	u32 selector, control, status, mask, co, so;

	if (!READ_ONCE(ve->powered))
		return IRQ_NONE;
	if (READ_ONCE(ve->mode) == 4) {
		if (!(readl(ve->regs + 0x05c) & BIT(2)) ||
		    !(readl(ve->regs + 0x628) & BIT(0)))
			return IRQ_NONE;
		writel(BIT(0), ve->regs + 0x628);
		readl(ve->regs + 0x628);
		atomic_set(&ve->vcu_pending, 1);
		wake_up_all(&ve->wait);
		return IRQ_HANDLED;
	}
	selector = readl(ve->regs) & 0xf;
	switch (selector) {
	case 0: co = 0x114; so = 0x11c; mask = 0x7c; break;
	case 1: co = 0x220; so = 0x228; mask = 0xf; break;
	case 2: co = 0x324; so = 0x32c; mask = 0xf; break;
	case 3: co = 0x414; so = 0x41c; mask = 0xf; break;
	case 4: co = 0x530; so = 0x538; mask = 0xf; break;
	case 6: co = 0xf04; so = 0xf08; mask = 1; break;
	default: return IRQ_NONE;
	}
	control = readl(ve->regs + co);
	status = readl(ve->regs + so);
	if (!(control & mask) || !(status & 0xf))
		return IRQ_NONE;
	/* Userspace reads and clears codec status; only mask interrupt enables. */
	writel(control & ~mask, ve->regs + co);
	readl(ve->regs + co);
	atomic_set(&ve->pending, 1);
	wake_up_all(&ve->wait);
	return IRQ_HANDLED;
}

/* CCU reset status proves only assertion, not AXI drain. All imports therefore
 * remain pinned until reboot, including normal completion and final close.
 * Failed reset additionally prevents every subsequent decoder session.
 */
static int ve_assert_reset(struct t736_ve *ve)
{
	int ret = reset_control_assert(ve->reset);

	if (!ret) {
		udelay(10);
		ret = reset_control_status(ve->reset);
		ret = ret == 1 ? 0 : (ret < 0 ? ret : -EIO);
	}
	if (ret) {
		ve_poison(ve);
		dev_err(ve->dev, "decoder reset fence failed: %d; DMA allocations quarantined\n", ret);
	}
	return ret;
}

static int ve_reset(struct t736_ve *ve)
{
	u32 value;
	int ret;

	if (ve->irq_enabled) {
		disable_irq(ve->irq);
		ve->irq_enabled = false;
	}
	ret = ve_assert_reset(ve);

	if (ret)
		return ret;
	ret = reset_control_deassert(ve->reset);
	if (ret) {
		ve_poison(ve);
		return ret;
	}
	/* sun60iw2 vendor reset sequence, DEC-side VCU register mode pulse. */
	value = readl(ve->regs + 0x804);
	writel(value | BIT(0) | BIT(4), ve->regs + 0x804);
	readl(ve->regs + 0x804);
	writel(value & ~(BIT(0) | BIT(4)), ve->regs + 0x804);
	readl(ve->regs + 0x804);
	atomic_set(&ve->pending, 0);
	atomic_set(&ve->vcu_pending, 0);
	enable_irq(ve->irq);
	ve->irq_enabled = true;
	return 0;
}

static int ve_open(struct inode *inode, struct file *file)
{
	struct t736_ve *ve = container_of(file->private_data, struct t736_ve, misc);
	struct ve_file *f;
	int ret = 0;

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	f->ve = ve;
	INIT_LIST_HEAD(&f->mappings);
	mutex_lock(&ve->lock);
	if (ve->poisoned) {
		ret = -EIO;
		goto out;
	}
	if (ve->owner && ve->owner != task_tgid(current)) {
		ret = -EBUSY;
		goto out;
	}
	if (!ve->files) {
		ret = pm_runtime_resume_and_get(ve->dev);
		if (ret < 0)
			goto out;
		ret = clk_prepare_enable(ve->clk);
		if (ret)
			goto put_pm;
		ret = reset_control_deassert(ve->reset);
		if (ret)
			goto disable_clock;
		ve->mode = 2;
		atomic_set(&ve->pending, 0);
		atomic_set(&ve->vcu_pending, 0);
		WRITE_ONCE(ve->powered, true);
		enable_irq(ve->irq);
		ve->irq_enabled = true;
		ve->owner = get_pid(task_tgid(current));
	}
	ve->files++;
	file->private_data = f;
	nonseekable_open(inode, file);
	mutex_unlock(&ve->lock);
	return 0;
disable_clock:
	clk_disable_unprepare(ve->clk);
put_pm:
	pm_runtime_put_sync(ve->dev);
out:
	mutex_unlock(&ve->lock);
	kfree(f);
	return ret;
}

static int ve_release(struct inode *inode, struct file *file)
{
	struct ve_file *f = file->private_data;
	struct t736_ve *ve = f->ve;
	unsigned int i;
	int ret;

	mutex_lock(&ve->lock);
	for (i = 0; i < ARRAY_SIZE(ve->locks); i++)
		if (ve->locks[i] == f) {
			put_pid(ve->lock_tasks[i]);
			ve->lock_tasks[i] = NULL;
			WRITE_ONCE(ve->locks[i], NULL);
		}
	ve->engine_refs -= f->engine_refs;
	list_splice_tail_init(&f->mappings, &ve->retired);
	if (--ve->files == 0) {
		/* No live Cedar register VMAs remain: their vm_file pins this file. */
		if (ve->irq_enabled) {
			disable_irq(ve->irq);
			ve->irq_enabled = false;
		}
		ret = ve_assert_reset(ve);
		WRITE_ONCE(ve->powered, false);
		/* No hardware DMA-drain completion is documented. Never recycle a
		 * disclosed/imported buffer, even when the reset bit reads asserted.
		 * The 128 MiB heap budget bounds this explicit bring-up quarantine.
		 */
		if (!ret && !ve->poisoned) {
			clk_disable_unprepare(ve->clk);
			pm_runtime_put_sync(ve->dev);
		}
		/* On failed reset keep power and mappings, and disallow reuse. */
		put_pid(ve->owner);
		ve->owner = NULL;
	}
	mutex_unlock(&ve->lock);
	wake_up_all(&ve->wait);
	kfree(f);
	return 0;
}

static long ve_lock_ioctl(struct ve_file *f, unsigned long arg, bool release)
{
	struct t736_ve *ve = f->ve;
	unsigned int index;
	int ret;

	if (!is_power_of_2(arg) || arg > BIT(4))
		return -EINVAL;
	index = __ffs(arg);
	for (;;) {
		mutex_lock(&ve->lock);
		if (ve->poisoned) {
			ret = -EIO;
		} else if (release) {
			ret = ve->locks[index] == f &&
			      ve->lock_tasks[index] == task_pid(current) ? 0 : -EPERM;
			if (!ret) {
				put_pid(ve->lock_tasks[index]);
				ve->lock_tasks[index] = NULL;
				WRITE_ONCE(ve->locks[index], NULL);
			}
		} else if (ve->locks[index] == f &&
			   ve->lock_tasks[index] == task_pid(current)) {
			ret = -EDEADLK;
		} else if (!ve->locks[index]) {
			ve->lock_tasks[index] = get_pid(task_pid(current));
			WRITE_ONCE(ve->locks[index], f);
			ret = 0;
		} else {
			ret = -EAGAIN;
		}
		mutex_unlock(&ve->lock);
		if (ret != -EAGAIN) {
			if (release)
				wake_up_all(&ve->wait);
			return ret;
		}
		ret = wait_event_interruptible(ve->wait,
			!READ_ONCE(ve->locks[index]) || READ_ONCE(ve->poisoned));
		if (ret)
			return ret;
	}
}

static long ve_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct ve_file *f = file->private_data;
	struct t736_ve *ve = f->ve;
	struct ve_env_arg env = { 0 };
	atomic_t *pending;
	long ret = 0;

	if (READ_ONCE(ve->owner) != task_tgid(current))
		return -EPERM;
	if (READ_ONCE(ve->poisoned))
		return -EIO;
	if (cmd == 0x310 || cmd == 0x311)
		return ve_lock_ioctl(f, arg, cmd == 0x311);
	/* Snapshot VMA ownership before taking the device lock. */
	if (cmd == 0x506 || cmd == 0x917)
		return ve_cache_ioctl(f, arg);
	if (cmd == 0x102 || cmd == 0x805) {
		if (arg > 10)
			return -EINVAL;
		pending = cmd == 0x102 ? &ve->pending : &ve->vcu_pending;
		ret = wait_event_interruptible_timeout(ve->wait,
			atomic_read(pending) || READ_ONCE(ve->poisoned), arg * HZ);
		if (ret < 0)
			return ret;
		if (READ_ONCE(ve->poisoned))
			return -EIO;
		if (atomic_xchg(pending, 0)) {
			dma_rmb();
			return 1;
		}
		if (!ret && arg) {
			mutex_lock(&ve->lock);
			ve_poison(ve);
			mutex_unlock(&ve->lock);
			dev_err(ve->dev, "decoder timeout: imports pinned, new sessions disabled until reboot\n");
		}
		return 0;
	}
	mutex_lock(&ve->lock);
	if (ve->poisoned) {
		mutex_unlock(&ve->lock);
		return -EIO;
	}
	switch (cmd) {
	case 0x101:
		ret = copy_to_user((void __user *)arg, &env, sizeof(env)) ? -EFAULT : 0;
		break;
	case 0x104:
		ret = ve_reset(ve);
		break;
	case 0x107:
		/* The first port deliberately uses the provider's fixed safe rate. */
		ret = clk_get_rate(ve->clk);
		break;
	case 0x206:
		f->engine_refs++;
		ve->engine_refs++;
		break;
	case 0x207:
		if (!f->engine_refs)
			ret = -EINVAL;
		else {
			f->engine_refs--;
			ve->engine_refs--;
		}
		break;
	case 0x501:
		ret = ve->engine_refs;
		break;
	case 0x502: case 0x504:
		ret = ve_map_ioctl(f, arg, false);
		break;
	case 0x503: case 0x505:
		ret = ve_map_ioctl(f, arg, true);
		break;
	case 0x710:
		ret = clk_get_rate(ve->clk) / 1000000;
		break;
	case 0x800:
		if (arg != 2 && arg != 4)
			ret = -EOPNOTSUPP;
		else
			WRITE_ONCE(ve->mode, arg);
		break;
	case 0x804: /* Decoder register base is at offset zero. */
	case 0x105: case 0x106: case 0x209: case 0x600:
	case 0x507: case 0x508: case 0x509: case 0x610: case 0x611:
	case 0x711: /* Fixed frequency; no debug payloads are consumed. */
		break;
	default:
		dev_warn_ratelimited(ve->dev, "unsupported decoder ioctl %#x\n", cmd);
		ret = -ENOTTY;
	}
	mutex_unlock(&ve->lock);
	return ret;
}

static int ve_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct ve_file *f = file->private_data;
	size_t size = vma->vm_end - vma->vm_start;

	if (READ_ONCE(f->ve->poisoned))
		return -EIO;
	if (READ_ONCE(f->ve->owner) != task_tgid(current) ||
	    !(vma->vm_flags & VM_SHARED) || (vma->vm_flags & VM_EXEC) ||
	    vma->vm_pgoff || size > VE_REG_USER_SIZE)
		return -EINVAL;
	vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP | VM_DONTCOPY);
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	return remap_pfn_range(vma, vma->vm_start, f->ve->reg_start >> PAGE_SHIFT,
			       size, vma->vm_page_prot);
}

static const struct file_operations ve_fops = {
	.owner = THIS_MODULE,
	.open = ve_open,
	.release = ve_release,
	.unlocked_ioctl = ve_ioctl,
	.mmap = ve_mmap,
};

static int ve_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dma_heap_export_info heap_info = { 0 };
	struct t736_ve *ve;
	struct resource *res;
	int ret;

	if (device_iommu_mapped(dev))
		return dev_err_probe(dev, -EINVAL, "WC heap requires the dedicated bypass DMA path\n");
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	ve = devm_kzalloc(dev, sizeof(*ve), GFP_KERNEL);
	if (!ve)
		return -ENOMEM;
	ve->dev = dev;
	mutex_init(&ve->lock);
	init_waitqueue_head(&ve->wait);
	INIT_LIST_HEAD(&ve->retired);
	atomic_long_set(&ve->allocated, 0);
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || resource_size(res) < VE_REG_USER_SIZE || !IS_ALIGNED(res->start, PAGE_SIZE))
		return -EINVAL;
	ve->reg_start = res->start;
	ve->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(ve->regs))
		return PTR_ERR(ve->regs);
	ve->clk = devm_clk_get(dev, "ve");
	if (IS_ERR(ve->clk))
		return dev_err_probe(dev, PTR_ERR(ve->clk), "VE clock\n");
	ve->reset = devm_reset_control_get_exclusive(dev, "ve");
	if (IS_ERR(ve->reset))
		return dev_err_probe(dev, PTR_ERR(ve->reset), "VE reset\n");
	ve->irq = platform_get_irq(pdev, 0);
	if (ve->irq < 0)
		return ve->irq;
	ret = devm_request_irq(dev, ve->irq, ve_irq, IRQF_NO_AUTOEN, "t736-cedar-wc", ve);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, ve);
	pm_runtime_set_suspended(dev);
	pm_runtime_enable(dev);
	ve->misc.minor = MISC_DYNAMIC_MINOR;
	ve->misc.name = "cedar_dev";
	ve->misc.fops = &ve_fops;
	ve->misc.parent = dev;
	ve->misc.mode = 0600;
	ret = misc_register(&ve->misc);
	if (ret)
		goto disable_pm;
	heap_info.name = "t736-ve";
	heap_info.ops = &ve_heap_ops;
	heap_info.priv = ve;
	ve->heap = dma_heap_add(&heap_info);
	if (IS_ERR(ve->heap)) {
		ret = PTR_ERR(ve->heap);
		misc_deregister(&ve->misc);
		goto disable_pm;
	}
	dev_info(dev, "Cedar decoder + private 32-bit WC heap; root-only, fixed-frequency bring-up\n");
	return 0;
disable_pm:
	pm_runtime_disable(dev);
	return ret;
}

static const struct of_device_id ve_of_match[] = {
	{ .compatible = "allwinner,t736-cedar-wc" }, { }
};

/* dma_heap has no removal API. This driver is bool/built-in and static-DT only. */
static struct platform_driver ve_driver = {
	.probe = ve_probe,
	.driver = {
		.name = "t736-cedar-wc",
		.of_match_table = ve_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(ve_driver);
MODULE_DESCRIPTION("T736 Cedar decoder with private write-combined DMA heap");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("DMA_BUF");
