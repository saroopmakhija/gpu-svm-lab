// SPDX-License-Identifier: GPL-2.0
/*
 * svmd_debugfs.c - looking inside, for tests and for humans.
 *
 *   /sys/kernel/debug/dri/<N>/svmd_stats       device + driver counters
 *   /sys/kernel/debug/dri/<N>/svmd_inject      write spec §9 INJECT bits
 *   /sys/kernel/debug/dri/<N>/svmd_fault_delay_ms   hold every fault
 */

#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/seq_file.h>
#include <drm/drm_debugfs.h>
#include <drm/drm_file.h>

#include "svmd.h"

static int svmd_stats_show(struct seq_file *m, void *data)
{
	struct drm_debugfs_entry *entry = m->private;
	struct svmd_device *sdev = to_svmd(entry->dev);
	void __iomem *r = sdev->regs;

	seq_printf(m, "device_faults     %llu\n", readq(r + SVMD_REG_STAT_FAULTS));
	seq_printf(m, "faults_resolved   %llu\n", sdev->faults_resolved);
	seq_printf(m, "faults_aborted    %llu\n", sdev->faults_aborted);
	seq_printf(m, "faults_duplicate  %llu\n", sdev->faults_duplicate);
	seq_printf(m, "fault_last_err    %d\n", sdev->fault_last_error);
	seq_printf(m, "audit_violations  %llu\n", sdev->audit_violations);
	seq_printf(m, "audit_mismatch    %llu\n", sdev->audit_mismatch);
	seq_printf(m, "audit_first       0x%llx\n", sdev->audit_first);
	seq_printf(m, "audit_seq_changed %llu\n", sdev->audit_seq_changed);
	seq_printf(m, "audit_pte_is_pages %llu\n", sdev->audit_pte_matches_pages);
	seq_printf(m, "audit_first_invals %u\n", sdev->audit_first_invals);
	seq_printf(m, "audit_range       0x%llx-0x%llx\n", sdev->audit_range_start,
		   sdev->audit_range_end);
	seq_printf(m, "tlb_hits          %llu\n", readq(r + SVMD_REG_STAT_TLB_HITS));
	seq_printf(m, "tlb_misses        %llu\n", readq(r + SVMD_REG_STAT_TLB_MISSES));
	seq_printf(m, "invalidations     %llu\n", readq(r + SVMD_REG_STAT_INVALS));
	seq_printf(m, "bytes             %llu\n", readq(r + SVMD_REG_STAT_BYTES));
	seq_printf(m, "migrations        %llu\n", sdev->migrations);
	seq_printf(m, "migrations_failed %llu\n", sdev->migrations_failed);
	seq_printf(m, "migrate_last_err  %d\n", sdev->migrate_last_error);
	seq_printf(m, "pages_to_vram     %llu\n", sdev->pages_to_vram);
	seq_printf(m, "pages_to_ram      %llu\n", sdev->pages_to_ram);
	seq_printf(m, "status            0x%x\n", readl(r + SVMD_REG_STATUS));
	return 0;
}

/* the flight recorder, oldest first, only events touching audit_first */
static int svmd_inval_log_show(struct seq_file *m, void *data)
{
	struct drm_debugfs_entry *entry = m->private;
	struct svmd_device *sdev = to_svmd(entry->dev);
	u64 va = sdev->audit_first & ~0xfffull;

	seq_printf(m, "events touching 0x%llx (frozen: %d)\n", va,
		   sdev->inval_log_frozen);
	for (u32 i = 0; i < SVMD_INVAL_LOG; i++) {
		struct svmd_inval_log *l = &sdev->inval_log[(sdev->inval_log_next + i) %
							   SVMD_INVAL_LOG];
		if (!l->end || (va && (va < l->start || va >= l->end)))
			continue;
		seq_printf(m, "#%u %c 0x%llx-0x%llx 0x%llx\n", l->seq, l->type,
			   l->start, l->end, l->info);
	}
	return 0;
}

static int svmd_inject_set(void *data, u64 val)
{
	struct svmd_device *sdev = data;

	writel(val, sdev->regs + SVMD_REG_INJECT);
	return 0;
}

static int svmd_inject_get(void *data, u64 *val)
{
	struct svmd_device *sdev = data;

	*val = readl(sdev->regs + SVMD_REG_INJECT);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(svmd_inject_fops, svmd_inject_get, svmd_inject_set,
			 "0x%llx\n");

/* Call after drm_dev_register(), which creates the debugfs directory. */
void svmd_debugfs_init(struct svmd_device *sdev)
{
	struct dentry *root = sdev->drm.debugfs_root;

	if (IS_ERR_OR_NULL(root))
		return;
	debugfs_create_file_unsafe("svmd_inject", 0644, root, sdev,
				   &svmd_inject_fops);
	debugfs_create_u32("svmd_fault_delay_ms", 0644, root,
			   &sdev->fault_delay_ms);
	debugfs_create_u32("svmd_audit", 0644, root, &sdev->audit);
	debugfs_create_u32("svmd_job_irq_delay_ms", 0644, root,
			   &sdev->job_irq_delay_ms);
}

/* Call before drm_dev_register(): DRM creates these files for us. */
void svmd_debugfs_early(struct svmd_device *sdev)
{
	drm_debugfs_add_file(&sdev->drm, "svmd_stats", svmd_stats_show, NULL);
	drm_debugfs_add_file(&sdev->drm, "svmd_inval_log", svmd_inval_log_show, NULL);
}
