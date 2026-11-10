/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * svmd_regs.h - the SVMD hardware interface, shared by the QEMU device
 * and the Linux driver so the two can never disagree.
 *
 * This file is docs/hardware-spec.md turned into numbers. Change the spec
 * first, then this file.
 */
#ifndef SVMD_REGS_H
#define SVMD_REGS_H

/* §1 PCI identity */
#define SVMD_PCI_VENDOR_ID	0x1234
#define SVMD_PCI_DEVICE_ID	0x5356
#define SVMD_PCI_CLASS		0x1200	/* processing accelerator */

#define SVMD_BAR_REGS		0
#define SVMD_BAR_VRAM		2
#define SVMD_BAR_MSIX		4
#define SVMD_REGS_SIZE		0x1000

/* MSI-X vectors */
#define SVMD_VEC_JOB		0
#define SVMD_VEC_FAULT		1
#define SVMD_VEC_ERROR		2
#define SVMD_NUM_VECTORS	3

/* §2 registers (byte offsets into BAR0) */
#define SVMD_REG_ID		0x000
#define SVMD_REG_VERSION	0x004
#define SVMD_REG_CAPS		0x008
#define SVMD_REG_SCRATCH	0x00C
#define SVMD_REG_STATUS		0x010
#define SVMD_REG_CONTROL	0x014
#define SVMD_REG_ERROR_CODE	0x018
#define SVMD_REG_IRQ_STATUS	0x020
#define SVMD_REG_IRQ_MASK	0x024
#define SVMD_REG_IRQ_RAISE	0x028

#define SVMD_REG_RING_BASE	0x040	/* 64-bit */
#define SVMD_REG_RING_SIZE	0x048
#define SVMD_REG_RING_HEAD	0x04C
#define SVMD_REG_RING_TAIL	0x050
#define SVMD_REG_FENCE_ADDR	0x058	/* 64-bit */
#define SVMD_REG_FENCE_VALUE	0x060

#define SVMD_REG_PT_ROOT	0x080	/* 64-bit */
#define SVMD_REG_MMU_CONTROL	0x088
#define SVMD_REG_INVAL_VA	0x090	/* 64-bit */
#define SVMD_REG_INVAL_SIZE	0x098	/* 64-bit */
#define SVMD_REG_INVAL_CMD	0x0A0
#define SVMD_REG_INVAL_DONE	0x0A4

#define SVMD_REG_FAULTQ_BASE	0x0C0	/* 64-bit */
#define SVMD_REG_FAULTQ_SIZE	0x0C8
#define SVMD_REG_FAULTQ_HEAD	0x0CC
#define SVMD_REG_FAULTQ_TAIL	0x0D0
#define SVMD_REG_FAULT_RESUME	0x0D4

#define SVMD_REG_INJECT		0x100
#define SVMD_REG_STAT_FAULTS	0x140	/* 64-bit */
#define SVMD_REG_STAT_TLB_HITS	0x148	/* 64-bit */
#define SVMD_REG_STAT_TLB_MISSES 0x150	/* 64-bit */
#define SVMD_REG_STAT_BYTES	0x158	/* 64-bit */
#define SVMD_REG_STAT_INVALS	0x160	/* 64-bit */

/* §10 the copy engine: a second, simpler ring (PHYS only, never faults) */
#define SVMD_REG_CRING_BASE	0x180	/* 64-bit */
#define SVMD_REG_CRING_SIZE	0x188
#define SVMD_REG_CRING_HEAD	0x18C
#define SVMD_REG_CRING_TAIL	0x190	/* doorbell */
#define SVMD_REG_CFENCE_VALUE	0x198
#define SVMD_REG_CERROR		0x19C

/* register values */
#define SVMD_ID_MAGIC		0x53564D44	/* "SVMD" */
#define SVMD_VERSION_1		0x00000001

#define SVMD_CAPS_MMU		(1u << 0)
#define SVMD_CAPS_FAULTS	(1u << 1)
#define SVMD_CAPS_VRAM		(1u << 2)

/* bit n of IRQ_STATUS / IRQ_MASK / IRQ_RAISE is MSI-X vector n */
#define SVMD_IRQ_JOB		(1u << SVMD_VEC_JOB)
#define SVMD_IRQ_FAULT		(1u << SVMD_VEC_FAULT)
#define SVMD_IRQ_ERROR		(1u << SVMD_VEC_ERROR)
#define SVMD_IRQ_ALL		(SVMD_IRQ_JOB | SVMD_IRQ_FAULT | SVMD_IRQ_ERROR)

/* STATUS / CONTROL */
#define SVMD_STATUS_BUSY	(1u << 0)
#define SVMD_STATUS_STALLED	(1u << 1)
#define SVMD_STATUS_ERROR	(1u << 2)
#define SVMD_CONTROL_ENGINE_ENABLE (1u << 0)
#define SVMD_CONTROL_RESET	(1u << 1)

/* §8 ERROR_CODE */
#define SVMD_ERR_NONE		0
#define SVMD_ERR_OPCODE		1
#define SVMD_ERR_ALIGN		2
#define SVMD_ERR_PTE		3
#define SVMD_ERR_DMA		4
#define SVMD_ERR_ABORTED	5
#define SVMD_ERR_RING		6

/* §4 commands: 32 bytes each, little-endian */
#define SVMD_CMD_SIZE		32
#define SVMD_OP_NOP		0x00
#define SVMD_OP_FILL		0x01
#define SVMD_OP_COPY		0x02
#define SVMD_OP_ADD		0x03
#define SVMD_OP_FENCE		0x04
#define SVMD_OP_SET_ROOT	0x05	/* kernel only: dst = page-table root */
#define SVMD_CMD_IRQ		(1u << 0)	/* FENCE: raise the JOB interrupt */
#define SVMD_CMD_PHYS		(1u << 1)	/* skip the MMU (kernel only) */
#define SVMD_CMD_DST_VRAM	(1u << 2)	/* PHYS: dst is a VRAM offset */
#define SVMD_CMD_SRC_VRAM	(1u << 3)	/* PHYS: src is a VRAM offset */

/* §5 MMU: 48-bit VAs, 4 KiB pages, 4 levels of 512 entries */
#define SVMD_MMU_ENABLE		(1u << 0)
#define SVMD_VA_BITS		48
#define SVMD_PAGE_SHIFT		12
#define SVMD_PAGE_SIZE		(1ull << SVMD_PAGE_SHIFT)
#define SVMD_PT_LEVELS		4
#define SVMD_PT_ENTRIES		512
#define SVMD_PTE_VALID		(1ull << 0)
#define SVMD_PTE_WRITE		(1ull << 1)
#define SVMD_PTE_VRAM		(1ull << 2)	/* leaf only */
#define SVMD_PTE_ADDR_MASK	0x000ffffffffff000ull
#define SVMD_PTE_RSVD_MASK	(~(SVMD_PTE_ADDR_MASK | 7ull))
/* index of `va` in the table at `level` (0 = root) */
#define SVMD_PT_INDEX(va, level) \
	(((va) >> (SVMD_PAGE_SHIFT + 9 * (SVMD_PT_LEVELS - 1 - (level)))) & 511)

/* §6 faults */
#define SVMD_FAULT_REC_SIZE	32
#define SVMD_FAULT_VA		0	/* u64, page aligned */
#define SVMD_FAULT_ACCESS	8	/* u32 */
#define SVMD_FAULT_REASON	12	/* u32 */
#define SVMD_FAULT_SLOT		16	/* u32 */
#define SVMD_FAULT_LEVEL	20	/* u32 */
#define SVMD_FAULT_ROOT		24	/* u64: page table in use */
#define SVMD_ACCESS_WRITE	(1u << 0)
#define SVMD_ACCESS_READ	(1u << 1)
#define SVMD_REASON_NOT_PRESENT	1
#define SVMD_REASON_READ_ONLY	2
#define SVMD_REASON_BAD_ENTRY	3
#define SVMD_RESUME_RETRY	1
#define SVMD_RESUME_ABORT	2

/* §9 fault injection (INJECT) */
#define SVMD_INJECT_DUP_FAULTS	(1u << 0)
#define SVMD_INJECT_SLOW_INVAL	(1u << 1)
#define SVMD_INJECT_SPURIOUS	(1u << 2)	/* self-clearing */
#define SVMD_INJECT_NO_TLB	(1u << 3)

#endif /* SVMD_REGS_H */
