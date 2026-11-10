// SPDX-License-Identifier: GPL-2.0
/*
 * svmd_copy.c - the copy engine (spec §10): moving pages for migration.
 *
 * Synchronous and simple on purpose: write PHYS commands into the copy
 * ring, ring its doorbell, poll CFENCE_VALUE. It never faults, so it works
 * even while the main engine is stalled on the very fault that asked for
 * the migration - the reason it exists.
 */

#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/pci.h>

#include "svmd.h"

#define CRING_SLOTS	64
#define CRING_MASK	(CRING_SLOTS - 1)

int svmd_copy_init(struct svmd_device *sdev)
{
	mutex_init(&sdev->copy_lock);
	sdev->cring = dmam_alloc_coherent(&sdev->pdev->dev,
					  CRING_SLOTS * SVMD_CMD_SIZE,
					  &sdev->cring_dma, GFP_KERNEL);
	if (!sdev->cring)
		return -ENOMEM;
	writeq(sdev->cring_dma, sdev->regs + SVMD_REG_CRING_BASE);
	writel(CRING_SLOTS, sdev->regs + SVMD_REG_CRING_SIZE);
	sdev->ctail = 0;
	return 0;
}

/* Run `n` prepared commands (PHYS) and wait for all of them. */
int svmd_copy_run(struct svmd_device *sdev, u8 (*cmds)[SVMD_CMD_SIZE],
		  unsigned int n)
{
	void __iomem *r = sdev->regs;
	unsigned int i = 0;
	int ret = 0;

	mutex_lock(&sdev->copy_lock);
	while (i < n && !ret) {
		/* a batch that fits, plus its FENCE (the ring is empty here) */
		unsigned int batch = min(n - i, CRING_SLOTS - 2u);
		u32 seqno = ++sdev->cseqno, done;

		for (unsigned int k = 0; k < batch; k++, i++) {
			memcpy(sdev->cring + sdev->ctail * SVMD_CMD_SIZE, cmds[i],
			       SVMD_CMD_SIZE);
			sdev->ctail = (sdev->ctail + 1) & CRING_MASK;
		}
		svmd_encode_cmd(sdev->cring + sdev->ctail * SVMD_CMD_SIZE,
				SVMD_OP_FENCE, 0, seqno, 0, 0, 0);
		sdev->ctail = (sdev->ctail + 1) & CRING_MASK;
		writel(sdev->ctail, r + SVMD_REG_CRING_TAIL);	/* doorbell */

		ret = readl_poll_timeout(r + SVMD_REG_CFENCE_VALUE, done,
					 svmd_seqno_passed(done, seqno) ||
					 readl(r + SVMD_REG_CERROR),
					 10, 2000000);
		if (!ret && readl(r + SVMD_REG_CERROR))
			ret = -EIO;
	}
	mutex_unlock(&sdev->copy_lock);
	if (ret)
		dev_err(&sdev->pdev->dev, "copy engine failed (%d, error %u)\n",
			ret, readl(r + SVMD_REG_CERROR));
	return ret;
}
