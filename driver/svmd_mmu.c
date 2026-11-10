// SPDX-License-Identifier: GPL-2.0
/*
 * svmd_mmu.c - the device's page tables, and telling its TLB to forget.
 *
 * Tables are 4 KiB pages of 512 entries, four levels deep (spec §5), kept
 * in coherent DMA memory so the device's page walker reads what we write.
 *
 * The rules that keep this correct:
 *  - Tables are only ever ADDED while the VM exists (svmd_pt_prepare), and
 *    only freed when the whole VM goes. Zapping clears leaf entries only.
 *  - Allocation happens in svmd_pt_prepare, which callers run BEFORE taking
 *    the SVM notifier lock: allocating can trigger memory reclaim, reclaim
 *    can trigger the notifier, and the notifier needs that same lock.
 *  - After clearing entries, invalidate the TLB and WAIT for the device to
 *    confirm. Only then may the old pages be given to someone else.
 */

#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/pci.h>
#include <linux/slab.h>

#include "svmd.h"

#define PTE_TABLE	(SVMD_PTE_VALID | SVMD_PTE_WRITE)

static struct svmd_pt *svmd_pt_alloc(struct svmd_device *sdev, int level)
{
	struct svmd_pt *pt = kzalloc(sizeof(*pt), GFP_KERNEL);

	if (!pt)
		return NULL;
	pt->cpu = dma_alloc_coherent(&sdev->pdev->dev, SVMD_PAGE_SIZE, &pt->dma,
				     GFP_KERNEL);
	if (level < SVMD_PT_LEVELS - 1)
		pt->child = kcalloc(SVMD_PT_ENTRIES, sizeof(*pt->child),
				    GFP_KERNEL);
	if (!pt->cpu || (level < SVMD_PT_LEVELS - 1 && !pt->child)) {
		if (pt->cpu)
			dma_free_coherent(&sdev->pdev->dev, SVMD_PAGE_SIZE,
					  pt->cpu, pt->dma);
		kfree(pt->child);
		kfree(pt);
		return NULL;
	}
	return pt;
}

static void svmd_pt_free(struct svmd_device *sdev, struct svmd_pt *pt)
{
	if (!pt)
		return;
	if (pt->child)
		for (int i = 0; i < SVMD_PT_ENTRIES; i++)
			svmd_pt_free(sdev, pt->child[i]);
	dma_free_coherent(&sdev->pdev->dev, SVMD_PAGE_SIZE, pt->cpu, pt->dma);
	kfree(pt->child);
	kfree(pt);
}

/* Make sure every table on the way to `va`'s leaf entry exists. */
static int svmd_pt_prepare_one(struct svmd_vm *vm, u64 va)
{
	struct svmd_pt *pt = vm->root;

	for (int level = 0; level < SVMD_PT_LEVELS - 1; level++) {
		unsigned int i = SVMD_PT_INDEX(va, level);
		struct svmd_pt *next = READ_ONCE(pt->child[i]);

		if (!next) {
			next = svmd_pt_alloc(vm->sdev, level + 1);
			if (!next)
				return -ENOMEM;
			/* the entry first, then publish the pointer */
			WRITE_ONCE(pt->cpu[i], next->dma | PTE_TABLE);
			WRITE_ONCE(pt->child[i], next);
		}
		pt = next;
	}
	return 0;
}

/* Allocate tables for [start, end). Caller holds vm->lock, NOT the
 * notifier lock (see the top of this file). */
int svmd_pt_prepare(struct svmd_vm *vm, u64 start, u64 end)
{
	/* one leaf table covers 2 MiB */
	for (u64 va = start; va < end; va = (va | (SZ_2M - 1)) + 1) {
		int ret = svmd_pt_prepare_one(vm, va);

		if (ret)
			return ret;
	}
	return 0;
}

static struct svmd_pt *svmd_pt_leaf(struct svmd_vm *vm, u64 va)
{
	struct svmd_pt *pt = vm->root;

	for (int level = 0; pt && level < SVMD_PT_LEVELS - 1; level++)
		pt = READ_ONCE(pt->child[SVMD_PT_INDEX(va, level)]);
	return pt;
}

/* Write one leaf entry. Its tables must exist (svmd_pt_prepare). */
void svmd_pt_set(struct svmd_vm *vm, u64 va, u64 pte)
{
	struct svmd_pt *leaf = svmd_pt_leaf(vm, va);

	if (WARN_ON_ONCE(!leaf))
		return;
	WRITE_ONCE(leaf->cpu[SVMD_PT_INDEX(va, SVMD_PT_LEVELS - 1)], pte);
}

/* Clear the leaf entries of [start, end). Doesn't touch the TLB. */
void svmd_pt_zap(struct svmd_vm *vm, u64 start, u64 end)
{
	u64 va = start;

	while (va < end) {
		u64 next_2m = (va | (SZ_2M - 1)) + 1;	/* one leaf table each */
		struct svmd_pt *leaf = svmd_pt_leaf(vm, va);

		for (; leaf && va < end && va < next_2m; va += SVMD_PAGE_SIZE)
			WRITE_ONCE(leaf->cpu[SVMD_PT_INDEX(va, SVMD_PT_LEVELS - 1)],
				   0);
		va = next_2m;			/* (no table: nothing mapped) */
	}
}

/*
 * Tell the device to forget cached translations for [start, start+size)
 * (size 0: everything), and wait until it says it has (spec §2). Until
 * then it may still use the old ones - so callers must not free or reuse
 * the old pages before this returns.
 */
int svmd_tlb_invalidate(struct svmd_device *sdev, u64 start, u64 size)
{
	void __iomem *r = sdev->regs;
	u32 seqno, done;
	int ret;

	mutex_lock(&sdev->inval_lock);
	seqno = ++sdev->inval_seqno;
	writeq(start, r + SVMD_REG_INVAL_VA);
	writeq(size, r + SVMD_REG_INVAL_SIZE);
	/* writel's barrier: the zapped entries are visible before this */
	writel(seqno, r + SVMD_REG_INVAL_CMD);
	ret = readl_poll_timeout(r + SVMD_REG_INVAL_DONE, done,
				 svmd_seqno_passed(done, seqno), 10, 1000000);
	mutex_unlock(&sdev->inval_lock);
	if (ret)
		dev_err(&sdev->pdev->dev, "TLB invalidation %u never finished\n",
			seqno);
	return ret;
}

struct svmd_vm *svmd_vm_alloc(struct svmd_device *sdev)
{
	struct svmd_vm *vm = kzalloc(sizeof(*vm), GFP_KERNEL);
	int ret;

	if (!vm)
		return ERR_PTR(-ENOMEM);
	vm->sdev = sdev;
	kref_init(&vm->ref);
	mutex_init(&vm->lock);
	spin_lock_init(&vm->gc_lock);
	INIT_LIST_HEAD(&vm->gc_list);
	vm->root = svmd_pt_alloc(sdev, 0);
	if (!vm->root) {
		kfree(vm);
		return ERR_PTR(-ENOMEM);
	}
	/* faults name their VM by page-table root: make it findable */
	ret = xa_insert(&sdev->vms, vm->root->dma >> PAGE_SHIFT, vm, GFP_KERNEL);
	if (ret) {
		svmd_pt_free(sdev, vm->root);
		kfree(vm);
		return ERR_PTR(ret);
	}
	return vm;
}

void svmd_vm_get(struct svmd_vm *vm)
{
	kref_get(&vm->ref);
}

static void svmd_vm_release(struct kref *ref)
{
	struct svmd_vm *vm = container_of(ref, struct svmd_vm, ref);
	struct svmd_device *sdev = vm->sdev;

	xa_erase(&sdev->vms, vm->root->dma >> PAGE_SHIFT);
	if (vm->has_svm)
		svmd_svm_teardown(vm);
	/* no job uses this VM any more, but the TLB may still hold it */
	svmd_tlb_invalidate(sdev, 0, 0);
	svmd_pt_free(sdev, vm->root);
	mutex_destroy(&vm->lock);
	kfree(vm);
}

void svmd_vm_put(struct svmd_vm *vm)
{
	if (vm)
		kref_put(&vm->ref, svmd_vm_release);
}

/* Turn on translation. Kernel commands use PHYS, so they're unaffected. */
int svmd_mmu_init(struct svmd_device *sdev)
{
	mutex_init(&sdev->inval_lock);
	spin_lock_init(&sdev->inval_log_lock);
	xa_init(&sdev->vms);
	writel(SVMD_MMU_ENABLE, sdev->regs + SVMD_REG_MMU_CONTROL);
	return 0;
}
