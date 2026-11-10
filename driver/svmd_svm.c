// SPDX-License-Identifier: GPL-2.0
/*
 * svmd_svm.c - shared virtual memory: the device uses a process's own
 * pointers, through drm_gpusvm (the kernel's GPU SVM helper).
 *
 * Each open file's VM mirrors the memory of the process that opened it.
 * Nothing is mapped up front. The device faults (spec §6), and we:
 *
 *   1. find or create a drm_gpusvm "range" around the address (a 4K-2M
 *      chunk that fits in one CPU mapping),
 *   2. get its pages - drm_gpusvm asks the core MM (hmm_range_fault) for
 *      them and DMA-maps them for the device,
 *   3. write the page-table entries - but only if, under the notifier
 *      lock, the pages are still valid: the CPU side may have changed
 *      them since step 2. If so, start again.
 *
 * The other direction is the MMU notifier: when the CPU side changes
 * (munmap, migration, fork's copy-on-write...), the core MM calls
 * svmd_invalidate BEFORE it frees anything. We clear the entries, flush
 * the device's TLB and wait for it - the order from the design notes -
 * and only then return. Unmapped ranges go on a list that the next fault
 * (or teardown) cleans up: the "garbage collector".
 *
 * Phase 6: if the process asked for VRAM, a faulted range is first
 * migrated into the device's own memory (svmd_vram.c); its entries then
 * point into VRAM. When the CPU touches such a page, the core MM migrates
 * it back and our notifier clears the entries - the next device access
 * faults again. If VRAM is full, the range just stays in system memory.
 *
 * This follows the worked example at the top of drm_gpusvm.c.
 */

#include <linux/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/slab.h>
#include <drm/drm_gpusvm.h>
#include <drm/drm_pagemap.h>

#include "svmd.h"
#include "svmd_trace.h"

/* Range sizes drm_gpusvm may pick, largest first. */
static const unsigned long svmd_chunk_sizes[] = { SZ_2M, SZ_64K, SZ_4K };

static struct svmd_vm *gpusvm_to_vm(struct drm_gpusvm *gpusvm)
{
	return container_of(gpusvm, struct svmd_vm, svm);
}

static struct svmd_range *to_svmd_range(struct drm_gpusvm_range *r)
{
	return container_of(r, struct svmd_range, base);
}

static struct drm_gpusvm_range *svmd_range_alloc(struct drm_gpusvm *gpusvm)
{
	struct svmd_range *r = kzalloc(sizeof(*r), GFP_KERNEL);

	if (!r)
		return NULL;
	drm_gpusvm_init_pages(&r->pages, &gpusvm_to_vm(gpusvm)->sdev->drm);
	INIT_LIST_HEAD(&r->gc_link);
	return &r->base;
}

static void svmd_range_free(struct drm_gpusvm_range *range)
{
	struct svmd_range *r = to_svmd_range(range);

	/* the last word: unmaps anything still DMA-mapped, frees the array */
	drm_gpusvm_free_pages(range->gpusvm, &r->pages,
			      drm_gpusvm_range_size(range) >> PAGE_SHIFT);
	kfree(r);
}

/*
 * The CPU side of [start, end) is about to change. Called by the core MM
 * with the notifier lock held for writing; may sleep. When this returns,
 * the device must not be able to reach the old pages.
 */
static void svmd_invalidate(struct drm_gpusvm *gpusvm,
			    struct drm_gpusvm_notifier *notifier,
			    const struct mmu_notifier_range *mmu_range)
{
	struct svmd_vm *vm = gpusvm_to_vm(gpusvm);
	struct drm_gpusvm_ctx ctx = { .in_notifier = true };
	unsigned long start = max(mmu_range->start,
				  drm_gpusvm_notifier_start(notifier));
	unsigned long end = min(mmu_range->end,
				drm_gpusvm_notifier_end(notifier));
	unsigned long zap_start = ULONG_MAX, zap_end = 0;
	struct drm_gpusvm_range *range = NULL;
	u64 t0;

	/*
	 * Clear the WHOLE of every range this touches, not just [start,
	 * end): below, each range's pages are DMA-unmapped as a whole and its
	 * pages become invalid as a whole, so no entry of it may survive. (A
	 * range only partly unmapped is later removed; entries left for its
	 * other part would point at pages nobody tracks - found by the churn
	 * stress test as corrupted data. xe does the same.)
	 */
	unsigned int nranges = 0;

	drm_gpusvm_for_each_range(range, notifier, start, end) {
		zap_start = min(zap_start, drm_gpusvm_range_start(range));
		zap_end = max(zap_end, drm_gpusvm_range_end(range));
		to_svmd_range(range)->invals++;
		nranges++;
	}
	svmd_log(vm->sdev, 'Z', mmu_range->start, mmu_range->end, mmu_range->event);
	if (zap_start >= zap_end)
		return;				/* nothing of ours here */

	/* 1. entries, 2. TLB (and wait), 3. only then the DMA mappings */
	t0 = ktime_get_ns();
	svmd_pt_zap(vm, zap_start, zap_end);
	svmd_tlb_invalidate(vm->sdev, zap_start, zap_end - zap_start);
	trace_svmd_invalidate(zap_start, zap_end, mmu_range->event,
			      ktime_get_ns() - t0);

	/*
	 * NULL first: drm_gpusvm_for_each_range() STARTS FROM `range` if it
	 * isn't NULL, and the loop above can leave it pointing at a range
	 * past `end`. Then this loop ran zero times: entries were cleared
	 * but pages were never unmapped or marked invalid, and the next
	 * fault re-bound those stale pages - the device wrote into memory
	 * the process had freed (found with the audit + flight recorder).
	 */
	range = NULL;
	drm_gpusvm_for_each_range(range, notifier, start, end) {
		struct svmd_range *r = to_svmd_range(range);

		svmd_log(vm->sdev, 'U', drm_gpusvm_range_start(range),
			 drm_gpusvm_range_end(range),
			 range->flags.__flags | (u64)r->pages.flags.__flags << 16);
		drm_gpusvm_unmap_pages(gpusvm, &r->pages,
				       drm_gpusvm_range_size(range) >> PAGE_SHIFT,
				       &ctx);
		/*
		 * Gone from the CPU side: remove the range at the next fault.
		 * Only the FIRST unmap queues it (the flag never clears): the
		 * collector takes a range off the list before removing it, so
		 * a second munmap in that window would otherwise queue it
		 * again, and it would be removed twice (found by the churn
		 * stress test: a WARN in drm_gpusvm_range_remove).
		 */
		if (mmu_range->event != MMU_NOTIFY_UNMAP || range->flags.unmapped)
			continue;
		drm_gpusvm_range_set_unmapped(range, &r->pages, 1, mmu_range);
		spin_lock(&vm->gc_lock);
		if (list_empty(&r->gc_link)) {
			drm_gpusvm_range_get(range);
			list_add_tail(&r->gc_link, &vm->gc_list);
		}
		spin_unlock(&vm->gc_lock);
	}
}

void svmd_log(struct svmd_device *sdev, char type, u64 start, u64 end, u64 info)
{
	unsigned long flags;
	u32 n;

	if (!READ_ONCE(sdev->audit))		/* only while debugging */
		return;
	spin_lock_irqsave(&sdev->inval_log_lock, flags);
	if (!sdev->inval_log_frozen) {
		n = sdev->inval_log_next++;
		sdev->inval_log[n % SVMD_INVAL_LOG] =
			(struct svmd_inval_log){ start, end, info, type, n };
	}
	spin_unlock_irqrestore(&sdev->inval_log_lock, flags);
}

static const struct drm_gpusvm_ops svmd_gpusvm_ops = {
	.range_alloc = svmd_range_alloc,
	.range_free = svmd_range_free,
	.invalidate = svmd_invalidate,
};

/* Remove ranges the CPU unmapped. Caller holds vm->lock. */
static void svmd_garbage_collect(struct svmd_vm *vm)
{
	struct svmd_range *r;

	lockdep_assert_held(&vm->lock);
	for (;;) {
		spin_lock(&vm->gc_lock);
		r = list_first_entry_or_null(&vm->gc_list, struct svmd_range,
					     gc_link);
		if (r)
			list_del_init(&r->gc_link);
		spin_unlock(&vm->gc_lock);
		if (!r)
			return;

		/*
		 * Unbind: the device must not reach any of the range once it's
		 * gone. (The notifier cleared it already; do it again here so
		 * removal never depends on that - the kernel's example GC does
		 * the same with driver_unbind_range().) Then DMA-unmap, BEFORE
		 * leaving the tree (see drm_gpusvm.c).
		 */
		svmd_log(vm->sdev, 'G', drm_gpusvm_range_start(&r->base),
			 drm_gpusvm_range_end(&r->base), 0);
		svmd_pt_zap(vm, drm_gpusvm_range_start(&r->base),
			    drm_gpusvm_range_end(&r->base));
		svmd_tlb_invalidate(vm->sdev, drm_gpusvm_range_start(&r->base),
				    drm_gpusvm_range_size(&r->base));
		drm_gpusvm_unmap_pages(&vm->svm, &r->pages,
				       drm_gpusvm_range_size(&r->base) >> PAGE_SHIFT,
				       &(struct drm_gpusvm_ctx){});
		drm_gpusvm_range_remove(&vm->svm, &r->base);
		drm_gpusvm_range_put(&r->base);
	}
}

/*
 * Step 3: write the entries for every page of the range - under the
 * notifier lock, and only if the pages are still the ones we got.
 * Returns -EAGAIN if the CPU side changed in between.
 */
static int svmd_bind_range(struct svmd_vm *vm, struct svmd_range *r,
			   bool writable)
{
	unsigned long start = drm_gpusvm_range_start(&r->base);
	unsigned long npages = drm_gpusvm_range_size(&r->base) >> PAGE_SHIFT;
	u64 flags = SVMD_PTE_VALID | (writable ? SVMD_PTE_WRITE : 0);
	unsigned long i, j;
	int ret;

	/* allocate tables now: never while holding the notifier lock */
	ret = svmd_pt_prepare(vm, start, start + (npages << PAGE_SHIFT));
	if (ret)
		return ret;

	drm_gpusvm_notifier_lock(&vm->svm);
	if (!drm_gpusvm_pages_valid(&vm->svm, &r->pages)) {
		drm_gpusvm_notifier_unlock(&vm->svm);
		return -EAGAIN;
	}
	svmd_log(vm->sdev, 'B', start, start + (npages << PAGE_SHIFT),
		 r->pages.dma_addr[0].addr);
	/* dma_addr[] is compact: entry j covers (1 << order) pages */
	for (i = 0, j = 0; i < npages; j++) {
		struct drm_pagemap_addr *a = &r->pages.dma_addr[j];
		unsigned long n = 1ul << a->order;
		/* our VRAM: the address is an offset into BAR2 (spec §5) */
		u64 vram = a->proto == DRM_INTERCONNECT_DRIVER ? SVMD_PTE_VRAM : 0;

		for (unsigned long k = 0; k < n; k++)
			svmd_pt_set(vm, start + ((i + k) << PAGE_SHIFT),
				    (a->addr + (k << PAGE_SHIFT)) | flags | vram);
		i += n;
	}
	drm_gpusvm_notifier_unlock(&vm->svm);
	return 0;
}

/* Is `va` in a CPU mapping, and may the device write it? */
static int svmd_vma_access(struct mm_struct *mm, u64 va, bool *writable)
{
	struct vm_area_struct *vma;
	int ret = -EFAULT;

	if (!mmget_not_zero(mm))
		return -EFAULT;			/* the process is exiting */
	mmap_read_lock(mm);
	vma = vma_lookup(mm, va);
	if (vma && (vma->vm_flags & VM_READ)) {
		*writable = vma->vm_flags & VM_WRITE;
		ret = 0;
	}
	mmap_read_unlock(mm);
	mmput(mm);
	return ret;
}

/* A device fault at `va` in a process's VM: map it, or say why not. */
static int svmd_svm_resolve(struct svmd_vm *vm, u64 va, bool write)
{
	struct svmd_device *sdev = vm->sdev;
	bool migrate = sdev->dpagemap && READ_ONCE(vm->prefer_vram);
	struct drm_gpusvm_ctx ctx = {
		/* pages already in OUR VRAM are fine as they are */
		.device_private_page_owner = sdev,
		.devmem_possible = migrate,
		.allow_mixed = 1,		/* part VRAM, part system */
	};
	struct drm_gpusvm_range *range;
	struct svmd_range *r;
	bool writable, tried_vram = false;
	int ret, tries = 0;

	ret = svmd_vma_access(vm->svm.mm, va, &writable);
	if (ret)
		return ret;			/* not mapped: abort the job */
	if (write && !writable)
		return -EACCES;			/* read-only memory: abort */
	ctx.read_only = !writable;

	mutex_lock(&vm->lock);
retry:
	if (++tries > 16) {			/* the CPU side keeps changing */
		ret = -EBUSY;
		goto out;
	}
	/* process unmaps first, so we see the current set of ranges */
	svmd_garbage_collect(vm);

	range = drm_gpusvm_range_find_or_insert(&vm->svm, va, vm->svm.mm_start,
						vm->svm.mm_start + vm->svm.mm_range,
						&ctx);
	if (IS_ERR(range)) {
		ret = PTR_ERR(range);
		goto out;
	}
	r = to_svmd_range(range);

	ret = drm_gpusvm_get_pages(&vm->svm, &r->pages, vm->svm.mm,
				   &range->notifier->notifier,
				   drm_gpusvm_range_start(range),
				   drm_gpusvm_range_end(range), &ctx);
	if (ret == -EOPNOTSUPP || ret == -EFAULT || ret == -EPERM) {
		/* CPU mappings changed under us */
		if (ret == -EOPNOTSUPP)
			drm_gpusvm_range_evict(&vm->svm, range);
		goto retry;
	}
	if (ret)
		goto out;

	/*
	 * Phase 6: move the range into VRAM, then fetch its pages again.
	 * Migrating AFTER get_pages, not before, on purpose: get_pages gives
	 * never-touched memory real pages first. An empty 2 MiB-aligned range
	 * would otherwise be offered to migration as one huge (2 MiB) entry,
	 * which our 4 KiB VRAM pages can't take (-EBUSY) - found by the vram
	 * test. Best effort: if it fails (VRAM full...), stay in system RAM.
	 */
	if (migrate && writable && !tried_vram &&
	    !r->pages.flags.has_devmem_pages) {
		int err = drm_pagemap_populate_mm(sdev->dpagemap,
						  drm_gpusvm_range_start(range),
						  drm_gpusvm_range_end(range),
						  vm->svm.mm, 0);
		tried_vram = true;
		if (err) {
			sdev->migrations_failed++;
			sdev->migrate_last_error = err;
		} else {
			sdev->migrations++;
			goto retry;		/* pages are now VRAM pages */
		}
	}

	ret = svmd_bind_range(vm, r, writable);
	if (ret == -EAGAIN)
		goto retry;
out:
	mutex_unlock(&vm->lock);
	return ret;
}

/*
 * Debug check (debugfs svmd_audit = 1): every valid device entry must lie
 * in a range whose pages are currently valid. A violation means an entry
 * outlived the memory it points to - the device could write into a page
 * that belongs to someone else. Walks every table: slow, tests only.
 */
static void svmd_audit_walk(struct svmd_vm *vm, struct svmd_pt *pt, int level,
			    u64 base)
{
	struct svmd_device *sdev = vm->sdev;

	for (int i = 0; i < SVMD_PT_ENTRIES; i++) {
		u64 va = base | ((u64)i << (SVMD_PAGE_SHIFT +
					   9 * (SVMD_PT_LEVELS - 1 - level)));
		struct drm_gpusvm_notifier *n;
		struct drm_gpusvm_range *range = NULL;

		if (level < SVMD_PT_LEVELS - 1) {
			if (pt->child[i])
				svmd_audit_walk(vm, pt->child[i], level + 1, va);
			continue;
		}
		if (!(READ_ONCE(pt->cpu[i]) & SVMD_PTE_VALID))
			continue;
		n = drm_gpusvm_notifier_find(&vm->svm, va, va + 1);
		if (n)
			range = drm_gpusvm_range_find(n, va, va + 1);
		if (range && drm_gpusvm_pages_valid(&vm->svm,
				&to_svmd_range(range)->pages)) {
			/* stronger: does the CPU map the same page here? */
			struct page *page = NULL;
			u64 pte = READ_ONCE(pt->cpu[i]);

			if (pte & SVMD_PTE_VRAM)
				continue;
			if (get_user_pages_remote(vm->svm.mm, va, 1, FOLL_NOFAULT,
						  &page, NULL) == 1) {
				bool same = page_to_phys(page) ==
					    (pte & SVMD_PTE_ADDR_MASK);
				put_page(page);
				if (same)
					continue;
				/* was the range invalidated after its pages were
				 * fetched, without our callback noticing? */
				if (mmu_interval_check_retry(&n->notifier,
					to_svmd_range(range)->pages.notifier_seq))
					sdev->audit_seq_changed++;
				/* does the entry match the range's own page list? */
				{
					struct svmd_range *r = to_svmd_range(range);
					unsigned long idx = (va - drm_gpusvm_range_start(range)) >> PAGE_SHIFT;
					unsigned long ii = 0, jj = 0;

					while (ii + (1ul << r->pages.dma_addr[jj].order) <= idx) {
						ii += 1ul << r->pages.dma_addr[jj].order;
						jj++;
					}
					if ((pte & SVMD_PTE_ADDR_MASK) ==
					    r->pages.dma_addr[jj].addr + ((idx - ii) << PAGE_SHIFT))
						sdev->audit_pte_matches_pages++;
				}
				if (!sdev->audit_mismatch++) {
					sdev->inval_log_frozen = true;
					sdev->audit_first = va | 2;	/* bit1: wrong page */
					sdev->audit_first_invals = to_svmd_range(range)->invals;
					sdev->audit_range_start = drm_gpusvm_range_start(range);
					sdev->audit_range_end = drm_gpusvm_range_end(range);
				}
			}
			continue;
		}
		if (!sdev->audit_violations++)
			sdev->audit_first = va | (range ? 1 : 0);	/* bit0: had a range */
	}
}

void svmd_svm_audit(struct svmd_vm *vm)
{
	if (!vm->has_svm)
		return;
	if (!mmget_not_zero(vm->svm.mm))
		return;
	mutex_lock(&vm->lock);
	/* lock order as the core MM uses it: mmap_lock, then notifier lock */
	mmap_read_lock(vm->svm.mm);
	drm_gpusvm_notifier_lock(&vm->svm);
	svmd_audit_walk(vm, vm->root, 0, 0);
	drm_gpusvm_notifier_unlock(&vm->svm);
	mmap_read_unlock(vm->svm.mm);
	mutex_unlock(&vm->lock);
	mmput(vm->svm.mm);
}

/* A VM that mirrors `mm`. Faults in it are resolved from the process. */
struct svmd_vm *svmd_svm_create(struct svmd_device *sdev, struct mm_struct *mm)
{
	struct svmd_vm *vm = svmd_vm_alloc(sdev);
	int ret;

	if (IS_ERR(vm))
		return vm;
	/* the whole 48-bit space the device can address: CPU pointers as-is */
	ret = drm_gpusvm_init(&vm->svm, "svmd", mm,
			      0, 1ull << SVMD_VA_BITS, SZ_512M,
			      &svmd_gpusvm_ops, svmd_chunk_sizes,
			      ARRAY_SIZE(svmd_chunk_sizes));
	if (ret) {
		svmd_vm_put(vm);
		return ERR_PTR(ret);
	}
	/* tell lockdep which lock serialises range insert/remove */
	drm_gpusvm_driver_set_lock(&vm->svm, &vm->lock);
	vm->has_svm = true;
	vm->resolve = svmd_svm_resolve;
	return vm;
}

/* Last reference gone: no job uses this VM. Drop every range. */
void svmd_svm_teardown(struct svmd_vm *vm)
{
	mutex_lock(&vm->lock);
	svmd_garbage_collect(vm);
	mutex_unlock(&vm->lock);
	drm_gpusvm_fini(&vm->svm);
}
