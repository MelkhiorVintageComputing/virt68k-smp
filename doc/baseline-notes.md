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

## NetBSD 11.0 on QEMU 11.1.2 (stock)

- The official sets were turned into a live image with
  `distrib/virt68k/liveimage/emuimage` (`nbmake-virt68k live_image`, see
  `scripts/netbsd-run.py` for how it is booted: `-kernel netbsd -append root=ld0a`,
  virtio-blk-device).
- It reaches the login prompt in about 7 s on stock QEMU, and on our QEMU with `-smp 1` and `-smp 4`.
  With `-smp 4`, the stock kernel prints "mainbus0: unknown bootinfo tag: 0x00008007" and
  counts it as 1 error while detecting hardware.
- A self-built GENERIC kernel (build.sh on Linux, gcc 12.5) boots the same image.
- **Known stock issue**: with 1000M of RAM or more (896M works), the stock kernel on stock QEMU
  panics very early ("MMU fault", bad kernel read at 0x4 in `rw_vector_enter`, before
  the copyright banner). This is a NetBSD-side limit, not related to SMP.
  Use `-m 512M` (the default in `netbsd-run.py`).
  - Cause: the kernel segment table is sized by `MAXKL2SIZE` (`m68k/include/pmap_motorola.h`).
    The value 32 gives 4 pages, i.e. 32 level-2 blocks of 32 MB of KVA each, which runs out at
    about 1 GB of RAM.
  - Workaround (2026-10-25, not applied to our branches): `#define MAXKL2SIZE 128`. On the
    pristine 11.0 sources (UP GENERIC, stock QEMU), this boots and passes a memory-touching
    stress at 896M, 1G, 1.5G, 2G and 3G. With 32, every size from 1G up panics.
  - Caveats:
    - `stfree`/`pm_stfree` is a 32-bit bitmap, so blocks 32 and up are not tracked.
      `pmap_bootstrap` then computes `1U << i` with i >= 32 (harmless on m68k, but undefined
      in C).
    - The header is shared by all m68k ports.
    - `hw.physmem` reads negative from 2G up (use `hw.physmem64`).
    - Separately, a single process using roughly 150 MB of scattered address space panics with
      "enter: out of address space", whatever the RAM size (the `MAXUL2SIZE` user limit).

## Networking (QEMU built with libslirp 4.8.0)

- `scripts/netbsd-run.py --net` adds `-netdev user,id=n0 -device virtio-net-device,netdev=n0`;
  NetBSD attaches it as vioif0.
- Tested with GENERIC (`-smp 1`) and GENERIC.MP (`-smp 4`):
  - Static config and DHCP (`dhcpcd -1 -w vioif0`) both work: 10.0.2.15, gw 10.0.2.2,
    DNS 10.0.2.3.
  - Ping works, and DNS resolves external names.
  - An HTTP download of the 11.0 kernel (1.7 MB, 3 s) has the right SHA512.
- HTTPS fails with "certificate verify failed" on a fresh live image: `/etc/openssl/certs` is
  empty, because the image build does not run `certctl rehash`. After `certctl rehash`,
  HTTPS works (done once in `images/netbsd-11.0-mp-live.img`).
- Plain `dhcpcd -w` daemonizes and keeps the console attached, which hangs the scripted console;
  use `-1` (one shot) in scripts.
