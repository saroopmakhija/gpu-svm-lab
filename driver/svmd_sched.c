// SPDX-License-Identifier: GPL-2.0
/*
 * svmd_sched.c - glue between the device and drm_sched, the kernel's
 * shared GPU job scheduler.
 *
 * drm_sched does the generic work: one queue ("entity") per open file,
 * waiting for a job's dependencies (dma_fences) before running it, never
 * putting more on the ring than fits (credits), noticing hangs (timeout).
 * We supply four callbacks:
 *
 *   run_job      put the job on the ring, return its hardware fence
 *   timedout_job the device took too long: stop it, fail what's pending
 *   free_job     the job is done: release its buffers
 *   cancel_job   the scheduler is shutting down: fail this job's fence
 */

#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>

#include "svmd.h"

/*
 * The device can no longer be trusted to finish work (error interrupt or
 * timeout). Stop it FIRST - completing a fence may let a buffer be freed,
 * so the device must already be done with memory - then fail every
 * pending fence. Phase 2b has no recovery: later jobs fail at run_job.
 */
void svmd_engine_wedge(struct svmd_device *sdev, int error)
{
	WRITE_ONCE(sdev->engine_dead, true);
	svmd_ring_stop(sdev);
	svmd_fence_fail_all(sdev, error);
}

static struct dma_fence *svmd_run_job(struct drm_sched_job *sched_job)
{
	struct svmd_job *job = to_svmd_job(sched_job);
	struct svmd_device *sdev = container_of(sched_job->sched,
						struct svmd_device, sched);

	/* a job this one waited on failed: don't run, report its error */
	if (unlikely(sched_job->s_fence->finished.error))
		return NULL;
	if (unlikely(READ_ONCE(sdev->engine_dead)))
		return ERR_PTR(-EIO);

	svmd_ring_emit(sdev, job);
	/* the scheduler gets its own reference to the hardware fence */
	return dma_fence_get(&job->hw_fence->base);
}

static enum drm_gpu_sched_stat svmd_timedout_job(struct drm_sched_job *sched_job)
{
	struct svmd_device *sdev = container_of(sched_job->sched,
						struct svmd_device, sched);

	/* finished in the meantime? (the interrupt can race the timer) */
	if (dma_fence_is_signaled(&to_svmd_job(sched_job)->hw_fence->base))
		return DRM_GPU_SCHED_STAT_NO_HANG;

	dev_err(&sdev->pdev->dev, "job timed out (head %u, status 0x%x)\n",
		readl(sdev->regs + SVMD_REG_RING_HEAD),
		readl(sdev->regs + SVMD_REG_STATUS));
	svmd_engine_wedge(sdev, -ETIMEDOUT);
	return DRM_GPU_SCHED_STAT_RESET;
}

static void svmd_free_job(struct drm_sched_job *sched_job)
{
	drm_sched_job_cleanup(sched_job);
	svmd_job_free(to_svmd_job(sched_job));
}

static void svmd_cancel_job(struct drm_sched_job *sched_job)
{
	struct svmd_device *sdev = container_of(sched_job->sched,
						struct svmd_device, sched);
	struct svmd_fence *f = to_svmd_job(sched_job)->hw_fence;
	unsigned long flags;

	if (!dma_fence_was_initialized(&f->base))	/* never reached run_job */
		return;
	spin_lock_irqsave(&sdev->fence_lock, flags);
	if (!list_empty(&f->link)) {
		list_del_init(&f->link);
		dma_fence_set_error(&f->base, -ECANCELED);
		dma_fence_signal_locked(&f->base);
		dma_fence_put(&f->base);
	}
	spin_unlock_irqrestore(&sdev->fence_lock, flags);
}

static const struct drm_sched_backend_ops svmd_sched_ops = {
	.run_job = svmd_run_job,
	.timedout_job = svmd_timedout_job,
	.free_job = svmd_free_job,
	.cancel_job = svmd_cancel_job,
};

/*
 * Allocate everything a job needs up front - including its hardware fence,
 * because run_job may not allocate: it runs while memory might be being
 * reclaimed, and reclaim may be waiting on fences (deadlock).
 */
struct svmd_job *svmd_job_alloc(unsigned int num_cmds, unsigned int max_bos)
{
	struct svmd_job *job;

	job = kzalloc(struct_size(job, bos, max_bos), GFP_KERNEL);
	if (!job)
		return NULL;
	job->num_cmds = num_cmds;
	job->cmds = kcalloc(num_cmds, SVMD_CMD_SIZE, GFP_KERNEL);
	job->bo_write = kcalloc(max_bos ?: 1, sizeof(bool), GFP_KERNEL);
	job->hw_fence = kzalloc(sizeof(*job->hw_fence), GFP_KERNEL);
	if (!job->cmds || !job->bo_write || !job->hw_fence) {
		svmd_job_free(job);
		return NULL;
	}
	INIT_LIST_HEAD(&job->hw_fence->link);
	return job;
}

/* Release a job's buffers and memory (not its drm_sched part). */
void svmd_job_free(struct svmd_job *job)
{
	for (unsigned int i = 0; i < job->num_bos; i++)
		drm_gem_object_put(job->bos[i]);
	/*
	 * Initialised (it reached run_job): refcounted, so just drop ours.
	 * Not `base.ops != NULL`: since 7.x a fence clears ops when it
	 * signals, which made that test kfree() live fences (found with a
	 * refcount warning; see README).
	 */
	if (job->hw_fence && dma_fence_was_initialized(&job->hw_fence->base))
		dma_fence_put(&job->hw_fence->base);
	else
		kfree(job->hw_fence);
	svmd_vm_put(job->vm);
	kfree(job->bo_write);
	kfree(job->cmds);
	kfree(job);
}

/*
 * Run the driver's own commands (self-tests; later, migration copies) and
 * wait. `vm`: switch to this address space first (NULL: keep whatever the
 * device has - the self-test uses that to keep the TLB warm on purpose).
 * Returns the job's error: 0, -EFAULT if a fault aborted it, ...
 */
int svmd_kernel_job(struct svmd_device *sdev, struct svmd_vm *vm,
		    u8 (*cmds)[SVMD_CMD_SIZE], unsigned int n)
{
	struct svmd_job *job = svmd_job_alloc(n, 0);
	struct dma_fence *done;
	long left;
	int ret;

	if (!job)
		return -ENOMEM;
	memcpy(job->cmds, cmds, n * SVMD_CMD_SIZE);
	if (vm) {
		svmd_vm_get(vm);
		job->vm = vm;
	}
	ret = drm_sched_job_init(&job->base, &sdev->kernel_entity, n + 2,
				 sdev, 0);
	if (ret) {
		svmd_job_free(job);
		return ret;
	}
	drm_sched_job_arm(&job->base);
	done = dma_fence_get(&job->base.s_fence->finished);
	drm_sched_entity_push_job(&job->base);	/* the scheduler owns it now */

	left = dma_fence_wait_timeout(done, false,
				      msecs_to_jiffies(2 * SVMD_JOB_TIMEOUT_MS));
	ret = left <= 0 ? -ETIMEDOUT : done->error;
	dma_fence_put(done);
	return ret;
}

static void svmd_sched_fini(void *data)
{
	struct svmd_device *sdev = data;

	/* stop the device before anything it uses can be freed */
	svmd_ring_stop(sdev);
	svmd_fence_fail_all(sdev, -ENODEV);
	drm_sched_entity_destroy(&sdev->kernel_entity);
	drm_sched_fini(&sdev->sched);
}

int svmd_sched_init(struct svmd_device *sdev)
{
	struct drm_gpu_scheduler *sched_list[] = { &sdev->sched };
	const struct drm_sched_init_args args = {
		.ops = &svmd_sched_ops,
		.num_rqs = DRM_SCHED_PRIORITY_COUNT,
		/* a job costs (commands + 1 FENCE) ring slots */
		.credit_limit = SVMD_RING_SLOTS - 1,
		.timeout = msecs_to_jiffies(SVMD_JOB_TIMEOUT_MS),
		.name = "svmd",
		.dev = &sdev->pdev->dev,
	};
	int ret;

	ret = drm_sched_init(&sdev->sched, &args);
	if (ret)
		return ret;
	ret = drm_sched_entity_init(&sdev->kernel_entity,
				    DRM_SCHED_PRIORITY_KERNEL, sched_list, 1,
				    NULL);
	if (ret) {
		drm_sched_fini(&sdev->sched);
		return ret;
	}
	return devm_add_action_or_reset(&sdev->pdev->dev, svmd_sched_fini,
					sdev);
}

int svmd_open(struct drm_device *drm, struct drm_file *file)
{
	struct svmd_device *sdev = to_svmd(drm);
	struct drm_gpu_scheduler *sched_list[] = { &sdev->sched };
	struct svmd_file *sf;
	int ret;

	sf = kzalloc(sizeof(*sf), GFP_KERNEL);
	if (!sf)
		return -ENOMEM;
	/* this file's address space mirrors the process opening it */
	sf->vm = svmd_svm_create(sdev, current->mm);
	if (IS_ERR(sf->vm)) {
		ret = PTR_ERR(sf->vm);
		kfree(sf);
		return ret;
	}
	ret = drm_sched_entity_init(&sf->entity, DRM_SCHED_PRIORITY_NORMAL,
				    sched_list, 1, NULL);
	if (ret) {
		svmd_vm_put(sf->vm);
		kfree(sf);
		return ret;
	}
	file->driver_priv = sf;
	return 0;
}

void svmd_postclose(struct drm_device *drm, struct drm_file *file)
{
	struct svmd_file *sf = file->driver_priv;

	/* waits (bounded) for this file's queued jobs, then tears down */
	drm_sched_entity_destroy(&sf->entity);
	/* jobs still in flight hold their own VM references */
	svmd_vm_put(sf->vm);
	kfree(sf);
}
