# virt68k-smp

This project adds SMP to QEMU's m68k `virt` machine (MTTCG, one host thread per vCPU) and adds
MULTIPROCESSOR support to NetBSD/virt68k.

Targets: QEMU 11.1.2 and NetBSD 11.0.

## Layout

| Path | Content |
|------|---------|
| `Inputs/` | Pristine, add-only originals: the QEMU git tree at v11.1.2, the signed QEMU tarball, and the NetBSD 11.0 release and source sets. Read-only; only `Inputs/MANIFEST` is tracked. |
| `work/qemu` | QEMU work tree, branch `virt-smp` (on top of v11.1.2). |
| `work/qemu-stock` | Pristine v11.1.2 worktree, kept for comparison runs. |
| `work/netbsd-src` | NetBSD 11.0 sources (from the source sets), branch `virt68k-mp`. |
| `work/{obj,tools,destdir,release}` | NetBSD build.sh output. |
| `images/` | Kernels and disk images we produce. |
| `doc/virt-smp-spec.md` | Specification of the paravirtual SMP controller (QEMU's copy: `docs/specs/virt-smp.rst`). |
| `doc/pmap-perf.md` | MP pmap performance work: tools, design changes, measurements. |
| `doc/baseline-notes.md` | Notes on the stock QEMU virt machine and its MTTCG readiness. |
| `baremetal/` | Freestanding UP (`t*`) and SMP (`s*`) test programs. |
| `scripts/` | `build-qemu.sh`, `build-kernel.sh`, `run-baremetal.sh`; `netbsd-run.py` (boot, log in, run commands; `--monitor` and `--ddb` for hangs); `qemu-regs.py`; guest-side `soak.sh`, `memstress.c`, `pmap-bench.{sh,c}`, `tlbtest.c`, `selfhost-kernel.sh`. |
| `patches/qemu/` | `git format-patch` export of the QEMU series. |
| `patches/netbsd/` | `git format-patch` export of the NetBSD series (branch `virt68k-mp`, against the 11.0 import). |

## Quick start

```sh
scripts/build-qemu.sh                                     # builds work/qemu/build/qemu-system-m68k
scripts/run-baremetal.sh                                  # UP tests, -smp 1, single-threaded TCG
scripts/run-baremetal.sh -s "1 2 4 8 16" -t "single multi"   # full matrix
scripts/netbsd-run.py --smp 4 --net --kernel work/obj/sys/arch/virt68k/compile/GENERIC.MP/netbsd \
    --image images/netbsd-11.0-mp-live.img -c 'dhcpcd -1 -w vioif0' -c 'ping -c 3 10.0.2.2'
QEMU=work/qemu-stock/build/qemu-system-m68k scripts/run-baremetal.sh 't*'   # stock QEMU
```

Each test prints `PASS` or `FAIL` and powers the machine off through virt-ctrl. Logs are written to
`baremetal/build/logs/`.

## QEMU series (patches/qemu)

1. target/m68k: set the M bit on the first write to a page loaded by a read. This is a pre-existing UP
   bug, found by `t4-mmu`.
2. target/m68k: set table descriptor U/M bits atomically. This is an MTTCG race; `s6-mmu` catches it.
3. target/m68k: enable MTTCG.
4. standard-headers: add `BI_VIRT_SMP_BASE`. This is a placeholder: the Linux uapi header has to change first.
5. hw/misc: add the virt-smp device.
6. hw/m68k/virt: support up to 16 CPUs. With `-smp 1` the machine is unchanged.

## NetBSD series (patches/netbsd)

The series is against the NetBSD 11.0 sources (the pristine import, commit 2ade45572). It applies
in order with `git am`.

1. m68k: use a user-reserved PTE bit (UR, bit 11) for the software wired flag `PG_W`, instead of
   U0, which the 68040/060 drive on the UPA0 pin (snoop control on MP boards). This is a UP
   change, and a prerequisite for MP. See doc/pgw-ur.md.
2. atari: name the U0 page attribute that the Milan uses (cosmetic).
3. m68k, virt68k: per-CPU state for MULTIPROCESSOR (step N1).
4. virt68k: virtsmp(4), secondary CPU start, IPIs, and an MP-safe pmap (steps N2–N5).
5. m68k atomics: the void `atomic_*_32` operations become CAS loops (MP-safe).
6. virt68k: attach virtsmp(4), and so the CPUs, before the other devices.
7. m68k pmap (MP): targeted and batched TLB shootdowns.
8. m68k pmap (MP): no broadcast shootdowns for user PT pages.
9. m68k pmap (MP): per-pmap locks instead of the kernel lock.
10. m68k pmap (MP): clear the wired bit atomically in `pmap_unwire`.

Patches 1–2 work alone on a uniprocessor kernel. 3–10 add MULTIPROCESSOR; see
doc/netbsd-mp-design.md and doc/pmap-perf.md.
