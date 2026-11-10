// SPDX-License-Identifier: GPL-2.0
/*
 * svmd_vram.c - device memory ("VRAM", BAR2) that pages can migrate into.
 *
 * How the pieces fit:
 *
 *   dev_pagemap (MEMORY_DEVICE_PRIVATE)
 *       Gives every VRAM page a struct page, so the core MM can track it.
 *       "Private": the CPU can't touch it. When it tries, the CPU faults,
 *       and the core MM calls back (via drm_pagemap) to migrate it home.
 *
 *   drm_pagemap  (ops below)
 *       populate_mm   migrate a range of a process into VRAM
 *       device_map    "this VRAM page is at VRAM offset X" - what goes in
 *                     our page-table entries (with the VRAM bit)
 *
 *   drm_pagemap_devmem  (one per migrated range, devmem ops below)
 *       which VRAM pages it got, and how to copy data in and out: with
 *       the copy engine (svmd_copy.c), never the main engine
 *
 * VRAM is handed out from a simple bitmap of pages.
 */

#include <linux/bitmap.h>
#include <linux/dma-fence.h>
#include <linux/memremap.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <drm/drm_pagemap.h>

#include "svmd.h"
#include "svmd_trace.h"

struct svmd_vram {
	struct dev_pagemap pagemap;
	struct drm_pagemap dpagemap;
	struct svmd_device *sdev;
	unsigned long base_pfn;		/* first struct page's pfn */
	unsigned long npages;
	spinlock_t lock;		/* the bitmap */
	unsigned long *used;
};

/* One migrated range's VRAM. */
struct svmd_devmem {
	struct drm_pagemap_devmem base;
	struct svmd_vram *vram;
	unsigned long first, npages;	/* in VRAM pages */
};

static struct svmd_vram *to_vram(struct drm_pagemap *dpagemap)
{
	return container_of(dpagemap, struct svmd_vram, dpagemap);
}

/* The VRAM a device page belongs to. */
static struct svmd_vram *page_to_vram(struct page *page)
{
	return container_of(page_pgmap(page), struct svmd_vram, pagemap);
}

/* Where a VRAM page lives, as the device sees it (offset into BAR2). */
static u64 svmd_vram_offset(struct svmd_vram *vram, struct page *page)
{
	return (u64)(page_to_pfn(page) - vram->base_pfn) << PAGE_SHIFT;
}

/*--- devmem ops: one migrated range --------------------------------------*/

static void svmd_devmem_release(struct drm_pagemap_devmem *base)
{
	struct svmd_devmem *d = container_of(base, struct svmd_devmem, base);
	unsigned long flags;

	spin_lock_irqsave(&d->vram->lock, flags);
	bitmap_clear(d->vram->used, d->first, d->npages);
	spin_unlock_irqrestore(&d->vram->lock, flags);
	kfree(d);
}

static int svmd_populate_devmem_pfn(struct drm_pagemap_devmem *base,
				    unsigned long npages, unsigned long *pfn)
{
	struct svmd_devmem *d = container_of(base, struct svmd_devmem, base);

	for (unsigned long i = 0; i < npages; i++)
		pfn[i] = d->vram->base_pfn + d->first + i;
	return 0;
}

/*
 * Copy between VRAM pages and system pages (by DMA address). Both arrays
 * may have holes (NULL page / zero address): those pages aren't moving.
 */
static int svmd_vram_copy(struct page **pages, struct drm_pagemap_addr *addr,
			  unsigned long npages, struct dma_fence *pre,
			  bool to_vram)
{
	struct svmd_vram *vram = NULL;
	u8 (*cmds)[SVMD_CMD_SIZE];
	unsigned long i, moved = 0;
	unsigned int n = 0;
	u64 t0;
	int ret;

	if (pre) {
		ret = dma_fence_wait(pre, false);
		if (ret)
			return ret;
	}
	cmds = kvcalloc(npages, SVMD_CMD_SIZE, GFP_KERNEL);
	if (!cmds)
		return -ENOMEM;

	for (i = 0; i < npages; i++) {
		unsigned long len;
		u64 off;

		if (!pages[i] || !addr[i].addr)
			continue;
		vram = page_to_vram(pages[i]);
		off = svmd_vram_offset(vram, pages[i]);
		len = PAGE_SIZE << addr[i].order;
		if (to_vram)
			svmd_encode_cmd(cmds[n++], SVMD_OP_COPY,
					SVMD_CMD_PHYS | SVMD_CMD_DST_VRAM, 0,
					off, addr[i].addr, len);
		else
			svmd_encode_cmd(cmds[n++], SVMD_OP_COPY,
					SVMD_CMD_PHYS | SVMD_CMD_SRC_VRAM, 0,
					addr[i].addr, off, len);
		moved += 1ul << addr[i].order;
		i += (1ul << addr[i].order) - 1;
	}

	t0 = ktime_get_ns();
	ret = n ? svmd_copy_run(vram->sdev, cmds, n) : 0;
	if (!ret && n)
		trace_svmd_migrate(to_vram, moved, ktime_get_ns() - t0);
	if (!ret && n) {
		if (to_vram)
			vram->sdev->pages_to_vram += moved;
		else
			vram->sdev->pages_to_ram += moved;
	}
	kvfree(cmds);
	return ret;
}

static int svmd_copy_to_devmem(struct page **pages,
			       struct drm_pagemap_addr *addr,
			       unsigned long npages, struct dma_fence *pre)
{
	return svmd_vram_copy(pages, addr, npages, pre, true);
}

static int svmd_copy_to_ram(struct page **pages, struct drm_pagemap_addr *addr,
			    unsigned long npages, struct dma_fence *pre)
{
	return svmd_vram_copy(pages, addr, npages, pre, false);
}

static const struct drm_pagemap_devmem_ops svmd_devmem_ops = {
	.devmem_release = svmd_devmem_release,
	.populate_devmem_pfn = svmd_populate_devmem_pfn,
	.copy_to_devmem = svmd_copy_to_devmem,
	.copy_to_ram = svmd_copy_to_ram,
};

/*--- drm_pagemap ops: the whole of VRAM ----------------------------------*/

static struct drm_pagemap_addr
svmd_device_map(struct drm_pagemap *dpagemap, struct device *dev,
		struct page *page, unsigned int order,
		enum dma_data_direction dir)
{
	struct svmd_vram *vram = to_vram(dpagemap);

	/* only our own device can reach our VRAM (no peer-to-peer here) */
	if (dev != &vram->sdev->pdev->dev)
		return drm_pagemap_addr_encode(DMA_MAPPING_ERROR,
					       DRM_INTERCONNECT_DRIVER, order, dir);
	return drm_pagemap_addr_encode(svmd_vram_offset(vram, page),
				       DRM_INTERCONNECT_DRIVER, order, dir);
}

static void svmd_device_unmap(struct drm_pagemap *dpagemap, struct device *dev,
			      const struct drm_pagemap_addr *addr)
{
	/* nothing was mapped: VRAM offsets need no DMA mapping */
}

/* Migrate [start, end) of `mm` into VRAM. Called with mmap_lock held. */
static int svmd_populate_mm(struct drm_pagemap *dpagemap, unsigned long start,
			    unsigned long end, struct mm_struct *mm,
			    unsigned long timeslice_ms)
{
	struct svmd_vram *vram = to_vram(dpagemap);
	struct drm_pagemap_migrate_details details = {
		.timeslice_ms = timeslice_ms,
	};
	unsigned long npages = (end - start) >> PAGE_SHIFT, first, flags;
	struct svmd_devmem *d;

	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	spin_lock_irqsave(&vram->lock, flags);
	first = bitmap_find_next_zero_area(vram->used, vram->npages, 0, npages, 0);
	if (first < vram->npages)
		bitmap_set(vram->used, first, npages);
	spin_unlock_irqrestore(&vram->lock, flags);
	if (first >= vram->npages) {
		kfree(d);
		return -ENOSPC;			/* VRAM full: stay in system memory */
	}
	d->vram = vram;
	d->first = first;
	d->npages = npages;

	drm_pagemap_devmem_init(&d->base, &vram->sdev->pdev->dev, mm,
				&svmd_devmem_ops, dpagemap, end - start, NULL);
	/* consumes `d`: on any failure, svmd_devmem_release frees it */
	return drm_pagemap_migrate_to_devmem(&d->base, mm, start, end, &details);
}

static void svmd_dpagemap_destroy(struct drm_pagemap *dpagemap,
				  bool is_atomic_or_reclaim)
{
	struct svmd_vram *vram = to_vram(dpagemap);

	kfree(vram->used);
	kfree(vram);
}

static const struct drm_pagemap_ops svmd_dpagemap_ops = {
	.device_map = svmd_device_map,
	.device_unmap = svmd_device_unmap,
	.populate_mm = svmd_populate_mm,
	.destroy = svmd_dpagemap_destroy,
};

static void svmd_vram_put(void *data)
{
	drm_pagemap_put(data);
}

/*
 * Set up VRAM at probe. Devm order matters: the put is registered BEFORE
 * devm_memremap_pages, so on unbind the pages are torn down first (that
 * waits until no process still holds a VRAM page), and only then is the
 * drm_pagemap dropped.
 */
int svmd_vram_init(struct svmd_device *sdev)
{
	struct device *dev = &sdev->pdev->dev;
	u64 size = pci_resource_len(sdev->pdev, SVMD_BAR_VRAM);
	struct svmd_vram *vram;
	struct resource *res;
	void *addr;
	int ret;

	if (!(readl(sdev->regs + SVMD_REG_CAPS) & SVMD_CAPS_VRAM) || !size)
		return 0;

	vram = kzalloc(sizeof(*vram), GFP_KERNEL);
	if (!vram)
		return -ENOMEM;
	vram->sdev = sdev;
	vram->npages = size >> PAGE_SHIFT;
	spin_lock_init(&vram->lock);
	vram->used = bitmap_zalloc(vram->npages, GFP_KERNEL);
	if (!vram->used) {
		kfree(vram);
		return -ENOMEM;
	}

	ret = drm_pagemap_init(&vram->dpagemap, &vram->pagemap, &sdev->drm,
			       &svmd_dpagemap_ops);
	if (ret) {
		bitmap_free(vram->used);
		kfree(vram);
		return ret;
	}
	ret = devm_add_action_or_reset(dev, svmd_vram_put, &vram->dpagemap);
	if (ret)
		return ret;

	/* a free physical address range to hang the struct pages on */
	res = devm_request_free_mem_region(dev, &iomem_resource, size);
	if (IS_ERR(res))
		return PTR_ERR(res);
	vram->pagemap.type = MEMORY_DEVICE_PRIVATE;
	vram->pagemap.range.start = res->start;
	vram->pagemap.range.end = res->end;
	vram->pagemap.nr_range = 1;
	vram->pagemap.owner = sdev;	/* hmm: "these pages are ours" */
	vram->pagemap.ops = drm_pagemap_pagemap_ops_get();
	addr = devm_memremap_pages(dev, &vram->pagemap);
	if (IS_ERR(addr))
		return PTR_ERR(addr);
	vram->base_pfn = PHYS_PFN(res->start);

	sdev->dpagemap = &vram->dpagemap;
	dev_info(dev, "VRAM: %llu MiB as device-private memory\n", size >> 20);
	return 0;
}
