/* SPDX-License-Identifier: GPL-2.0 */
/* svmd.h - the driver's own data structures, shared by its .c files. */
#ifndef SVMD_H
#define SVMD_H

#include <linux/completion.h>
#include <linux/dma-fence.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>
#include <drm/drm_device.h>
#include <drm/drm_gpusvm.h>
#include <drm/gpu_scheduler.h>

#include "svmd_regs.h"

#define SVMD_RING_SLOTS		256	/* power of 2 (§3) */
#define SVMD_FAULTQ_RECS	64	/* power of 2 (§6) */
#define SVMD_JOB_TIMEOUT_MS	5000

struct svmd_vm;

/* debug flight recorder: page-table events, frozen at the first audit
 * mismatch (debugfs svmd_inval_log) */
struct svmd_inval_log {
	u64 start, end, info;
	char type;		/* Z zap(info=event) B bind(info=first page) G gc */
	u32 seq;
};
#define SVMD_INVAL_LOG	8192

struct svmd_device {
	struct drm_device drm;		/* DRM embeds us in its device */
	struct pci_dev *pdev;
	void __iomem *regs;		/* BAR0 */

	/* phase 1 self-test: the handler signals this for each vector */
	struct completion irq_seen[SVMD_NUM_VECTORS];

	/*--- the ring (svmd_ring.c). Only run_job writes it, and the
	 * scheduler calls run_job from one ordered workqueue, so the
	 * fields below need no lock of their own. */
	void *ring;			/* CPU view of the slots */
	dma_addr_t ring_dma;		/* the device's view */
	u32 tail;			/* our copy of RING_TAIL */
	u32 last_seqno;			/* newest seqno handed to the device */

	/*--- hardware fences: one per job on the ring */
	u32 *fence_cpu;			/* the device writes done seqnos here */
	dma_addr_t fence_dma;
	u64 fence_context;		/* our timeline's id, for dma_fence */
	spinlock_t fence_lock;		/* the fences' lock; guards the list */
	struct list_head pending_fences;
	bool engine_dead;		/* error or hang: no more work runs */

	/*--- scheduling (svmd_sched.c) */
	struct drm_gpu_scheduler sched;
	struct drm_sched_entity kernel_entity;	/* the driver's own jobs */

	/*--- MMU (svmd_mmu.c) */
	struct mutex inval_lock;	/* one TLB invalidation at a time */
	u32 inval_seqno;

	/*--- faults (svmd_fault.c) */
	void *faultq;			/* SVMD_FAULTQ_RECS records */
	dma_addr_t faultq_dma;
	u32 faultq_head;
	struct workqueue_struct *fault_wq;
	struct work_struct fault_work;
	struct xarray vms;		/* page-table root (pfn) -> svmd_vm */
	u32 fault_delay_ms;		/* debugfs: hold each fault (tests) */
	u32 job_irq_delay_ms;		/* debugfs: retire finished jobs late */
	struct delayed_work fence_dwork;
	u64 faults_resolved, faults_aborted, faults_duplicate;
	int fault_last_error;
	u32 audit;			/* debugfs: check entries after faults */
	u64 audit_violations, audit_mismatch, audit_first, audit_seq_changed;
	u64 audit_range_start, audit_range_end, audit_pte_matches_pages;
	u32 audit_first_invals;
	spinlock_t inval_log_lock;
	struct svmd_inval_log inval_log[SVMD_INVAL_LOG];
	u32 inval_log_next;
	bool inval_log_frozen;

	/*--- the copy engine (svmd_copy.c) */
	struct mutex copy_lock;
	void *cring;
	dma_addr_t cring_dma;
	u32 ctail, cseqno;

	/*--- VRAM (svmd_vram.c); NULL if the device has none */
	struct drm_pagemap *dpagemap;
	u64 pages_to_vram, pages_to_ram;
	u64 migrations, migrations_failed;
	int migrate_last_error;
};

static inline struct svmd_device *to_svmd(struct drm_device *drm)
{
	return container_of(drm, struct svmd_device, drm);
}

/*--- page tables (svmd_mmu.c) ---------------------------------------------*/

/* One 4 KiB page of page-table entries, and (above the leaves) its children. */
struct svmd_pt {
	u64 *cpu;
	dma_addr_t dma;
	struct svmd_pt **child;		/* NULL for leaf tables (level 3) */
};

/*
 * A device address space. Every open file gets one that mirrors its
 * process's memory through drm_gpusvm (phase 5); the self-test builds one
 * by hand (phase 3-4).
 */
struct svmd_vm {
	struct svmd_device *sdev;
	struct kref ref;
	struct mutex lock;		/* faults + garbage collection; the
					 * "driver lock" drm_gpusvm checks */
	struct svmd_pt *root;

	/* phase 5: mirroring a process (NULL mm for the self-test VM) */
	bool has_svm;
	struct drm_gpusvm svm;
	spinlock_t gc_lock;
	struct list_head gc_list;	/* ranges unmapped by the CPU */

	bool prefer_vram;		/* migrate faulted ranges to VRAM */

	/* how to fix a fault in this VM: 0 = mapped, retry; <0 = abort */
	int (*resolve)(struct svmd_vm *vm, u64 va, bool write);
	void *priv;			/* for `resolve` */
};

/* One drm_gpusvm range and the pages it maps. */
struct svmd_range {
	struct drm_gpusvm_range base;
	struct drm_gpusvm_pages pages;
	struct list_head gc_link;
	u32 invals;			/* debug: invalidations applied to it */
};


int svmd_mmu_init(struct svmd_device *sdev);
struct svmd_vm *svmd_vm_alloc(struct svmd_device *sdev);
void svmd_vm_get(struct svmd_vm *vm);
void svmd_vm_put(struct svmd_vm *vm);
int svmd_pt_prepare(struct svmd_vm *vm, u64 start, u64 end);
void svmd_pt_set(struct svmd_vm *vm, u64 va, u64 pte);
void svmd_pt_zap(struct svmd_vm *vm, u64 start, u64 end);
int svmd_tlb_invalidate(struct svmd_device *sdev, u64 start, u64 size);

/*--- SVM (svmd_svm.c) ------------------------------------------------------*/

struct svmd_vm *svmd_svm_create(struct svmd_device *sdev, struct mm_struct *mm);
void svmd_svm_teardown(struct svmd_vm *vm);
void svmd_svm_audit(struct svmd_vm *vm);
void svmd_log(struct svmd_device *sdev, char type, u64 start, u64 end, u64 info);

/*--- copy engine and VRAM (svmd_copy.c, svmd_vram.c) -----------------------*/

int svmd_copy_init(struct svmd_device *sdev);
int svmd_copy_run(struct svmd_device *sdev, u8 (*cmds)[SVMD_CMD_SIZE],
		  unsigned int n);
int svmd_vram_init(struct svmd_device *sdev);

/*--- faults (svmd_fault.c) -------------------------------------------------*/

int svmd_fault_init(struct svmd_device *sdev);
void svmd_fault_irq(struct svmd_device *sdev);
void svmd_fault_fini(struct svmd_device *sdev);

/*--- jobs (svmd_ring.c, svmd_sched.c) --------------------------------------*/

/* What the device signals when it reaches a job's FENCE command. */
struct svmd_fence {
	struct dma_fence base;		/* first: dma_fence frees from here */
	struct list_head link;		/* on pending_fences until signaled */
};

/* One job: its commands, the buffers it uses, and its hardware fence. */
struct svmd_job {
	struct drm_sched_job base;
	struct svmd_fence *hw_fence;	/* allocated at submit, armed in run_job */
	struct svmd_vm *vm;		/* SET_ROOT to this first (NULL: don't) */
	unsigned int num_cmds;
	u8 (*cmds)[SVMD_CMD_SIZE];	/* already encoded (§4) */
	unsigned int num_bos;
	bool *bo_write;			/* does the job write bos[i]? */
	struct drm_gem_object *bos[];	/* each holds a reference */
};

static inline struct svmd_job *to_svmd_job(struct drm_sched_job *j)
{
	return container_of(j, struct svmd_job, base);
}

/* Per open file: its own queue, and its own address space. */
struct svmd_file {
	struct drm_sched_entity entity;
	struct svmd_vm *vm;
};

/* svmd_ring.c */
int svmd_ring_init(struct svmd_device *sdev);
void svmd_ring_stop(struct svmd_device *sdev);
void svmd_ring_emit(struct svmd_device *sdev, struct svmd_job *job);
void svmd_fence_process(struct svmd_device *sdev);
void svmd_fence_fail_all(struct svmd_device *sdev, int error);
void svmd_fence_fail_current(struct svmd_device *sdev, int error);
void svmd_encode_cmd(u8 *slot, u8 op, u8 flags, u32 arg32,
		     u64 dst, u64 src, u64 len);

/* svmd_sched.c */
int svmd_sched_init(struct svmd_device *sdev);
struct svmd_job *svmd_job_alloc(unsigned int num_cmds, unsigned int max_bos);
void svmd_job_free(struct svmd_job *job);
void svmd_engine_wedge(struct svmd_device *sdev, int error);
int svmd_kernel_job(struct svmd_device *sdev, struct svmd_vm *vm,
		    u8 (*cmds)[SVMD_CMD_SIZE], unsigned int n);
int svmd_open(struct drm_device *drm, struct drm_file *file);
void svmd_postclose(struct drm_device *drm, struct drm_file *file);

/* svmd_selftest.c */
int svmd_selftests(struct svmd_device *sdev);

/* svmd_debugfs.c */
void svmd_debugfs_early(struct svmd_device *sdev);
void svmd_debugfs_init(struct svmd_device *sdev);

/*
 * Has `seqno` completed, given the newest completed one? Done in signed
 * 32-bit arithmetic so it keeps working after the counter wraps past 2^32.
 */
static inline bool svmd_seqno_passed(u32 completed, u32 seqno)
{
	return (s32)(completed - seqno) >= 0;
}

#endif /* SVMD_H */
