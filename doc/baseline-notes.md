# Baseline notes (QEMU 11.1.2, NetBSD 11.0)

## QEMU `virt` m68k machine (hw/m68k/virt.c @ v11.1.2)

Memory map (all MMIO in the top 16 MiB):

| Base         | Size    | Device                                   | IRQ                         |
|--------------|---------|------------------------------------------|-----------------------------|
| 0x00000000   | RAM     | RAM (max ~3.2 GiB)                        |                             |
| 0xff000000   | 6×4 KiB | goldfish-pic #1..#6 → CPU IPL 1..6        | (via m68k-irqc)             |
| 0xff006000   | 2×4 KiB | goldfish-rtc ×2 (2nd one = timer)         | PIC6 irq1, PIC6 irq2        |
| 0xff008000   | 4 KiB   | goldfish-tty                              | PIC1 irq32                  |
| 0xff009000   | 4 KiB   | virt-ctrl (reset/halt/panic)              | PIC1 irq1                   |
| 0xff00a000   | —       | **free** up to 0xff00ffff                 |                             |
| 0xff010000   | 128×0x200 | virtio-mmio                             | PIC2..5, all irqs           |

- IRQ numbering in bootinfo: `PIC_IRQ(n, i) = 8 + (n-1)*32 + i - 1`.
- `m68k-irq-controller` (hw/intc/m68k_irqc.c): 7 GPIO inputs (IPL 1..7), keeps an `ipr`
  bitmap and calls `m68k_set_irq_level(cpu, highest, 24+level)` (autovector). Bound to
  **one** CPU through the `m68k-cpu` link. For SMP: one irqc per CPU.
- `mc->max_cpus = 1`. A single CPU is created; `main_cpu_reset` does cpu_reset, sets PC = ELF
  entry, A7 = `initial_stack`, which is **never set (0)**, so the guest must set its own SP.
- The kernel is loaded as ELF. Bootinfo starts at `(highest_loaded_addr + 1) & ~1`: BI_MACHTYPE,
  CPU/FPU/MMU type, MEMCHUNK, BI_VIRT_QEMU_VERSION, GF_PIC/RTC/TTY, CTRL, VIRTIO base+irq,
  COMMAND_LINE, RNG_SEED, RAMDISK, LAST.
- The initrd is loaded page-aligned at the top of RAM.

## target/m68k MTTCG readiness

- `m68k_tcg_ops` (target/m68k/cpu.c:633): `guest_default_memory_order = TCG_MO_ALL`,
  `mttcg_supported = false`. Flipping it is the switch.
- TAS: `tcg_gen_atomic_fetch_or_tl`, OK. CAS: `tcg_gen_atomic_cmpxchg_i32`, OK.
  CAS2.W: `gen_helper_exit_atomic` under CF_PARALLEL (serialized, OK). CAS2.L: parallel helper using
  a 64-bit cmpxchg when the operands are adjacent, else `cpu_loop_exit_atomic`, OK.
- **To fix**: 68040 table walk (target/m68k/helper.c ~772-866) sets U/M bits with plain
  `address_space_ldl` + `address_space_stl`, so a concurrent update by another vCPU (or by the guest
  clearing M) can be lost. Use an atomic OR / cmpxchg on host RAM.
- `m68k_set_irq_level` (helper.c:943) writes pending_level/vector and calls cpu_interrupt. It is
  called from devices under the BQL, which is fine when there is one irqc per CPU.
