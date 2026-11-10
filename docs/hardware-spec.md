# SVMD — hardware manual (v0.1)

SVMD ("shared virtual memory device") is a made-up PCI device that we
implement in QEMU and write a Linux driver for. It does very little work —
copy, fill, add — but its **memory system** behaves like a modern GPU's:
its own page tables, a translation cache (TLB), replayable page faults, and
its own memory (VRAM).

Each section says which project phase first needs it, and **why** it is
designed that way. Everything is little-endian.

---

## 1. PCI identity (phase 1)

| field | value |
|---|---|
| vendor ID | `0x1234` (QEMU's "experimental device" vendor) |
| device ID | `0x5356` ("SV") |
| class | `0x1200` (processing accelerator) |

| BAR | contents | size |
|---|---|---|
| BAR0 | registers (section 2) | 4 KiB |
| BAR2 | device memory, "VRAM" (section 7), 64-bit, prefetchable | 256 MiB (QEMU property `vram_size`) |
| BAR4 | MSI-X table (QEMU manages this) | — |

**Why:** a BAR ("base address register") is how a PCI device tells the
computer "I have this much memory/registers; put them somewhere in the
address space". BAR0 is small and uncached because every read/write is a
command to the device. BAR2 is big and prefetchable because it is plain
memory.

### Interrupts (MSI-X vectors)

| vector | meaning |
|---|---|
| 0 | job progress (a FENCE command asked for an interrupt) |
| 1 | page fault queued (section 6) |
| 2 | error (bad command, bad page-table entry, fault-queue overflow) |

**Why separate vectors:** the fault handler and the completion handler run
different code at different priority. With one vector the driver would
have to read a status register first to know which work to do.

---

## 2. Registers (BAR0)

`R` = read-only, `W` = write-only, `RW`, `RW1C` = write 1 to clear a bit.
64-bit registers must be accessed with a single 64-bit access.

| offset | name | size | access | phase | meaning |
|---|---|---|---|---|---|
| `0x000` | `ID` | 32 | R | 1 | always `0x53564D44` ("SVMD") |
| `0x004` | `VERSION` | 32 | R | 1 | `0x00000001` |
| `0x008` | `CAPS` | 32 | R | 1 | bit0 MMU, bit1 FAULTS, bit2 VRAM |
| `0x00C` | `SCRATCH` | 32 | RW | 1 | holds whatever was written; for testing |
| `0x010` | `STATUS` | 32 | R | 2 | bit0 BUSY, bit1 STALLED (waiting on a fault), bit2 ERROR |
| `0x014` | `CONTROL` | 32 | RW | 2 | bit0 ENGINE_ENABLE, bit1 RESET (self-clearing) |
| `0x018` | `ERROR_CODE` | 32 | R | 2 | section 8 |
| `0x020` | `IRQ_STATUS` | 32 | RW1C | 1 | bit0 JOB, bit1 FAULT, bit2 ERROR |
| `0x024` | `IRQ_MASK` | 32 | RW | 1 | 1 = interrupt enabled |
| `0x028` | `IRQ_RAISE` | 32 | W | 1 | test hook: raise the interrupts for the bits written |
| **ring** | | | | | |
| `0x040` | `RING_BASE` | 64 | RW | 2 | DMA address of the command ring |
| `0x048` | `RING_SIZE` | 32 | RW | 2 | number of 32-byte slots, a power of 2 |
| `0x04C` | `RING_HEAD` | 32 | R | 2 | next slot the device will read |
| `0x050` | `RING_TAIL` | 32 | RW | 2 | **doorbell**: one past the last slot the driver filled |
| `0x058` | `FENCE_ADDR` | 64 | RW | 2 | DMA address the device writes completed seqnos to |
| `0x060` | `FENCE_VALUE` | 32 | R | 2 | last completed seqno (same value, readable here) |
| **MMU** | | | | | |
| `0x080` | `PT_ROOT` | 64 | RW | 3 | DMA address of the top-level page table |
| `0x088` | `MMU_CONTROL` | 32 | RW | 3 | bit0 MMU_ENABLE |
| `0x090` | `INVAL_VA` | 64 | RW | 3 | start of the range to invalidate |
| `0x098` | `INVAL_SIZE` | 64 | RW | 3 | length in bytes; `0` = everything |
| `0x0A0` | `INVAL_CMD` | 32 | W | 3 | write a seqno to start an invalidation |
| `0x0A4` | `INVAL_DONE` | 32 | R | 3 | seqno of the last finished invalidation |
| **faults** | | | | | |
| `0x0C0` | `FAULTQ_BASE` | 64 | RW | 4 | DMA address of the fault queue |
| `0x0C8` | `FAULTQ_SIZE` | 32 | RW | 4 | number of 32-byte records, a power of 2 |
| `0x0CC` | `FAULTQ_HEAD` | 32 | RW | 4 | driver: next record it will read |
| `0x0D0` | `FAULTQ_TAIL` | 32 | R | 4 | device: next record it will write |
| `0x0D4` | `FAULT_RESUME` | 32 | W | 4 | `1` = retry the stalled access, `2` = abort the job |
| **testing** | | | | | |
| `0x100` | `INJECT` | 32 | RW | 7 | section 9 |
| `0x140` | `STAT_FAULTS` | 64 | R | 4 | faults reported |
| `0x148` | `STAT_TLB_HITS` | 64 | R | 3 | |
| `0x150` | `STAT_TLB_MISSES` | 64 | R | 3 | |
| `0x158` | `STAT_BYTES` | 64 | R | 2 | bytes the engine read + wrote |
| `0x160` | `STAT_INVALS` | 64 | R | 3 | invalidations completed |

**Why `IRQ_STATUS` is write-1-to-clear:** the driver reads the status,
handles bits 0 and 1, and writes back exactly `0b011`. If a new fault
arrived in between, its bit is set again and is not lost — writing 0s
never clears anything.

**Why invalidation takes a seqno (`INVAL_CMD` / `INVAL_DONE`):** an
invalidation is not instant — the device must finish in-flight accesses
that used the old translation. The driver starts it, then waits until
`INVAL_DONE` reaches its seqno. That wait is the "wait until the device
confirms" step; skipping it is a real bug class, and section 9 can make
the device slow on purpose to expose it.

---

## 3. The command ring (phase 2)

The ring is an array of `RING_SIZE` 32-byte slots in system memory. The
driver is the only writer of slots; the device is the only reader.

```
          RING_HEAD (device reads here)      RING_TAIL (driver writes here)
              v                                  v
   [ done | done | queued | queued | queued | empty | empty | done ]
```

To submit:
1. Write commands into slots `TAIL, TAIL+1, ...` (indices wrap mod `RING_SIZE`).
2. **Memory barrier** — make the slot contents visible before step 3.
3. Write the new tail to `RING_TAIL` (the doorbell).

The ring is empty when `HEAD == TAIL`. The driver must never let `TAIL`
catch up with `HEAD` from behind, so at most `RING_SIZE - 1` slots are in
use.

**Why a ring and a doorbell:** writing a register is slow (it crosses to
the device), writing memory is cheap. The driver can queue many commands
with plain memory writes and ring the doorbell once. Every real GPU works
this way. **Why the barrier:** without it the CPU or compiler may reorder
the writes, so the device sees the new tail before the command it points
to — and executes garbage.

---

## 4. Commands (phase 2)

Every command is 32 bytes:

```
offset  size  field
0       1     opcode
1       1     flags     bit0 IRQ (FENCE only), bit1 PHYS (section 5)
2       2     reserved, must be 0
4       4     arg32     (meaning depends on opcode)
8       8     dst
16      8     src
24      8     len       (bytes)
```

| opcode | name | effect |
|---|---|---|
| `0x00` | `NOP` | nothing |
| `0x01` | `FILL` | write the 32-bit `arg32` repeatedly over `dst[0..len)` |
| `0x02` | `COPY` | copy `len` bytes `src` → `dst` (must not overlap) |
| `0x03` | `ADD` | for each 32-bit word in `dst[0..len)`: `word += arg32` |
| `0x04` | `FENCE` | write seqno `arg32` to `FENCE_ADDR` and `FENCE_VALUE`; if flags.IRQ, raise vector 0 |
| `0x05` | `SET_ROOT` | (kernel only) use the page table at DMA address `dst` from now on, and flush the TLB |

`flags` bits 2 and 3 (`DST_VRAM`, `SRC_VRAM`) only matter with `PHYS`: they
say `dst` / `src` is an offset into VRAM (BAR2) instead of a DMA address.
The kernel uses them to migrate pages between system memory and VRAM.

**Why `SET_ROOT` is a command, not just the `PT_ROOT` register:** jobs from
different processes sit in the same ring. Each process has its own page
table, so the switch has to happen *between* jobs, in ring order - only a
command can do that. (`PT_ROOT` reads back the current root.)

`len` must be a multiple of 4 for `FILL` and `ADD`. Commands run strictly
in order.

**Why `ADD`:** it is *not* idempotent — doing it twice gives the wrong
answer. That forces the device to resume a faulted command exactly where
it stopped (section 6), instead of restarting it. Real GPUs face the same
problem with atomics.

**Why `FENCE`:** the driver needs to know when work is done without
polling. A fence is a number the device writes when it reaches that
point in the ring; the kernel's `dma_fence` objects are built on this.

---

## 5. Addresses and the MMU (phase 3)

With `MMU_ENABLE` off, `dst`/`src` are DMA addresses (phase 2).

With it on, they are **device-virtual addresses** (VAs), translated by the
device's page tables. For SVM the driver keeps these tables in sync with a
process's CPU page tables, so a pointer from `malloc()` means the same
thing to both.

The `PHYS` flag bypasses translation for one command. Only the kernel
driver may set it (it uses it to copy pages during migration); the driver
must reject user commands that set it.

### Page-table format

48-bit VAs, 4 KiB pages, 4 levels of 512 eight-byte entries — the same
shape as x86-64 and arm64, so you can compare with the CPU's:

```
 47      39 38      30 29      21 20      12 11         0
[ L0 index ][ L1 index ][ L2 index ][ L3 index ][ offset   ]
```

Each table is one 4 KiB page. An entry:

| bits | field |
|---|---|
| 0 | `VALID` |
| 1 | `WRITE` (0 = read-only) |
| 2 | `VRAM` — address is an offset into BAR2 memory, not a DMA address (leaf only, phase 6) |
| 3–11 | reserved, must be 0 |
| 12–51 | address of the next table, or of the 4 KiB page (leaf) |
| 52–63 | reserved, must be 0 |

Levels 0–2 point to tables, level 3 points to pages. (No huge pages in v1.)

### TLB

The device caches up to 16 recent translations. It is **not** kept
coherent with memory: changing a page-table entry has no effect on a
cached translation until the driver invalidates it (section 2). This is
deliberate — it is how real GPUs behave, and it makes the
stale-translation bug from the design notes reproducible.

---

## 6. Faults (phase 4)

A **fault** happens when the engine accesses a VA whose entry is not
`VALID` at some level, or writes to a page without `WRITE`.

Instead of failing, the device:

1. **Stalls** the engine at that exact access (`STATUS.STALLED = 1`),
   remembering how far into the current command it got.
2. Writes a fault record at `FAULTQ_TAIL` and advances the tail.
3. Raises vector 1.
4. Waits for the driver to write `FAULT_RESUME`:
   - `1` (retry): re-translate the faulting access and continue the
     command from where it stopped. If it faults again, go to step 1.
   - `2` (abort): skip the rest of the job up to the next `FENCE`, run
     that `FENCE` normally, and continue with the next job. The device
     doesn't know about jobs' errors - the driver, which chose to abort,
     marks the job as failed itself. The engine keeps running.

Fault record (32 bytes):

```
offset  size  field
0       8     va         faulting address, rounded down to 4 KiB
8       4     access     bit0 WRITE, bit1 READ
12      4     reason     1 = not present, 2 = write to read-only, 3 = bad entry
16      4     ring_slot  which command faulted
20      4     level      page-table level where the walk stopped (0–3)
24      8     root       page table in use (tells the driver which process)
```

If the queue is full, the device stalls until the driver frees a slot and
sets `ERROR` only if the driver also stops making progress (it never
drops a fault).

**Why "replayable" faults:** the driver can only fix a fault by sleeping
(reading CPU page tables, maybe allocating memory). The device can't wait
inside an interrupt, so it parks the work, reports, and is told to
retry. This is the model `drm_gpusvm` is built for.

---

## 7. Device memory, "VRAM" (phase 6)

BAR2 is the device's own memory. A leaf entry with `VRAM` set means
"this page lives at offset `address` in BAR2". The engine accesses VRAM
much faster than system memory (QEMU models this with an optional delay
on system-memory accesses, property `sysmem_delay_ns`).

The driver moves pages between system memory and VRAM with `COPY` +
`PHYS`. When the CPU touches a page that lives in VRAM, the kernel calls
the driver to move it back first.

---

## 8. Error codes (`ERROR_CODE`)

| code | meaning |
|---|---|
| 0 | none |
| 1 | unknown opcode |
| 2 | misaligned length |
| 3 | reserved bits set in a page-table entry |
| 4 | DMA error (address outside RAM / BAR2) |
| 5 | job aborted by `FAULT_RESUME = 2` |
| 6 | bad ring setup: `RING_SIZE` not a power of 2 or changed while busy, or `RING_TAIL` ≥ `RING_SIZE` |

On any error the engine **stops** (`STATUS.ERROR`), raises vector 2, and
runs nothing more until the driver writes `CONTROL.RESET` and sets the ring
up again. Commands after a bad one can't be trusted to mean what the driver
intended, so the device doesn't guess.

---

## 9. Fault injection (`INJECT`, phase 7)

Switches that make the device misbehave in ways real hardware can, so the
driver's error paths actually get exercised:

| bit | effect |
|---|---|
| 0 | report every fault twice (driver must cope with duplicates) |
| 1 | delay invalidation completion by `inval_delay_us` |
| 2 | spurious fault-interrupt with an empty queue (self-clearing) |
| 3 | ignore the TLB (always walk) — to compare against bug reports |

---

## 10. The copy engine (phase 6)

A second engine with its own ring, for the kernel to move data between
system memory and VRAM:

| offset | name | size | access | meaning |
|---|---|---|---|---|
| `0x180` | `CRING_BASE` | 64 | RW | DMA address of the copy ring |
| `0x188` | `CRING_SIZE` | 32 | RW | slots, a power of 2 |
| `0x18C` | `CRING_HEAD` | 32 | R | next slot it will read |
| `0x190` | `CRING_TAIL` | 32 | RW | doorbell |
| `0x198` | `CFENCE_VALUE` | 32 | R | last `FENCE` seqno it reached |
| `0x19C` | `CERROR` | 32 | R | error code (§8); non-zero stops it |

Same 32-byte commands as §4, but only `COPY`, `FILL` and `FENCE`, and
every `COPY`/`FILL` must have `PHYS` (with `DST_VRAM`/`SRC_VRAM` as
needed). It has no MMU, so it **never faults**. `FENCE` here only updates
`CFENCE_VALUE` (no memory write, no interrupt): the driver polls.

**Why a second engine:** migrating pages into VRAM happens *while
resolving a fault* - that is, while the main engine is stalled waiting
for that very fault. A copy queued on the main ring would sit behind the
stalled job forever: deadlock. Real GPUs have separate copy engines
for this (and more). `CONTROL.RESET` resets both engines.
