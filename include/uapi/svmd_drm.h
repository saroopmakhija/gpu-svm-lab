/* SPDX-License-Identifier: (GPL-2.0 WITH Linux-syscall-note) OR MIT */
/*
 * svmd_drm.h - what user programs can ask the svmd driver to do.
 *
 * This is "uAPI": once real programs use it, it can never change in an
 * incompatible way. So every struct has explicit sizes, explicit padding,
 * and 64-bit fields for pointers (a 32-bit program must work too).
 *
 * Programs never see device addresses. They name memory as (buffer
 * handle, offset), and the driver checks the bounds and translates.
 *
 * Waiting: pass a DRM syncobj as `out_sync`, then use the core
 * DRM_IOCTL_SYNCOBJ_WAIT - the standard way to wait for GPU work.
 * `in_sync` makes a job wait for any fence first (explicit sync).
 * Ordering between jobs that share a buffer is automatic ("implicit
 * sync"), even across different open files.
 *
 * Commands may also use plain CPU pointers (handle 0): shared virtual
 * memory. The device faults them in from the process's own memory.
 *
 * v0.3: replaced v0.2's DRM_SVMD_WAIT (seqnos) with syncobjs; in_sync.
 * v0.4: pointer commands (SVM). v0.5: SET_PARAM (prefer VRAM).
 */
#ifndef SVMD_DRM_H
#define SVMD_DRM_H

#include <drm/drm.h>

#define DRM_SVMD_GEM_CREATE	0x00
#define DRM_SVMD_SUBMIT		0x01
#define DRM_SVMD_SET_PARAM	0x02

/* Allocate a buffer the device can use. mmap() it at `mmap_offset`. */
struct drm_svmd_gem_create {
	__u64 size;		/* in: bytes (rounded up to a page) */
	__u32 handle;		/* out */
	__u32 pad;
	__u64 mmap_offset;	/* out: pass to mmap() on the DRM fd */
};

/* One command. Only FILL, COPY and ADD are allowed from user space. */
struct drm_svmd_cmd {
	__u32 op;		/* SVMD_OP_FILL / _COPY / _ADD */
	__u32 arg32;		/* FILL: pattern, ADD: addend */
	__u32 dst_handle;
	__u32 src_handle;	/* COPY only, else 0 */
	__u64 dst_offset;
	__u64 src_offset;
	__u64 len;		/* bytes */
};

#define DRM_SVMD_MAX_CMDS	64

/* Queue a job: commands that run in order. */
struct drm_svmd_submit {
	__u64 cmds;		/* in: user pointer to drm_svmd_cmd[count] */
	__u32 count;		/* in: 1..DRM_SVMD_MAX_CMDS */
	__u32 in_sync;		/* in: syncobj to wait for before starting, or 0 */
	__u32 out_sync;		/* in: syncobj to signal when done, or 0 */
	__u32 pad;
};

/* Per open file settings. */
#define DRM_SVMD_PARAM_PREFER_VRAM	1	/* value 1: migrate memory the
						 * device faults on into VRAM */
struct drm_svmd_set_param {
	__u32 param;
	__u32 pad;
	__u64 value;
};

#define DRM_IOCTL_SVMD_GEM_CREATE \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_SVMD_GEM_CREATE, struct drm_svmd_gem_create)
#define DRM_IOCTL_SVMD_SUBMIT \
	DRM_IOW(DRM_COMMAND_BASE + DRM_SVMD_SUBMIT, struct drm_svmd_submit)
#define DRM_IOCTL_SVMD_SET_PARAM \
	DRM_IOW(DRM_COMMAND_BASE + DRM_SVMD_SET_PARAM, struct drm_svmd_set_param)

#endif /* SVMD_DRM_H */
