// SPDX-License-Identifier: GPL-2.0
/*
 * svmd_ring.c - the hardware side of running a job.
 *
 *   emit:    write the job's commands into the ring, then a FENCE, then
 *            ring the doorbell (RING_TAIL)                  (spec §3, §4)
 *   finish:  the device writes the FENCE's seqno to fence_cpu and raises
 *            the JOB interrupt; we signal every hardware fence that has
 *            now completed, and drm_sched takes it from there
 */

#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/pci.h>
#include <linux/unaligned.h>

#include "svmd.h"

#define RING_MASK	(SVMD_RING_SLOTS - 1)

/* Lay one command out exactly as §4 says: 32 little-endian bytes. */
void svmd_encode_cmd(u8 *slot, u8 op, u8 flags, u32 arg32,
		     u64 dst, u64 src, u64 len)
{
	slot[0] = op;
	slot[1] = flags;
	put_unaligned_le16(0, slot + 2);
	put_unaligned_le32(arg32, slot + 4);
	put_unaligned_le64(dst, slot + 8);
	put_unaligned_le64(src, slot + 16);
	put_unaligned_le64(len, slot + 24);
}

static const char *svmd_fence_driver_name(struct dma_fence *f)
{
	return "svmd";
}

static const char *svmd_fence_timeline_name(struct dma_fence *f)
{
	return "svmd-ring";
}

static const struct dma_fence_ops svmd_fence_ops = {
	.get_driver_name = svmd_fence_driver_name,
	.get_timeline_name = svmd_fence_timeline_name,
};

static void svmd_fence_dwork(struct work_struct *work)
{
	svmd_fence_process(container_of(work, struct svmd_device,
					fence_dwork.work));
}

int svmd_ring_init(struct svmd_device *sdev)
{
	struct device *dev = &sdev->pdev->dev;
	void __iomem *r = sdev->regs;

	spin_lock_init(&sdev->fence_lock);
	INIT_LIST_HEAD(&sdev->pending_fences);
	INIT_DELAYED_WORK(&sdev->fence_dwork, svmd_fence_dwork);
	sdev->fence_context = dma_fence_context_alloc(1);

	/* "coherent": CPU and device see each other's writes, no flushing */
	sdev->ring = dmam_alloc_coherent(dev, SVMD_RING_SLOTS * SVMD_CMD_SIZE,
					 &sdev->ring_dma, GFP_KERNEL);
	sdev->fence_cpu = dmam_alloc_coherent(dev, sizeof(u32),
					      &sdev->fence_dma, GFP_KERNEL);
	if (!sdev->ring || !sdev->fence_cpu)
		return -ENOMEM;

	writel(SVMD_CONTROL_RESET, r + SVMD_REG_CONTROL);
	writeq(sdev->ring_dma, r + SVMD_REG_RING_BASE);
	writel(SVMD_RING_SLOTS, r + SVMD_REG_RING_SIZE);
	writeq(sdev->fence_dma, r + SVMD_REG_FENCE_ADDR);
	writel(SVMD_CONTROL_ENGINE_ENABLE, r + SVMD_REG_CONTROL);
	return 0;
}

/* Make the device stop touching memory. Must happen before any buffer it
 * might be using is released. */
void svmd_ring_stop(struct svmd_device *sdev)
{
	writel(SVMD_CONTROL_RESET, sdev->regs + SVMD_REG_CONTROL);
	cancel_delayed_work_sync(&sdev->fence_dwork);
}

/* Free slots, keeping one empty so "full" and "empty" look different. */
static u32 svmd_ring_space(struct svmd_device *sdev)
{
	u32 head = readl(sdev->regs + SVMD_REG_RING_HEAD);

	return SVMD_RING_SLOTS - 1 - ((sdev->tail - head) & RING_MASK);
}

/*
 * Put a job on the ring. Called only from run_job. There is always room:
 * the scheduler's credit limit (SVMD_RING_SLOTS - 1) counts each job as
 * num_cmds + 2 slots (SET_ROOT, commands, FENCE) and only returns them when
 * its fence signals - which is exactly when the device moves past them.
 */
void svmd_ring_emit(struct svmd_device *sdev, struct svmd_job *job)
{
	struct svmd_fence *f = job->hw_fence;
	unsigned long flags;
	unsigned int i;

	WARN_ON_ONCE(svmd_ring_space(sdev) < job->num_cmds + 2);

	/* 1. the commands, in plain memory - first switching to the job's
	 * address space, if it has one (this also empties the TLB) */
	if (job->vm) {
		svmd_encode_cmd(sdev->ring + sdev->tail * SVMD_CMD_SIZE,
				SVMD_OP_SET_ROOT, 0, 0, job->vm->root->dma, 0, 0);
		sdev->tail = (sdev->tail + 1) & RING_MASK;
	}
	for (i = 0; i < job->num_cmds; i++) {
		memcpy(sdev->ring + sdev->tail * SVMD_CMD_SIZE, job->cmds[i],
		       SVMD_CMD_SIZE);
		sdev->tail = (sdev->tail + 1) & RING_MASK;
	}
	dma_fence_init(&f->base, &svmd_fence_ops, &sdev->fence_lock,
		       sdev->fence_context, ++sdev->last_seqno);
	svmd_encode_cmd(sdev->ring + sdev->tail * SVMD_CMD_SIZE,
			SVMD_OP_FENCE, SVMD_CMD_IRQ, sdev->last_seqno, 0, 0, 0);
	sdev->tail = (sdev->tail + 1) & RING_MASK;

	/* 2. track the fence BEFORE the device can possibly reach it */
	spin_lock_irqsave(&sdev->fence_lock, flags);
	list_add_tail(&f->link, &sdev->pending_fences);
	dma_fence_get(&f->base);		/* the list's reference */
	spin_unlock_irqrestore(&sdev->fence_lock, flags);

	/*
	 * 3. the doorbell. writel() (not writel_relaxed) puts a barrier
	 * before the register write, so the device can never see the new
	 * tail before the slots it points to.
	 */
	writel(sdev->tail, sdev->regs + SVMD_REG_RING_TAIL);
}

/* JOB interrupt: signal every fence the device has passed, in order. */
void svmd_fence_process(struct svmd_device *sdev)
{
	u32 done = READ_ONCE(*sdev->fence_cpu);
	struct svmd_fence *f, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&sdev->fence_lock, flags);
	list_for_each_entry_safe(f, tmp, &sdev->pending_fences, link) {
		if (!svmd_seqno_passed(done, f->base.seqno))
			break;
		list_del_init(&f->link);
		dma_fence_signal_locked(&f->base);
		dma_fence_put(&f->base);
	}
	spin_unlock_irqrestore(&sdev->fence_lock, flags);
}

/*
 * The device will never finish what's pending (it stopped, or hung):
 * complete every pending fence with `error` so nobody waits forever.
 * Call only after the device has been stopped.
 */
void svmd_fence_fail_all(struct svmd_device *sdev, int error)
{
	struct svmd_fence *f, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&sdev->fence_lock, flags);
	list_for_each_entry_safe(f, tmp, &sdev->pending_fences, link) {
		list_del_init(&f->link);
		dma_fence_set_error(&f->base, error);
		dma_fence_signal_locked(&f->base);
		dma_fence_put(&f->base);
	}
	spin_unlock_irqrestore(&sdev->fence_lock, flags);
}

/*
 * The device is about to skip the rest of the running job (a fault that
 * can't be fixed): mark that job's fence failed. It still signals normally
 * when the device reaches the job's FENCE.
 *
 * Which job is running? The oldest one the device hasn't PASSED - not
 * simply the oldest on our list: the device may have finished a job and
 * faulted in the next one before the JOB interrupt retired the first.
 * Marking the oldest listed fence then failed a job that had succeeded,
 * and let the aborted one report success with its memory never written
 * (found by the churn stress test: two threads, back-to-back jobs).
 */
void svmd_fence_fail_current(struct svmd_device *sdev, int error)
{
	u32 done = READ_ONCE(*sdev->fence_cpu);
	struct svmd_fence *f;
	unsigned long flags;

	spin_lock_irqsave(&sdev->fence_lock, flags);
	list_for_each_entry(f, &sdev->pending_fences, link) {
		if (!svmd_seqno_passed(done, f->base.seqno)) {
			dma_fence_set_error(&f->base, error);
			break;
		}
	}
	spin_unlock_irqrestore(&sdev->fence_lock, flags);
}
