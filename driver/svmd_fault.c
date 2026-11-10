// SPDX-License-Identifier: GPL-2.0
/*
 * svmd_fault.c - handling the device's page faults (spec §6).
 *
 * The device stops at a missing translation, puts a record in the fault
 * queue and raises the FAULT interrupt. The interrupt handler can't fix
 * anything - fixing may sleep (reading the process's page tables, taking
 * locks) - so it only wakes a worker. The worker reads each record, finds
 * the VM by the page-table root in it, asks that VM to resolve the fault,
 * and tells the device: RETRY (now mapped) or ABORT (it never will be;
 * the job fails with -EFAULT and the engine moves on to the next job).
 *
 * Known simplification: real drivers keep fault handling out of the
 * dma_fence signalling path (a faulting job can't be part of memory
 * reclaim's wait). Ours is in it - fine for a teaching device.
 */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/pci.h>
#include <linux/unaligned.h>

#include "svmd.h"
#include "svmd_trace.h"

struct svmd_fault_rec {
	u64 va, root;
	u32 access, reason, slot, level;
};

static void svmd_fault_read(struct svmd_device *sdev, u32 i,
			    struct svmd_fault_rec *f)
{
	const u8 *raw = sdev->faultq + i * SVMD_FAULT_REC_SIZE;

	f->va = get_unaligned_le64(raw + SVMD_FAULT_VA);
	f->access = get_unaligned_le32(raw + SVMD_FAULT_ACCESS);
	f->reason = get_unaligned_le32(raw + SVMD_FAULT_REASON);
	f->slot = get_unaligned_le32(raw + SVMD_FAULT_SLOT);
	f->level = get_unaligned_le32(raw + SVMD_FAULT_LEVEL);
	f->root = get_unaligned_le64(raw + SVMD_FAULT_ROOT);
}

/* Which VM does a fault belong to? Takes a reference. */
static struct svmd_vm *svmd_vm_lookup(struct svmd_device *sdev, u64 root)
{
	struct svmd_vm *vm;

	xa_lock(&sdev->vms);
	vm = xa_load(&sdev->vms, root >> PAGE_SHIFT);
	if (vm && !kref_get_unless_zero(&vm->ref))
		vm = NULL;
	xa_unlock(&sdev->vms);
	return vm;
}

static void svmd_fault_handle(struct svmd_device *sdev,
			      const struct svmd_fault_rec *f)
{
	bool write = f->access & SVMD_ACCESS_WRITE;
	struct svmd_vm *vm = svmd_vm_lookup(sdev, f->root);
	u64 t0 = ktime_get_ns();
	int ret = -EFAULT;

	trace_svmd_fault(f->va, f->access, f->reason, f->root);
	if (sdev->fault_delay_ms)		/* tests: a slow fault */
		msleep(sdev->fault_delay_ms);

	if (vm && vm->resolve)
		ret = vm->resolve(vm, f->va, write);
	if (vm && READ_ONCE(sdev->audit))
		svmd_svm_audit(vm);
	svmd_vm_put(vm);
	trace_svmd_fault_done(f->va, ret, ktime_get_ns() - t0);

	if (!ret) {
		sdev->faults_resolved++;
		writel(SVMD_RESUME_RETRY, sdev->regs + SVMD_REG_FAULT_RESUME);
		return;
	}
	dev_dbg(&sdev->pdev->dev, "fault at 0x%llx (%s, reason %u): %d, aborting job\n",
		f->va, write ? "write" : "read", f->reason, ret);
	sdev->faults_aborted++;
	sdev->fault_last_error = ret;
	/* the job running now is the oldest unfinished one: fail it */
	svmd_fence_fail_current(sdev, -EFAULT);
	writel(SVMD_RESUME_ABORT, sdev->regs + SVMD_REG_FAULT_RESUME);
}

static void svmd_fault_work(struct work_struct *work)
{
	struct svmd_device *sdev = container_of(work, struct svmd_device,
						fault_work);
	struct svmd_fault_rec f, last = {};
	u32 tail;

	/* one batch = the records already in the queue when we look */
	while ((tail = readl(sdev->regs + SVMD_REG_FAULTQ_TAIL)) !=
	       sdev->faultq_head) {
		bool have_last = false;

		/* the device wrote the records before moving the tail */
		dma_rmb();
		while (sdev->faultq_head != tail) {
			svmd_fault_read(sdev, sdev->faultq_head, &f);
			sdev->faultq_head = (sdev->faultq_head + 1) %
					    SVMD_FAULTQ_RECS;
			writel(sdev->faultq_head,
			       sdev->regs + SVMD_REG_FAULTQ_HEAD);

			/*
			 * The same fault reported twice (hardware may) must be
			 * handled once: a second RESUME could land on a LATER
			 * stall and, if it said ABORT, kill an innocent job.
			 * Only within one batch, though: the device can't stall
			 * again until we've answered, so an identical record in
			 * a LATER batch is a real new fault (e.g. the CPU
			 * unmapped the page again) and must be handled.
			 */
			if (have_last && !memcmp(&f, &last, sizeof(f))) {
				sdev->faults_duplicate++;
				continue;
			}
			last = f;
			have_last = true;
			svmd_fault_handle(sdev, &f);
		}
	}
}

/* FAULT interrupt (interrupt context): just wake the worker. */
void svmd_fault_irq(struct svmd_device *sdev)
{
	queue_work(sdev->fault_wq, &sdev->fault_work);
}

int svmd_fault_init(struct svmd_device *sdev)
{
	struct device *dev = &sdev->pdev->dev;

	sdev->faultq = dmam_alloc_coherent(dev, SVMD_FAULTQ_RECS *
					   SVMD_FAULT_REC_SIZE,
					   &sdev->faultq_dma, GFP_KERNEL);
	if (!sdev->faultq)
		return -ENOMEM;
	/* ordered: one fault at a time, in the order the device reported */
	sdev->fault_wq = alloc_ordered_workqueue("svmd-fault", WQ_MEM_RECLAIM);
	if (!sdev->fault_wq)
		return -ENOMEM;
	INIT_WORK(&sdev->fault_work, svmd_fault_work);

	writeq(sdev->faultq_dma, sdev->regs + SVMD_REG_FAULTQ_BASE);
	writel(SVMD_FAULTQ_RECS, sdev->regs + SVMD_REG_FAULTQ_SIZE);
	sdev->faultq_head = 0;
	return 0;
}

void svmd_fault_fini(struct svmd_device *sdev)
{
	if (sdev->fault_wq)
		destroy_workqueue(sdev->fault_wq);	/* waits for the worker */
}
