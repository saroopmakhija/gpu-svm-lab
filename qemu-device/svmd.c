/*
 * SVMD - a toy GPU-like PCI device with shared virtual memory.
 *
 * The hardware manual is docs/hardware-spec.md; register numbers come from
 * include/svmd_regs.h, which the Linux driver uses too.
 *
 * Phase 1: identity registers, SCRATCH, and the three MSI-X interrupts.
 * Phase 2: the command ring, the doorbell, and an engine that runs
 *          FILL / COPY / ADD / FENCE.
 * Phase 3: an MMU - 4-level page tables, a 16-entry TLB that is NOT kept
 *          coherent with memory, and invalidation the driver must wait for.
 * Phase 4: replayable faults - stall, report in a queue, wait for the
 *          driver to say retry or abort.
 * Phase 6: VRAM (BAR2), reachable through VRAM page-table entries and
 *          kernel PHYS copies.
 *          A second, simpler copy engine moves data for migration - it
 *          can't fault, so it works even while the main engine is stalled.
 * Phase 7: fault injection, to make the driver's error paths run.
 *
 * Every engine step runs under QEMU's big lock, as do register accesses:
 * an invalidation or a resume can never land in the middle of a step.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msix.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "qom/object.h"
#include "svmd_regs.h"

#define TYPE_SVMD "svmd"

/* §4: one command, decoded from its 32 little-endian bytes */
typedef struct SvmdCmd {
    uint8_t opcode;
    uint8_t flags;
    uint32_t arg32;
    uint64_t dst, src, len;
} SvmdCmd;

/* §5: one cached translation */
typedef struct SvmdTlbEntry {
    bool valid;
    uint64_t vpn;               /* va >> 12 */
    uint64_t page;              /* DMA address, or VRAM offset */
    bool write;
    bool vram;
} SvmdTlbEntry;

/* Where a translated access goes. */
typedef struct SvmdPhys {
    uint64_t addr;
    bool vram;
} SvmdPhys;

typedef enum { XLATE_OK, XLATE_FAULT, XLATE_ERROR } SvmdXlate;

/* Work done per engine run before letting the rest of QEMU have a turn. */
#define SVMD_ENGINE_BUDGET  (1 * MiB)
#define SVMD_CHUNK          4096
#define SVMD_TLB_ENTRIES    16
OBJECT_DECLARE_SIMPLE_TYPE(SvmdState, SVMD)

struct SvmdState {
    PCIDevice parent_obj;

    MemoryRegion regs;          /* BAR0 */
    MemoryRegion vram;          /* BAR2 */
    uint64_t vram_size;         /* property */
    uint32_t inval_delay_us;    /* property: INJECT.SLOW_INVAL delay */

    uint32_t scratch;
    uint32_t irq_status;        /* pending interrupts, bit n = vector n */
    uint32_t irq_mask;          /* 1 = enabled */

    /* phase 2: the ring and the engine */
    uint32_t control;           /* SVMD_CONTROL_* */
    uint32_t error_code;        /* SVMD_ERR_*, 0 = running fine */
    uint64_t ring_base;
    uint32_t ring_size;         /* slots */
    uint32_t ring_head;         /* next slot the engine reads */
    uint32_t ring_tail;         /* one past the last slot the driver filled */
    uint64_t fence_addr;
    uint32_t fence_value;

    QEMUBH *engine_bh;          /* runs svmd_engine_run "soon" */
    bool have_cmd;              /* `cmd` is fetched and partly done */
    SvmdCmd cmd;
    uint64_t progress;          /* bytes of `cmd` already done */

    /* phase 3: the MMU */
    uint32_t mmu_control;
    uint64_t pt_root;
    SvmdTlbEntry tlb[SVMD_TLB_ENTRIES];
    unsigned tlb_next;          /* round-robin replacement */
    uint64_t inval_va, inval_size;
    uint32_t inval_done;
    QEMUTimer *inval_timer;     /* INJECT.SLOW_INVAL */
    bool inval_pending;
    uint64_t pend_va, pend_size;
    uint32_t pend_seqno;

    /* phase 4: faults */
    uint64_t faultq_base;
    uint32_t faultq_size, faultq_head, faultq_tail;
    bool stalled;               /* waiting for FAULT_RESUME */
    bool report_pending;        /* fault not yet in the queue (it was full) */
    bool aborting;              /* skipping to the next FENCE */
    uint8_t fault_rec[SVMD_FAULT_REC_SIZE];

    /* phase 6: the copy engine (§10) */
    QEMUBH *copy_bh;
    uint64_t cring_base;
    uint32_t cring_size, cring_head, cring_tail;
    uint32_t cfence_value, cerror;

    uint32_t inject;
    uint64_t stat_bytes, stat_faults, stat_tlb_hits, stat_tlb_misses;
    uint64_t stat_invals;
};

/*
 * Send an MSI-X message for every interrupt that is pending AND enabled.
 *
 * MSI-X is edge-style: each message is one "ding". So we only ding for
 * bits in `newly` - ones that just became pending-and-enabled - not for
 * every pending bit on every call, which would spam the driver.
 */
static void svmd_send_irqs(SvmdState *s, uint32_t newly)
{
    PCIDevice *pdev = PCI_DEVICE(s);

    newly &= s->irq_status & s->irq_mask;
    for (unsigned v = 0; v < SVMD_NUM_VECTORS; v++) {
        if (newly & (1u << v)) {
            msix_notify(pdev, v);
        }
    }
}

/* Something inside the device happened that wants interrupt bits `bits`. */
static void svmd_raise(SvmdState *s, uint32_t bits)
{
    uint32_t newly = bits & ~s->irq_status;     /* not already pending */

    s->irq_status |= bits & SVMD_IRQ_ALL;
    svmd_send_irqs(s, newly);
}

/*--- the engine (§3-4) --------------------------------------------------*/

static void svmd_engine_kick(SvmdState *s)
{
    qemu_bh_schedule(s->engine_bh);
}

/* Something went wrong: stop the engine until the driver resets it (§8). */
static void svmd_engine_error(SvmdState *s, uint32_t code)
{
    s->error_code = code;
    qemu_log_mask(LOG_GUEST_ERROR, "svmd: engine error %u at slot %u\n",
                  code, s->ring_head);
    svmd_raise(s, SVMD_IRQ_ERROR);
}

static bool svmd_engine_ok(SvmdState *s)
{
    return (s->control & SVMD_CONTROL_ENGINE_ENABLE) && !s->error_code;
}

/* Read or write system memory (DMA) or VRAM. False if out of range. */
static bool svmd_mem_raw(SvmdState *s, SvmdPhys p, void *buf, uint64_t len,
                         bool write)
{
    if (p.vram) {
        uint8_t *vram = memory_region_get_ram_ptr(&s->vram);

        if (p.addr > s->vram_size || len > s->vram_size - p.addr) {
            return false;
        }
        if (write) {
            memcpy(vram + p.addr, buf, len);
        } else {
            memcpy(buf, vram + p.addr, len);
        }
    } else if ((write ? pci_dma_write(PCI_DEVICE(s), p.addr, buf, len)
                      : pci_dma_read(PCI_DEVICE(s), p.addr, buf, len))
               != MEMTX_OK) {
        return false;
    }
    s->stat_bytes += len;
    return true;
}

/* The main engine's accesses: a failure stops it (§8). */
static bool svmd_mem(SvmdState *s, SvmdPhys p, void *buf, uint64_t len,
                     bool write)
{
    if (!svmd_mem_raw(s, p, buf, len, write)) {
        svmd_engine_error(s, SVMD_ERR_DMA);
        return false;
    }
    return true;
}

static bool svmd_dma(SvmdState *s, uint64_t addr, void *buf, uint64_t len,
                     bool write)
{
    return svmd_mem(s, (SvmdPhys){ .addr = addr }, buf, len, write);
}

/*--- the MMU (§5) -------------------------------------------------------*/

static void svmd_tlb_flush(SvmdState *s, uint64_t va, uint64_t size)
{
    uint64_t first = va >> SVMD_PAGE_SHIFT;
    uint64_t last = (va + size - 1) >> SVMD_PAGE_SHIFT;

    for (unsigned i = 0; i < SVMD_TLB_ENTRIES; i++) {
        SvmdTlbEntry *e = &s->tlb[i];

        if (!size || (e->vpn >= first && e->vpn <= last)) {
            e->valid = false;
        }
    }
}

/*
 * Translate one access. Look in the TLB first; on a miss, walk the four
 * levels of tables in guest memory and cache the result. The TLB is never
 * told when the tables change - only INVAL_CMD and SET_ROOT empty it.
 */
static SvmdXlate svmd_translate(SvmdState *s, uint64_t va, bool write,
                                bool phys, bool phys_vram, SvmdPhys *out,
                                uint32_t *reason, uint32_t *level)
{
    uint64_t vpn = va >> SVMD_PAGE_SHIFT, table = s->pt_root, pte = 0;
    SvmdTlbEntry *e = NULL;

    if (phys || !(s->mmu_control & SVMD_MMU_ENABLE)) {
        *out = (SvmdPhys){ .addr = va, .vram = phys_vram };
        return XLATE_OK;
    }

    if (!(s->inject & SVMD_INJECT_NO_TLB)) {
        for (unsigned i = 0; i < SVMD_TLB_ENTRIES; i++) {
            if (s->tlb[i].valid && s->tlb[i].vpn == vpn) {
                e = &s->tlb[i];
                break;
            }
        }
    }
    if (e) {
        s->stat_tlb_hits++;
    } else {
        SvmdTlbEntry walked = { .valid = true, .vpn = vpn };

        s->stat_tlb_misses++;
        if (va >> SVMD_VA_BITS || !table) {
            *reason = SVMD_REASON_NOT_PRESENT;
            *level = 0;
            return XLATE_FAULT;
        }
        for (unsigned l = 0; l < SVMD_PT_LEVELS; l++) {
            uint8_t raw[8];

            if (!svmd_dma(s, table + SVMD_PT_INDEX(va, l) * 8, raw, 8, false)) {
                return XLATE_ERROR;
            }
            pte = ldq_le_p(raw);
            *level = l;
            if (!(pte & SVMD_PTE_VALID)) {
                *reason = SVMD_REASON_NOT_PRESENT;
                return XLATE_FAULT;
            }
            if ((pte & SVMD_PTE_RSVD_MASK) ||
                (l < SVMD_PT_LEVELS - 1 && (pte & SVMD_PTE_VRAM))) {
                svmd_engine_error(s, SVMD_ERR_PTE);
                return XLATE_ERROR;
            }
            table = pte & SVMD_PTE_ADDR_MASK;
        }
        walked.page = pte & SVMD_PTE_ADDR_MASK;
        walked.write = pte & SVMD_PTE_WRITE;
        walked.vram = pte & SVMD_PTE_VRAM;
        if (s->inject & SVMD_INJECT_NO_TLB) {
            static SvmdTlbEntry uncached;
            uncached = walked;
            e = &uncached;
        } else {
            e = &s->tlb[s->tlb_next];
            s->tlb_next = (s->tlb_next + 1) % SVMD_TLB_ENTRIES;
            *e = walked;
        }
    }

    if (write && !e->write) {
        *reason = SVMD_REASON_READ_ONLY;
        *level = SVMD_PT_LEVELS - 1;
        return XLATE_FAULT;
    }
    *out = (SvmdPhys){ .addr = e->page | (va & (SVMD_PAGE_SIZE - 1)),
                       .vram = e->vram };
    return XLATE_OK;
}

static void svmd_inval_apply(SvmdState *s, uint64_t va, uint64_t size,
                             uint32_t seqno)
{
    svmd_tlb_flush(s, va, size);
    s->inval_done = seqno;
    s->stat_invals++;
}

/* INJECT.SLOW_INVAL: the invalidation lands late, like busy hardware. */
static void svmd_inval_timer(void *opaque)
{
    SvmdState *s = opaque;

    if (s->inval_pending) {
        s->inval_pending = false;
        svmd_inval_apply(s, s->pend_va, s->pend_size, s->pend_seqno);
    }
}

/*--- faults (§6) --------------------------------------------------------*/

/* Put the stalled access's record in the queue, if there is room. */
static void svmd_report_fault(SvmdState *s)
{
    int copies = (s->inject & SVMD_INJECT_DUP_FAULTS) ? 2 : 1;

    for (int i = 0; i < copies; i++) {
        uint32_t next = (s->faultq_tail + 1) % s->faultq_size;

        if (!s->faultq_size || next == s->faultq_head) {
            s->report_pending = (i == 0);       /* full: try again later */
            break;
        }
        if (!svmd_dma(s, s->faultq_base +
                      (uint64_t)s->faultq_tail * SVMD_FAULT_REC_SIZE,
                      s->fault_rec, SVMD_FAULT_REC_SIZE, true)) {
            return;
        }
        s->faultq_tail = next;
        s->report_pending = false;
    }
    if (!s->report_pending) {
        s->stat_faults++;
        svmd_raise(s, SVMD_IRQ_FAULT);
    }
}

/* Stop at this exact access and ask the driver for help. */
static void svmd_fault(SvmdState *s, uint64_t va, bool write,
                       uint32_t reason, uint32_t level)
{
    memset(s->fault_rec, 0, sizeof(s->fault_rec));
    stq_le_p(s->fault_rec + SVMD_FAULT_VA, va & ~(SVMD_PAGE_SIZE - 1));
    stl_le_p(s->fault_rec + SVMD_FAULT_ACCESS,
             write ? SVMD_ACCESS_WRITE : SVMD_ACCESS_READ);
    stl_le_p(s->fault_rec + SVMD_FAULT_REASON, reason);
    stl_le_p(s->fault_rec + SVMD_FAULT_SLOT, s->ring_head);
    stl_le_p(s->fault_rec + SVMD_FAULT_LEVEL, level);
    stq_le_p(s->fault_rec + SVMD_FAULT_ROOT, s->pt_root);
    s->stalled = true;
    svmd_report_fault(s);
}

/*--- running commands ---------------------------------------------------*/

/* Read the next ring slot into s->cmd and check it. */
static bool svmd_fetch(SvmdState *s)
{
    uint8_t raw[SVMD_CMD_SIZE];
    SvmdCmd *c = &s->cmd;

    if (!svmd_dma(s, s->ring_base + (uint64_t)s->ring_head * SVMD_CMD_SIZE,
                  raw, sizeof(raw), false)) {
        return false;
    }
    c->opcode = raw[0];
    c->flags = raw[1];
    c->arg32 = ldl_le_p(raw + 4);
    c->dst = ldq_le_p(raw + 8);
    c->src = ldq_le_p(raw + 16);
    c->len = ldq_le_p(raw + 24);

    switch (c->opcode) {
    case SVMD_OP_NOP:
    case SVMD_OP_COPY:
    case SVMD_OP_FENCE:
    case SVMD_OP_SET_ROOT:
        break;
    case SVMD_OP_FILL:
    case SVMD_OP_ADD:
        if (c->len % 4) {
            svmd_engine_error(s, SVMD_ERR_ALIGN);
            return false;
        }
        break;
    default:
        svmd_engine_error(s, SVMD_ERR_OPCODE);
        return false;
    }
    s->have_cmd = true;
    s->progress = 0;
    return true;
}

/*
 * Do up to `max` bytes of the current command, starting at s->progress.
 * Returns bytes done; 0 if the engine stalled on a fault or hit an error.
 *
 * Resuming from `progress` (instead of restarting) keeps ADD correct when
 * work is split across runs - and when a fault stops a command half way.
 * With the MMU on, a step never crosses a page, and every page it needs
 * is translated BEFORE any memory is touched: a step either happens
 * completely or not at all.
 */
static uint64_t svmd_step(SvmdState *s, uint64_t max)
{
    SvmdCmd *c = &s->cmd;
    bool phys = c->flags & SVMD_CMD_PHYS;
    uint8_t buf[SVMD_CHUNK];
    uint64_t off = s->progress, n, dva = c->dst + off, sva = c->src + off;
    SvmdPhys dst, src;
    uint32_t reason, level;
    SvmdXlate x;

    n = MIN(MIN(c->len - off, max), SVMD_CHUNK);
    if (!phys) {
        n = MIN(n, SVMD_PAGE_SIZE - (dva & (SVMD_PAGE_SIZE - 1)));
        if (c->opcode == SVMD_OP_COPY) {
            n = MIN(n, SVMD_PAGE_SIZE - (sva & (SVMD_PAGE_SIZE - 1)));
        }
    }

    if (c->opcode == SVMD_OP_COPY) {
        x = svmd_translate(s, sva, false, phys, c->flags & SVMD_CMD_SRC_VRAM,
                           &src, &reason, &level);
        if (x == XLATE_FAULT) {
            svmd_fault(s, sva, false, reason, level);
        }
        if (x != XLATE_OK) {
            return 0;
        }
    }
    x = svmd_translate(s, dva, true, phys, c->flags & SVMD_CMD_DST_VRAM,
                       &dst, &reason, &level);
    if (x == XLATE_FAULT) {
        svmd_fault(s, dva, true, reason, level);
    }
    if (x != XLATE_OK) {
        return 0;
    }

    switch (c->opcode) {
    case SVMD_OP_FILL:
        for (uint64_t i = 0; i < n; i += 4) {
            stl_le_p(buf + i, c->arg32);
        }
        return svmd_mem(s, dst, buf, n, true) ? n : 0;
    case SVMD_OP_COPY:
        if (!svmd_mem(s, src, buf, n, false)) {
            return 0;
        }
        return svmd_mem(s, dst, buf, n, true) ? n : 0;
    case SVMD_OP_ADD:
        if (!svmd_mem(s, dst, buf, n, false)) {
            return 0;
        }
        for (uint64_t i = 0; i < n; i += 4) {
            stl_le_p(buf + i, ldl_le_p(buf + i) + c->arg32);
        }
        return svmd_mem(s, dst, buf, n, true) ? n : 0;
    }
    g_assert_not_reached();
}

/* FENCE: "everything before me is done" (§4). */
static bool svmd_fence(SvmdState *s)
{
    uint8_t v[4];

    if (s->fence_addr) {
        stl_le_p(v, s->cmd.arg32);
        if (!svmd_dma(s, s->fence_addr, v, sizeof(v), true)) {
            return false;
        }
    }
    s->fence_value = s->cmd.arg32;
    if (s->cmd.flags & SVMD_CMD_IRQ) {
        svmd_raise(s, SVMD_IRQ_JOB);
    }
    return true;
}

/* Bottom half: run commands until the ring is empty, the budget is spent,
 * or the engine stalls on a fault. */
static void svmd_engine_run(void *opaque)
{
    SvmdState *s = opaque;
    uint64_t budget = SVMD_ENGINE_BUDGET;

    while (svmd_engine_ok(s) && !s->stalled) {
        if (!s->have_cmd) {
            if (s->ring_head == s->ring_tail) {
                return;                         /* idle */
            }
            if (!svmd_fetch(s)) {
                return;
            }
        }

        switch (s->cmd.opcode) {
        case SVMD_OP_FENCE:
            if (!svmd_fence(s)) {
                return;
            }
            s->aborting = false;                /* the aborted job is over */
            break;
        case SVMD_OP_SET_ROOT:
            s->pt_root = s->cmd.dst;
            svmd_tlb_flush(s, 0, 0);
            break;
        case SVMD_OP_NOP:
            break;
        default: {
            uint64_t done;

            if (s->aborting) {
                break;                          /* skip the rest of the job */
            }
            if (budget == 0) {
                svmd_engine_kick(s);            /* more next time */
                return;
            }
            done = svmd_step(s, budget);
            if (!done) {
                return;                         /* stalled or error */
            }
            s->progress += done;
            budget -= done;
            if (s->progress < s->cmd.len) {
                continue;
            }
        }
        }

        /* command finished: move to the next slot */
        s->have_cmd = false;
        s->ring_head = (s->ring_head + 1) % s->ring_size;
    }
}

static uint32_t svmd_status(SvmdState *s)
{
    uint32_t st = 0;

    if (s->have_cmd || s->ring_head != s->ring_tail) {
        st |= SVMD_STATUS_BUSY;
    }
    if (s->stalled) {
        st |= SVMD_STATUS_STALLED;
    }
    if (s->error_code) {
        st |= SVMD_STATUS_ERROR;
    }
    return st;
}

/* Forget all engine, MMU and fault state (CONTROL.RESET and device reset). */
static void svmd_engine_reset(SvmdState *s)
{
    s->control = 0;
    s->error_code = 0;
    s->ring_base = s->ring_size = s->ring_head = s->ring_tail = 0;
    s->fence_addr = 0;
    s->fence_value = 0;
    s->have_cmd = false;
    s->progress = 0;
    s->mmu_control = 0;
    s->pt_root = 0;
    svmd_tlb_flush(s, 0, 0);
    s->inval_pending = false;
    timer_del(s->inval_timer);
    s->faultq_base = s->faultq_size = s->faultq_head = s->faultq_tail = 0;
    s->stalled = s->report_pending = s->aborting = false;
    s->cring_base = s->cring_size = s->cring_head = s->cring_tail = 0;
    s->cfence_value = s->cerror = 0;
}

/*--- the copy engine (§10) ----------------------------------------------*/

static void svmd_copy_error(SvmdState *s, uint32_t code)
{
    s->cerror = code;
    qemu_log_mask(LOG_GUEST_ERROR, "svmd: copy engine error %u\n", code);
}

/* Run copy-ring commands to completion. PHYS only: nothing can fault. */
static void svmd_copy_run(void *opaque)
{
    SvmdState *s = opaque;
    uint8_t raw[SVMD_CMD_SIZE], buf[SVMD_CHUNK];

    while (!s->cerror && s->cring_size && s->cring_head != s->cring_tail) {
        uint64_t slot = s->cring_base + (uint64_t)s->cring_head * SVMD_CMD_SIZE;
        uint8_t op, flags;
        uint32_t arg;
        uint64_t dst, src, len;

        if (!svmd_mem_raw(s, (SvmdPhys){ .addr = slot }, raw, sizeof(raw),
                          false)) {
            svmd_copy_error(s, SVMD_ERR_DMA);
            return;
        }
        op = raw[0];
        flags = raw[1];
        arg = ldl_le_p(raw + 4);
        dst = ldq_le_p(raw + 8);
        src = ldq_le_p(raw + 16);
        len = ldq_le_p(raw + 24);

        if (op == SVMD_OP_FENCE) {
            s->cfence_value = arg;
        } else if ((op == SVMD_OP_COPY || op == SVMD_OP_FILL) &&
                   (flags & SVMD_CMD_PHYS)) {
            SvmdPhys d = { .addr = dst, .vram = flags & SVMD_CMD_DST_VRAM };
            SvmdPhys sr = { .addr = src, .vram = flags & SVMD_CMD_SRC_VRAM };

            if (op == SVMD_OP_FILL && len % 4) {
                svmd_copy_error(s, SVMD_ERR_ALIGN);
                return;
            }
            for (uint64_t off = 0; off < len; off += SVMD_CHUNK) {
                uint64_t n = MIN(len - off, SVMD_CHUNK);
                SvmdPhys dn = { d.addr + off, d.vram };
                SvmdPhys sn = { sr.addr + off, sr.vram };

                if (op == SVMD_OP_FILL) {
                    for (uint64_t i = 0; i < n; i += 4) {
                        stl_le_p(buf + i, arg);
                    }
                } else if (!svmd_mem_raw(s, sn, buf, n, false)) {
                    svmd_copy_error(s, SVMD_ERR_DMA);
                    return;
                }
                if (!svmd_mem_raw(s, dn, buf, n, true)) {
                    svmd_copy_error(s, SVMD_ERR_DMA);
                    return;
                }
            }
        } else {
            svmd_copy_error(s, SVMD_ERR_OPCODE);
            return;
        }
        s->cring_head = (s->cring_head + 1) % s->cring_size;
    }
}

/*--- registers (§2) -----------------------------------------------------*/

/*
 * The guest read `size` bytes at offset `addr` of BAR0. Whatever we return
 * is what the driver's readl() sees: this function IS the hardware.
 */
static uint64_t svmd_read(void *opaque, hwaddr addr, unsigned size)
{
    SvmdState *s = opaque;

    switch (addr) {
    case SVMD_REG_ID:           return SVMD_ID_MAGIC;
    case SVMD_REG_VERSION:      return SVMD_VERSION_1;
    case SVMD_REG_CAPS:
        return SVMD_CAPS_MMU | SVMD_CAPS_FAULTS | SVMD_CAPS_VRAM;
    case SVMD_REG_SCRATCH:      return s->scratch;
    case SVMD_REG_IRQ_STATUS:   return s->irq_status;
    case SVMD_REG_IRQ_MASK:     return s->irq_mask;
    case SVMD_REG_STATUS:       return svmd_status(s);
    case SVMD_REG_CONTROL:      return s->control;
    case SVMD_REG_ERROR_CODE:   return s->error_code;
    case SVMD_REG_RING_BASE:    return s->ring_base;
    case SVMD_REG_RING_SIZE:    return s->ring_size;
    case SVMD_REG_RING_HEAD:    return s->ring_head;
    case SVMD_REG_RING_TAIL:    return s->ring_tail;
    case SVMD_REG_FENCE_ADDR:   return s->fence_addr;
    case SVMD_REG_FENCE_VALUE:  return s->fence_value;
    case SVMD_REG_PT_ROOT:      return s->pt_root;
    case SVMD_REG_MMU_CONTROL:  return s->mmu_control;
    case SVMD_REG_INVAL_VA:     return s->inval_va;
    case SVMD_REG_INVAL_SIZE:   return s->inval_size;
    case SVMD_REG_INVAL_DONE:   return s->inval_done;
    case SVMD_REG_FAULTQ_BASE:  return s->faultq_base;
    case SVMD_REG_FAULTQ_SIZE:  return s->faultq_size;
    case SVMD_REG_FAULTQ_HEAD:  return s->faultq_head;
    case SVMD_REG_FAULTQ_TAIL:  return s->faultq_tail;
    case SVMD_REG_INJECT:       return s->inject;
    case SVMD_REG_CRING_BASE:   return s->cring_base;
    case SVMD_REG_CRING_SIZE:   return s->cring_size;
    case SVMD_REG_CRING_HEAD:   return s->cring_head;
    case SVMD_REG_CRING_TAIL:   return s->cring_tail;
    case SVMD_REG_CFENCE_VALUE: return s->cfence_value;
    case SVMD_REG_CERROR:       return s->cerror;
    case SVMD_REG_STAT_FAULTS:  return s->stat_faults;
    case SVMD_REG_STAT_TLB_HITS:   return s->stat_tlb_hits;
    case SVMD_REG_STAT_TLB_MISSES: return s->stat_tlb_misses;
    case SVMD_REG_STAT_BYTES:   return s->stat_bytes;
    case SVMD_REG_STAT_INVALS:  return s->stat_invals;
    default:
        qemu_log_mask(LOG_UNIMP, "svmd: read of unimplemented reg 0x%03"
                      HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static bool svmd_pow2(uint64_t v)
{
    return v && !(v & (v - 1));
}

static void svmd_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    SvmdState *s = opaque;
    uint32_t old;

    switch (addr) {
    case SVMD_REG_SCRATCH:
        s->scratch = val;
        break;
    case SVMD_REG_IRQ_STATUS:
        /* write-1-to-clear: 0 bits leave their interrupt alone */
        s->irq_status &= ~(uint32_t)val;
        break;
    case SVMD_REG_IRQ_MASK:
        old = s->irq_mask;
        s->irq_mask = val & SVMD_IRQ_ALL;
        /* unmasking an interrupt that is already pending delivers it now */
        svmd_send_irqs(s, s->irq_mask & ~old);
        break;
    case SVMD_REG_IRQ_RAISE:
        svmd_raise(s, val);
        break;
    case SVMD_REG_CONTROL:
        if (val & SVMD_CONTROL_RESET) {
            svmd_engine_reset(s);
            break;
        }
        s->control = val & SVMD_CONTROL_ENGINE_ENABLE;
        svmd_engine_kick(s);
        break;
    case SVMD_REG_RING_BASE:
        s->ring_base = val;
        break;
    case SVMD_REG_RING_SIZE:
        /* a power of 2, and only while the ring is empty (§3) */
        if (!svmd_pow2(val) || s->ring_head != s->ring_tail) {
            svmd_engine_error(s, SVMD_ERR_RING);
            break;
        }
        s->ring_size = val;
        s->ring_head = s->ring_tail = 0;
        break;
    case SVMD_REG_RING_TAIL:
        /* the doorbell: only schedules the engine, never runs it here */
        if (val >= s->ring_size) {
            svmd_engine_error(s, SVMD_ERR_RING);
            break;
        }
        s->ring_tail = val;
        svmd_engine_kick(s);
        break;
    case SVMD_REG_FENCE_ADDR:
        s->fence_addr = val;
        break;
    case SVMD_REG_PT_ROOT:
        s->pt_root = val;
        svmd_tlb_flush(s, 0, 0);
        break;
    case SVMD_REG_MMU_CONTROL:
        s->mmu_control = val & SVMD_MMU_ENABLE;
        svmd_tlb_flush(s, 0, 0);
        break;
    case SVMD_REG_INVAL_VA:
        s->inval_va = val;
        break;
    case SVMD_REG_INVAL_SIZE:
        s->inval_size = val;
        break;
    case SVMD_REG_INVAL_CMD:
        if (s->inval_pending) {                 /* finish the older one */
            svmd_inval_timer(s);
        }
        if (s->inject & SVMD_INJECT_SLOW_INVAL) {
            s->inval_pending = true;
            s->pend_va = s->inval_va;
            s->pend_size = s->inval_size;
            s->pend_seqno = val;
            timer_mod(s->inval_timer, qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) +
                      s->inval_delay_us);
        } else {
            svmd_inval_apply(s, s->inval_va, s->inval_size, val);
        }
        break;
    case SVMD_REG_FAULTQ_BASE:
        s->faultq_base = val;
        break;
    case SVMD_REG_FAULTQ_SIZE:
        if (!svmd_pow2(val)) {
            svmd_engine_error(s, SVMD_ERR_RING);
            break;
        }
        s->faultq_size = val;
        s->faultq_head = s->faultq_tail = 0;
        break;
    case SVMD_REG_FAULTQ_HEAD:
        /* the driver consumed records: room for a fault that's waiting */
        s->faultq_head = val % (s->faultq_size ?: 1);
        if (s->report_pending) {
            svmd_report_fault(s);
        }
        break;
    case SVMD_REG_FAULT_RESUME:
        if (!s->stalled) {
            break;                              /* stale or duplicate resume */
        }
        s->stalled = false;
        if (val == SVMD_RESUME_ABORT) {
            s->aborting = true;
        }
        svmd_engine_kick(s);
        break;
    case SVMD_REG_CRING_BASE:
        s->cring_base = val;
        break;
    case SVMD_REG_CRING_SIZE:
        if (!svmd_pow2(val) || s->cring_head != s->cring_tail) {
            svmd_copy_error(s, SVMD_ERR_RING);
            break;
        }
        s->cring_size = val;
        s->cring_head = s->cring_tail = 0;
        break;
    case SVMD_REG_CRING_TAIL:
        if (val >= s->cring_size) {
            svmd_copy_error(s, SVMD_ERR_RING);
            break;
        }
        s->cring_tail = val;
        qemu_bh_schedule(s->copy_bh);
        break;
    case SVMD_REG_INJECT:
        s->inject = val & ~SVMD_INJECT_SPURIOUS;
        if (val & SVMD_INJECT_SPURIOUS) {
            svmd_raise(s, SVMD_IRQ_FAULT);      /* nothing in the queue */
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "svmd: write of unimplemented reg 0x%03"
                      HWADDR_PRIx " = 0x%" PRIx64 "\n", addr, val);
        break;
    }
}

static const MemoryRegionOps svmd_regs_ops = {
    .read = svmd_read,
    .write = svmd_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /* §2: 32-bit registers, and 64-bit ones read/written in one access */
    .valid = { .min_access_size = 4, .max_access_size = 8 },
    .impl  = { .min_access_size = 4, .max_access_size = 8 },
};

static void svmd_reset(DeviceState *dev)
{
    SvmdState *s = SVMD(dev);

    s->scratch = 0;
    s->irq_status = 0;
    s->irq_mask = 0;
    s->inject = 0;
    s->stat_bytes = s->stat_faults = s->stat_invals = 0;
    s->stat_tlb_hits = s->stat_tlb_misses = 0;
    svmd_engine_reset(s);
}

/* "Plugging the card in": set up BARs and interrupts. */
static void svmd_realize(PCIDevice *pdev, Error **errp)
{
    SvmdState *s = SVMD(pdev);

    if (!svmd_pow2(s->vram_size)) {
        error_setg(errp, "svmd: vram_size must be a power of 2");
        return;
    }

    /*
     * The guarded bottom half: if the engine's DMA ever hits our own
     * registers, QEMU refuses instead of recursing into svmd_write.
     */
    s->engine_bh = qemu_bh_new_guarded(svmd_engine_run, s,
                                       &DEVICE(pdev)->mem_reentrancy_guard);
    s->copy_bh = qemu_bh_new_guarded(svmd_copy_run, s,
                                     &DEVICE(pdev)->mem_reentrancy_guard);
    s->inval_timer = timer_new_us(QEMU_CLOCK_VIRTUAL, svmd_inval_timer, s);

    memory_region_init_io(&s->regs, OBJECT(s), &svmd_regs_ops, s,
                          "svmd-regs", SVMD_REGS_SIZE);
    pci_register_bar(pdev, SVMD_BAR_REGS, PCI_BASE_ADDRESS_SPACE_MEMORY,
                     &s->regs);

    if (!memory_region_init_ram(&s->vram, OBJECT(s), "svmd-vram",
                                s->vram_size, errp)) {
        return;
    }
    pci_register_bar(pdev, SVMD_BAR_VRAM, PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64 |
                     PCI_BASE_ADDRESS_MEM_PREFETCH, &s->vram);

    if (msix_init_exclusive_bar(pdev, SVMD_NUM_VECTORS, SVMD_BAR_MSIX, errp)) {
        return;
    }
    /*
     * msix_notify() silently drops vectors not marked "in use" - forgetting
     * this makes every interrupt vanish with no error anywhere.
     */
    for (unsigned v = 0; v < SVMD_NUM_VECTORS; v++) {
        msix_vector_use(pdev, v);
    }
}

static void svmd_exit(PCIDevice *pdev)
{
    SvmdState *s = SVMD(pdev);

    qemu_bh_delete(s->engine_bh);
    qemu_bh_delete(s->copy_bh);
    timer_free(s->inval_timer);
    msix_uninit_exclusive_bar(pdev);
}

static const Property svmd_props[] = {
    DEFINE_PROP_SIZE("vram_size", SvmdState, vram_size, 256 * MiB),
    DEFINE_PROP_UINT32("inval_delay_us", SvmdState, inval_delay_us, 2000),
};

/* The device's ID card: what Linux sees when it scans the PCI bus. */
static void svmd_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = svmd_realize;
    k->exit = svmd_exit;
    k->vendor_id = SVMD_PCI_VENDOR_ID;
    k->device_id = SVMD_PCI_DEVICE_ID;
    k->class_id = SVMD_PCI_CLASS;
    k->revision = SVMD_VERSION_1;
    dc->desc = "SVMD shared-virtual-memory toy GPU";
    device_class_set_legacy_reset(dc, svmd_reset);
    device_class_set_props(dc, svmd_props);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo svmd_types[] = {
    {
        .name          = TYPE_SVMD,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(SvmdState),
        .class_init    = svmd_class_init,
        .interfaces    = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    },
};

DEFINE_TYPES(svmd_types)
