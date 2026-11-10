// SPDX-License-Identifier: GPL-2.0
/*
 * svmd_selftest.c - prove the hardware/driver contract at every probe,
 * before any user program can reach the device.
 *
 *   irq     registers answer, all 3 interrupts arrive        (phase 1)
 *   engine  FILL, COPY, ADD really happen, in order          (phase 2)
 *   tlb     the TLB is NOT coherent: a changed entry is ignored until
 *           invalidated - the reason invalidation exists      (phase 3)
 *   fault   a missing page is fixed and the job finishes;
 *           an unfixable one fails only that job             (phase 4)
 *   vram    the copy engine fills VRAM and copies it back    (phase 6)
 */

#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/pci.h>
#include <linux/slab.h>

#include "svmd.h"

static u32 svmd_rd(struct svmd_device *sdev, u32 reg)
{
	return readl(sdev->regs + reg);
}

static void svmd_wr(struct svmd_device *sdev, u32 reg, u32 val)
{
	writel(val, sdev->regs + reg);
}

static int svmd_irq_self_test(struct svmd_device *sdev)
{
	struct device *dev = &sdev->pdev->dev;
	int v;

	svmd_wr(sdev, SVMD_REG_SCRATCH, 0xC0FFEE);
	if (svmd_rd(sdev, SVMD_REG_SCRATCH) != 0xC0FFEE) {
		dev_err(dev, "SCRATCH did not hold its value\n");
		return -EIO;
	}
	for (v = 0; v < SVMD_NUM_VECTORS; v++) {
		reinit_completion(&sdev->irq_seen[v]);
		svmd_wr(sdev, SVMD_REG_IRQ_RAISE, BIT(v));
		if (!wait_for_completion_timeout(&sdev->irq_seen[v], HZ)) {
			dev_err(dev, "interrupt vector %d never arrived\n", v);
			return -ETIMEDOUT;
		}
	}
	dev_info(dev, "self-test irq: SCRATCH ok, %d/%d interrupts\n",
		 SVMD_NUM_VECTORS, SVMD_NUM_VECTORS);
	return 0;
}

/* True if every 32-bit word of the page at `p` is `v`. */
static bool page_is(const u32 *p, u32 v)
{
	for (unsigned int i = 0; i < PAGE_SIZE / 4; i++)
		if (p[i] != v)
			return false;
	return true;
}

static int svmd_engine_self_test(struct svmd_device *sdev, u32 *buf,
				 dma_addr_t dma)
{
	u8 cmds[3][SVMD_CMD_SIZE];
	u32 *p0 = buf, *p1 = buf + PAGE_SIZE / 4;
	int ret;

	/* page 0 = 0x11111111; page 1 = copy of page 0; page 1 += 1.
	 * Kernel commands use PHYS addresses: no translation. */
	svmd_encode_cmd(cmds[0], SVMD_OP_FILL, SVMD_CMD_PHYS, 0x11111111, dma,
			0, PAGE_SIZE);
	svmd_encode_cmd(cmds[1], SVMD_OP_COPY, SVMD_CMD_PHYS, 0,
			dma + PAGE_SIZE, dma, PAGE_SIZE);
	svmd_encode_cmd(cmds[2], SVMD_OP_ADD, SVMD_CMD_PHYS, 1, dma + PAGE_SIZE,
			0, PAGE_SIZE);
	ret = svmd_kernel_job(sdev, NULL, cmds, 3);
	if (ret)
		return ret;
	if (!page_is(p0, 0x11111111) || !page_is(p1, 0x11111112))
		return -EIO;
	dev_info(&sdev->pdev->dev, "self-test engine: FILL, COPY, ADD ok\n");
	return 0;
}

/* One FILL of a whole page at device-virtual address `va`. */
static int fill_va(struct svmd_device *sdev, struct svmd_vm *vm, u64 va,
		   u32 v)
{
	u8 cmd[1][SVMD_CMD_SIZE];

	svmd_encode_cmd(cmd[0], SVMD_OP_FILL, 0, v, va, 0, PAGE_SIZE);
	return svmd_kernel_job(sdev, vm, cmd, 1);
}

/* The self-test VM's fault handler: map one known address, refuse others. */
struct svmd_test_vm {
	u64 fixable_va;
	dma_addr_t fix_page;
	unsigned int resolved;
};

static int svmd_test_resolve(struct svmd_vm *vm, u64 va, bool write)
{
	struct svmd_test_vm *t = vm->priv;
	int ret;

	if (va != t->fixable_va)
		return -EFAULT;
	ret = svmd_pt_prepare(vm, va, va + PAGE_SIZE);
	if (ret)
		return ret;
	svmd_pt_set(vm, va, t->fix_page | SVMD_PTE_VALID | SVMD_PTE_WRITE);
	t->resolved++;
	return 0;
}

#define VA_X	0x0000123440000000ull	/* mapped by hand */
#define VA_Y	0x0000123480000000ull	/* fixable fault */
#define VA_Z	0x00001234c0000000ull	/* unfixable fault */

static int svmd_mmu_self_test(struct svmd_device *sdev, u32 *buf,
			      dma_addr_t dma)
{
	struct device *dev = &sdev->pdev->dev;
	struct svmd_test_vm t = { .fixable_va = VA_Y,
				  .fix_page = dma + 2 * PAGE_SIZE };
	u32 *a = buf, *b = buf + PAGE_SIZE / 4, *c = buf + 2 * PAGE_SIZE / 4;
	u64 faults0 = readq(sdev->regs + SVMD_REG_STAT_FAULTS);
	struct svmd_vm *vm;
	int ret;

	vm = svmd_vm_alloc(sdev);
	if (IS_ERR(vm))
		return PTR_ERR(vm);
	vm->resolve = svmd_test_resolve;
	vm->priv = &t;
	memset(buf, 0, 3 * PAGE_SIZE);

	/* --- tlb: X -> A, and the device caches that ------------------- */
	ret = svmd_pt_prepare(vm, VA_X, VA_X + PAGE_SIZE);
	if (ret)
		goto out;
	svmd_pt_set(vm, VA_X, dma | SVMD_PTE_VALID | SVMD_PTE_WRITE);
	ret = fill_va(sdev, vm, VA_X, 0xAAAAAAAA);	/* SET_ROOT, then X */
	if (ret || !page_is(a, 0xAAAAAAAA)) {
		dev_err(dev, "self-test tlb: translated write failed (%d)\n", ret);
		ret = ret ?: -EIO;
		goto out;
	}

	/* change X -> B in memory, but don't tell the device */
	svmd_pt_set(vm, VA_X, (dma + PAGE_SIZE) | SVMD_PTE_VALID |
			      SVMD_PTE_WRITE);
	ret = fill_va(sdev, NULL, VA_X, 0xBBBBBBBB);	/* no SET_ROOT: TLB kept */
	if (ret || !page_is(a, 0xBBBBBBBB) || !page_is(b, 0)) {
		dev_err(dev, "self-test tlb: expected the STALE page to be written\n");
		ret = ret ?: -EIO;
		goto out;
	}

	/* now invalidate: the device must walk the tables again */
	ret = svmd_tlb_invalidate(sdev, VA_X, PAGE_SIZE);
	if (!ret)
		ret = fill_va(sdev, NULL, VA_X, 0xCCCCCCCC);
	if (ret || !page_is(b, 0xCCCCCCCC) || !page_is(a, 0xBBBBBBBB)) {
		dev_err(dev, "self-test tlb: invalidation didn't take (%d)\n", ret);
		ret = ret ?: -EIO;
		goto out;
	}
	dev_info(dev, "self-test tlb: stale entry used until invalidated, as designed\n");

	/* --- fault: Y is unmapped; the handler maps it to page C -------- */
	ret = fill_va(sdev, vm, VA_Y, 0xDDDDDDDD);
	if (ret || !page_is(c, 0xDDDDDDDD) || t.resolved != 1) {
		dev_err(dev, "self-test fault: fixable fault failed (%d)\n", ret);
		ret = ret ?: -EIO;
		goto out;
	}

	/* --- abort: Z can't be fixed; only that job fails --------------- */
	ret = fill_va(sdev, vm, VA_Z, 0xEEEEEEEE);
	if (ret != -EFAULT) {
		dev_err(dev, "self-test fault: unfixable fault gave %d, not -EFAULT\n",
			ret);
		ret = -EIO;
		goto out;
	}
	ret = fill_va(sdev, vm, VA_X, 0x12345678);	/* engine still alive */
	if (ret || !page_is(b, 0x12345678)) {
		dev_err(dev, "self-test fault: engine dead after abort (%d)\n", ret);
		ret = ret ?: -EIO;
		goto out;
	}
	dev_info(dev, "self-test fault: fixed fault retried, unfixable one failed only its job (%llu faults)\n",
		 readq(sdev->regs + SVMD_REG_STAT_FAULTS) - faults0);
	ret = 0;
out:
	svmd_vm_put(vm);
	return ret;
}

static int svmd_vram_self_test(struct svmd_device *sdev, u32 *buf,
			       dma_addr_t dma)
{
	u8 cmds[2][SVMD_CMD_SIZE];
	int ret;

	if (!sdev->dpagemap)
		return 0;
	memset(buf, 0, PAGE_SIZE);
	/* the last VRAM page, so a wrong size or offset shows up */
	svmd_encode_cmd(cmds[0], SVMD_OP_FILL, SVMD_CMD_PHYS | SVMD_CMD_DST_VRAM,
			0x7a7a7a7a, pci_resource_len(sdev->pdev, SVMD_BAR_VRAM) -
			PAGE_SIZE, 0, PAGE_SIZE);
	svmd_encode_cmd(cmds[1], SVMD_OP_COPY, SVMD_CMD_PHYS | SVMD_CMD_SRC_VRAM,
			0, dma, pci_resource_len(sdev->pdev, SVMD_BAR_VRAM) -
			PAGE_SIZE, PAGE_SIZE);
	ret = svmd_copy_run(sdev, cmds, 2);
	if (ret || !page_is(buf, 0x7a7a7a7a))
		return ret ?: -EIO;
	dev_info(&sdev->pdev->dev, "self-test vram: copy engine filled VRAM and read it back\n");
	return 0;
}

int svmd_selftests(struct svmd_device *sdev)
{
	struct device *dev = &sdev->pdev->dev;
	dma_addr_t dma;
	u32 *buf;
	int ret;

	ret = svmd_irq_self_test(sdev);
	if (ret)
		return ret;

	buf = dma_alloc_coherent(dev, 3 * PAGE_SIZE, &dma, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	ret = svmd_engine_self_test(sdev, buf, dma);
	if (!ret)
		ret = svmd_mmu_self_test(sdev, buf, dma);
	if (!ret)
		ret = svmd_vram_self_test(sdev, buf, dma);
	dma_free_coherent(dev, 3 * PAGE_SIZE, buf, dma);
	if (ret)
		dev_err(dev, "self-tests failed (%d)\n", ret);
	return ret;
}
