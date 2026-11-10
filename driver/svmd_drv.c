// SPDX-License-Identifier: GPL-2.0
/*
 * svmd - Linux driver for the SVMD toy GPU (docs/hardware-spec.md).
 *
 * Phase 1: find the device, talk to its registers, receive its interrupts.
 * Phase 2: a DRM driver - buffers (GEM), and a submit ioctl that runs
 *          FILL / COPY / ADD on them through the command ring.
 * Phase 2b: jobs go through drm_sched; completion is a dma_fence that user
 *          space waits on with a syncobj; jobs sharing a buffer are ordered
 *          automatically (implicit sync through the buffer's dma_resv).
 * Phase 3-5: each open file gets a device address space mirroring its
 *          process (svmd_svm.c): commands may use plain CPU pointers, and
 *          the device faults them in (svmd_fault.c).
 */

#include <linux/dma-mapping.h>
#include <linux/dma-resv.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_syncobj.h>
#include <drm/drm_vma_manager.h>

#include "svmd.h"
#include "uapi/svmd_drm.h"

#define CREATE_TRACE_POINTS
#include "svmd_trace.h"

static u32 svmd_read(struct svmd_device *sdev, u32 reg)
{
	return readl(sdev->regs + reg);
}

static void svmd_write(struct svmd_device *sdev, u32 reg, u32 val)
{
	/* writel, not writel_relaxed: orders earlier memory writes first */
	writel(val, sdev->regs + reg);
}

/*--- interrupts ----------------------------------------------------------*/

/* Per-vector context, so each handler knows which vector it is. */
struct svmd_irq {
	struct svmd_device *sdev;
	unsigned int vec;
};

/*
 * Runs in interrupt context: may not sleep. Acknowledge our bit by writing
 * 1 to it (IRQ_STATUS is write-1-to-clear) - forgetting this would make
 * the device never interrupt for this vector again (see svmd_raise in the
 * QEMU device).
 */
static irqreturn_t svmd_irq_handler(int irq, void *data)
{
	struct svmd_irq *si = data;
	struct svmd_device *sdev = si->sdev;
	u32 bit = BIT(si->vec);

	if (!(svmd_read(sdev, SVMD_REG_IRQ_STATUS) & bit))
		return IRQ_NONE;	/* not ours (or a spurious ding) */
	svmd_write(sdev, SVMD_REG_IRQ_STATUS, bit);

	switch (si->vec) {
	case SVMD_VEC_JOB:
		/* tests: retire finished jobs late, to open the window in
		 * which "oldest listed fence" != "job the device is running" */
		if (READ_ONCE(sdev->job_irq_delay_ms))
			mod_delayed_work(system_dfl_wq, &sdev->fence_dwork,
					 msecs_to_jiffies(sdev->job_irq_delay_ms));
		else
			svmd_fence_process(sdev);
		break;
	case SVMD_VEC_FAULT:
		svmd_fault_irq(sdev);
		break;
	case SVMD_VEC_ERROR:
		/* only a real engine stop, not the self-test's fake one */
		if (svmd_read(sdev, SVMD_REG_STATUS) & SVMD_STATUS_ERROR) {
			dev_err(&sdev->pdev->dev, "engine stopped, error code %u\n",
				svmd_read(sdev, SVMD_REG_ERROR_CODE));
			svmd_engine_wedge(sdev, -EIO);
		}
		break;
	}
	complete(&sdev->irq_seen[si->vec]);
	return IRQ_HANDLED;
}

static int svmd_setup_irqs(struct svmd_device *sdev)
{
	struct pci_dev *pdev = sdev->pdev;
	int n, v, ret;

	/* ask the PCI core for exactly our 3 MSI-X vectors */
	n = pci_alloc_irq_vectors(pdev, SVMD_NUM_VECTORS, SVMD_NUM_VECTORS,
				  PCI_IRQ_MSIX);
	if (n < 0)
		return n;

	for (v = 0; v < SVMD_NUM_VECTORS; v++) {
		struct svmd_irq *si = devm_kzalloc(&pdev->dev, sizeof(*si),
						   GFP_KERNEL);
		if (!si)
			return -ENOMEM;
		si->sdev = sdev;
		si->vec = v;
		init_completion(&sdev->irq_seen[v]);

		ret = devm_request_irq(&pdev->dev, pci_irq_vector(pdev, v),
				       svmd_irq_handler, 0, "svmd", si);
		if (ret)
			return ret;
	}
	svmd_write(sdev, SVMD_REG_IRQ_MASK, SVMD_IRQ_ALL);
	return 0;
}

/*--- ioctls: what user programs can do (include/uapi/svmd_drm.h) ---------*/

static int svmd_ioctl_gem_create(struct drm_device *drm, void *data,
				 struct drm_file *file)
{
	struct drm_svmd_gem_create *args = data;
	struct drm_gem_dma_object *obj;
	int ret;

	if (!args->size || args->pad)
		return -EINVAL;

	obj = drm_gem_dma_create(drm, args->size);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	ret = drm_gem_create_mmap_offset(&obj->base);
	if (!ret)
		ret = drm_gem_handle_create(file, &obj->base, &args->handle);
	if (!ret)
		args->mmap_offset = drm_vma_node_offset_addr(&obj->base.vma_node);
	/* the handle now holds the buffer; drop our own reference */
	drm_gem_object_put(&obj->base);
	return ret;
}

/*
 * Turn (handle, offset, len) into a device address - after checking it.
 * The buffer reference is kept in the job (so it stays alive until the
 * device is done), even on failure, so one cleanup path releases it.
 * A buffer used twice in one job is stored once: the reservation locking
 * below must not lock the same buffer twice.
 */
static int svmd_resolve(struct drm_file *file, struct svmd_job *job,
			u32 handle, u64 offset, u64 len, bool write,
			dma_addr_t *addr)
{
	struct drm_gem_object *obj = drm_gem_object_lookup(file, handle);
	unsigned int i;

	if (!obj)
		return -ENOENT;
	for (i = 0; i < job->num_bos && job->bos[i] != obj; i++)
		;
	if (i < job->num_bos)
		drm_gem_object_put(obj);	/* already held */
	else
		job->bos[job->num_bos++] = obj;
	job->bo_write[i] |= write;

	/* written so a huge len can't wrap around and pass */
	if (offset > obj->size || len > obj->size - offset)
		return -EINVAL;
	*addr = to_drm_gem_dma_obj(obj)->dma_addr + offset;
	return 0;
}

/*
 * Check every user command and encode it into the job. Each command names
 * memory one of two ways:
 *   handles  (buffer handle, offset): the driver checks the range and
 *            gives the device a physical address (PHYS: no MMU)
 *   pointers (handle 0, offset = a CPU pointer): passed through as a
 *            device-virtual address. The MMU is the check: anything the
 *            process can't access faults, and the job fails.
 * One command can't mix the two (PHYS is per command).
 */
static int svmd_job_fill(struct drm_file *file, struct svmd_job *job,
			 const struct drm_svmd_cmd *ucmds)
{
	for (unsigned int i = 0; i < job->num_cmds; i++) {
		const struct drm_svmd_cmd *c = &ucmds[i];
		bool copy = c->op == SVMD_OP_COPY;
		bool by_ptr = !c->dst_handle;
		dma_addr_t dst = c->dst_offset, src = c->src_offset;
		int ret;

		/* user space may not FENCE, SET_ROOT (we do that) or set PHYS */
		switch (c->op) {
		case SVMD_OP_FILL:
		case SVMD_OP_ADD:
			if (c->len % 4 || c->src_handle || c->src_offset)
				return -EINVAL;
			break;
		case SVMD_OP_COPY:
			if (by_ptr != !c->src_handle)
				return -EINVAL;		/* mixed */
			break;
		default:
			return -EINVAL;
		}
		if (!c->len)
			return -EINVAL;

		if (by_ptr) {
			/* stay inside the device's 48-bit address space */
			if (dst >> SVMD_VA_BITS || c->len > (1ull << SVMD_VA_BITS) - dst ||
			    (copy && (src >> SVMD_VA_BITS ||
				      c->len > (1ull << SVMD_VA_BITS) - src)))
				return -EINVAL;
			svmd_encode_cmd(job->cmds[i], c->op, 0, c->arg32, dst, src,
					c->len);
			continue;
		}

		ret = svmd_resolve(file, job, c->dst_handle, c->dst_offset,
				   c->len, true, &dst);
		if (ret)
			return ret;
		if (copy) {
			ret = svmd_resolve(file, job, c->src_handle,
					   c->src_offset, c->len, false, &src);
			if (ret)
				return ret;
			/* §4: COPY ranges must not overlap */
			if (src < dst + c->len && dst < src + c->len)
				return -EINVAL;
		}
		svmd_encode_cmd(job->cmds[i], c->op, SVMD_CMD_PHYS, c->arg32,
				dst, src, c->len);
	}
	return 0;
}

/*
 * Dependencies, then implicit sync while holding every buffer's
 * reservation lock:
 *  1. this job waits for earlier jobs (from anyone) that write its
 *     buffers - and, for buffers it writes, for earlier readers too;
 *  2. this job's "finished" fence is published on each buffer, so later
 *     jobs wait for it in turn.
 * Step 2 needs the fence, which only exists after drm_sched_job_arm(), and
 * arming can't be undone - so everything that can fail happens before it.
 */
static int svmd_job_push(struct drm_file *file, struct svmd_job *job,
			 u32 in_sync, u32 out_sync)
{
	struct drm_syncobj *syncobj = NULL;
	struct ww_acquire_ctx ctx;
	struct dma_fence *done;
	unsigned int i;
	int ret;

	/* explicit sync: whatever fence the program handed us */
	if (in_sync) {
		ret = drm_sched_job_add_syncobj_dependency(&job->base, file,
							   in_sync, 0);
		if (ret)
			return ret;
	}
	if (out_sync) {
		syncobj = drm_syncobj_find(file, out_sync);
		if (!syncobj)
			return -ENOENT;
	}

	ret = drm_gem_lock_reservations(job->bos, job->num_bos, &ctx);
	if (ret)
		goto out_syncobj;
	for (i = 0; i < job->num_bos; i++) {
		ret = drm_sched_job_add_implicit_dependencies(&job->base,
					job->bos[i], job->bo_write[i]);
		if (!ret)
			ret = dma_resv_reserve_fences(job->bos[i]->resv, 1);
		if (ret)
			goto out_unlock;
	}

	/* --- point of no return --- */
	drm_sched_job_arm(&job->base);
	done = &job->base.s_fence->finished;
	for (i = 0; i < job->num_bos; i++)
		dma_resv_add_fence(job->bos[i]->resv, done,
				   job->bo_write[i] ? DMA_RESV_USAGE_WRITE :
						      DMA_RESV_USAGE_READ);
	if (syncobj)
		drm_syncobj_replace_fence(syncobj, done);
	drm_sched_entity_push_job(&job->base);	/* the scheduler owns it now */

out_unlock:
	drm_gem_unlock_reservations(job->bos, job->num_bos, &ctx);
out_syncobj:
	if (syncobj)
		drm_syncobj_put(syncobj);
	return ret;
}

static int svmd_ioctl_submit(struct drm_device *drm, void *data,
			     struct drm_file *file)
{
	struct svmd_file *sf = file->driver_priv;
	struct drm_svmd_submit *args = data;
	struct drm_svmd_cmd *ucmds;
	struct svmd_job *job;
	int ret;

	if (!args->count || args->count > DRM_SVMD_MAX_CMDS || args->pad)
		return -EINVAL;

	ucmds = memdup_array_user(u64_to_user_ptr(args->cmds), args->count,
				  sizeof(*ucmds));
	if (IS_ERR(ucmds))
		return PTR_ERR(ucmds);

	job = svmd_job_alloc(args->count, 2 * args->count);
	if (!job) {
		ret = -ENOMEM;
		goto out;
	}
	/* it runs in this file's address space */
	svmd_vm_get(sf->vm);
	job->vm = sf->vm;
	/* ring slots ("credits"): SET_ROOT + commands + FENCE */
	ret = drm_sched_job_init(&job->base, &sf->entity, args->count + 2,
				 sf, file->client_id);
	if (ret) {
		svmd_job_free(job);
		goto out;
	}

	ret = svmd_job_fill(file, job, ucmds);
	if (!ret)
		ret = svmd_job_push(file, job, args->in_sync, args->out_sync);
	if (ret) {
		/* never armed: undo drm_sched_job_init, then free */
		drm_sched_job_cleanup(&job->base);
		svmd_job_free(job);
	}
out:
	kfree(ucmds);
	return ret;
}

static int svmd_ioctl_set_param(struct drm_device *drm, void *data,
				struct drm_file *file)
{
	struct svmd_file *sf = file->driver_priv;
	struct drm_svmd_set_param *args = data;

	if (args->pad)
		return -EINVAL;
	switch (args->param) {
	case DRM_SVMD_PARAM_PREFER_VRAM:
		if (!to_svmd(drm)->dpagemap)
			return -ENODEV;
		WRITE_ONCE(sf->vm->prefer_vram, !!args->value);
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct drm_ioctl_desc svmd_ioctls[] = {
	DRM_IOCTL_DEF_DRV(SVMD_GEM_CREATE, svmd_ioctl_gem_create, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(SVMD_SUBMIT, svmd_ioctl_submit, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(SVMD_SET_PARAM, svmd_ioctl_set_param, DRM_RENDER_ALLOW),
};

DEFINE_DRM_GEM_DMA_FOPS(svmd_fops);

static const struct drm_driver svmd_drm_driver = {
	/* GEM buffers, syncobjs, and a render node (/dev/dri/renderD*) */
	.driver_features = DRIVER_GEM | DRIVER_RENDER | DRIVER_SYNCOBJ,
	DRM_GEM_DMA_DRIVER_OPS,
	.open = svmd_open,
	.postclose = svmd_postclose,
	.ioctls = svmd_ioctls,
	.num_ioctls = ARRAY_SIZE(svmd_ioctls),
	.fops = &svmd_fops,
	.name = "svmd",
	.desc = "SVMD shared-virtual-memory toy GPU",
	.major = 0,
	.minor = 5,
};

/*--- PCI glue ------------------------------------------------------------*/

static void svmd_ring_stop_action(void *data)
{
	svmd_ring_stop(data);
}

static void svmd_fault_fini_action(void *data)
{
	svmd_fault_fini(data);
}

/* Called by the PCI core when it finds a device matching svmd_ids. */
static int svmd_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct svmd_device *sdev;
	u32 magic;
	int ret;

	sdev = devm_drm_dev_alloc(&pdev->dev, &svmd_drm_driver,
				  struct svmd_device, drm);
	if (IS_ERR(sdev))
		return PTR_ERR(sdev);
	sdev->pdev = pdev;
	pci_set_drvdata(pdev, sdev);

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;
	/*
	 * Let the device write to memory on its own ("bus mastering").
	 * MSI-X interrupts ARE memory writes, so without this every
	 * interrupt is silently dropped. (Phase 1's first real bug.)
	 */
	pci_set_master(pdev);
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;

	sdev->regs = pcim_iomap_region(pdev, SVMD_BAR_REGS, "svmd");
	if (IS_ERR(sdev->regs))
		return PTR_ERR(sdev->regs);

	magic = svmd_read(sdev, SVMD_REG_ID);
	if (magic != SVMD_ID_MAGIC) {
		dev_err(&pdev->dev, "bad ID 0x%08x, not an SVMD\n", magic);
		return -ENODEV;
	}
	dev_info(&pdev->dev, "SVMD version %u, caps 0x%x\n",
		 svmd_read(sdev, SVMD_REG_VERSION),
		 svmd_read(sdev, SVMD_REG_CAPS));

	/*
	 * Ring and scheduler before interrupts: the handlers use them.
	 * Each comes with a devm "undo" action registered right after it;
	 * devm undoes in reverse order, so on unbind (or a later probe
	 * failure) the device is stopped before the memory it uses is freed.
	 */
	ret = svmd_ring_init(sdev);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(&pdev->dev, svmd_ring_stop_action, sdev);
	if (ret)
		return ret;
	ret = svmd_copy_init(sdev);
	if (ret)
		return ret;
	ret = svmd_mmu_init(sdev);
	if (ret)
		return ret;
	ret = svmd_fault_init(sdev);
	if (!ret)
		ret = devm_add_action_or_reset(&pdev->dev, svmd_fault_fini_action,
					       sdev);
	if (ret)
		return ret;
	ret = svmd_sched_init(sdev);
	if (ret)
		return ret;
	ret = svmd_vram_init(sdev);
	if (ret)
		return ret;

	ret = svmd_setup_irqs(sdev);
	if (ret)
		return ret;

	ret = svmd_selftests(sdev);
	if (ret)
		return ret;

	/* Only now may user programs open /dev/dri/renderD*. */
	svmd_debugfs_early(sdev);
	ret = drm_dev_register(&sdev->drm, 0);
	if (ret)
		return ret;
	svmd_debugfs_init(sdev);
	return 0;
}

static void svmd_remove(struct pci_dev *pdev)
{
	struct svmd_device *sdev = pci_get_drvdata(pdev);

	/* no new ioctls; open files get -ENODEV from now on */
	drm_dev_unplug(&sdev->drm);
	/* stop interrupts; devm then frees the handlers, stops the device and
	 * fails what's pending (svmd_sched_fini), and frees its memory */
	svmd_write(sdev, SVMD_REG_IRQ_MASK, 0);
}

static const struct pci_device_id svmd_ids[] = {
	{ PCI_DEVICE(SVMD_PCI_VENDOR_ID, SVMD_PCI_DEVICE_ID) },
	{ }
};
MODULE_DEVICE_TABLE(pci, svmd_ids);

static struct pci_driver svmd_pci_driver = {
	.name = "svmd",
	.id_table = svmd_ids,
	.probe = svmd_probe,
	.remove = svmd_remove,
};
module_pci_driver(svmd_pci_driver);

MODULE_DESCRIPTION("SVMD shared-virtual-memory toy GPU");
MODULE_LICENSE("GPL");
